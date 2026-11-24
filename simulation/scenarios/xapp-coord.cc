/**
 * xapp-coord.cc -- CMPS 342 project scenario (Phase 1: base scenario + per-UE throughput)
 *
 * Derived from the NIST ns3-oran example
 * "oran-lte-2-lte-distance-handover-lm-processing-delay-example.cc".
 * Modified Nov 2026 by H. Zoueihed and N. Sakr: stationary multi-UE two-cell layout,
 * downlink CBR/UDP traffic from a remote host, per-UE throughput logging to CSV,
 * command-line control of load, layout, RIC timing and output files.
 * Original NIST notice: software provided by NIST as a public service, "AS IS",
 * without warranty. Acknowledgement: National Institute of Standards and Technology.
 *
 * Layout (top view, eNB A = cell 1 at x=0, eNB B = cell 2 at x=isd):
 *
 *      near-A UEs (circle r = near-frac*isd)   edge UEs (x = edge-frac*isd)   near-B UEs
 *              (A) ------------------------------- | ----------------------------- (B)
 *
 *   near-A and edge UEs attach to cell A (the cell whose load we raise),
 *   near-B UEs attach to cell B.
 *
 * Outputs:
 *   <out>-tput.csv : one row per UE per window
 *                    time_s,ue,imsi,group,cell_id,rx_bytes,tput_mbps,offered_mbps
 *   <out>-ues.csv  : one row per UE: ue,imsi,group,x,y,initial_cell,offered_mbps
 *   <out>-ho.csv   : one row per completed handover: time_s,imsi,target_cell
 *
 * S1 additions (2026-11-22). With default arguments every file above is byte-identical to
 * the S0 baseline (enforced by scripts/validation/s0-regression.sh); new behaviour is opt-in:
 *   --layout-file=F      fixed UE layout from CSV (ue,group,x,y,z,initial_cell); no jitter
 *   --assign-streams=B   explicit RNG streams (B >= 0) for every exogenous random process
 *   --lm=noop            RIC with a do-nothing LM (matched-run control)
 *   --acct-guard=T       loss accounting excludes packets sent in the last T
 *   --trace-prb-subframes  raw per-frame PRB and MAC scheduling traces
 * Passive measurement outputs (simulation/metrics/xapp-metrics.h) are always written,
 * plus <out>-config.json with every resolved parameter and the RNG stream map.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/oran-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/propagation-module.h"

#include "cmd-lifecycle.h"
#include "oran-cmm-coord.h"
#include "oran-cmm-noop-logged.h"
#include "oran-command-lte-tx-power.h"
#include "oran-lm-scripted-commands.h"
#include "oran-lm-xapps.h"
#include "b5-policy.h"
#include "rate-estimator.h"
#include "ric-telemetry-store.h"
#include "oran-e2-node-terminator-lte-enb-tpc.h"
#include "oran-lm-tx-power-step.h"
#include "oran-query-trigger-telemetry-sink.h"
#include "oran-report-lte-enb-load.h"
#include "oran-reporter-lte-enb-load.h"
#include "oran-reporter-lte-ue-service.h"
#include "link-shadowing-loss-model.h"
#include "xapp-metrics.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <chrono>
#include <map>
#include <sstream>
#include <stdio.h>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("XappCoord");

namespace
{
NetDeviceContainer g_ueDevs;
std::vector<uint64_t> g_winBytes; // bytes received by each UE in the current window
std::vector<uint64_t> g_totBytes; // bytes received by each UE after warm-up
std::vector<double> g_offeredMbps;
std::vector<std::string> g_group;
std::ofstream g_tputCsv;
std::ofstream g_hoCsv;
std::ofstream g_powerCsv;
Time g_statsStart;
} // namespace

void
RxPacket(uint32_t ue, Ptr<const Packet> packet)
{
    g_winBytes[ue] += packet->GetSize();
    if (Simulator::Now() >= g_statsStart)
    {
        g_totBytes[ue] += packet->GetSize();
    }
}

uint16_t
ServingCell(uint32_t ue)
{
    return g_ueDevs.Get(ue)->GetObject<LteUeNetDevice>()->GetRrc()->GetCellId();
}

uint64_t
Imsi(uint32_t ue)
{
    return g_ueDevs.Get(ue)->GetObject<LteUeNetDevice>()->GetImsi();
}

void
SampleThroughput(Time window)
{
    double now = Simulator::Now().GetSeconds();
    for (uint32_t i = 0; i < g_ueDevs.GetN(); i++)
    {
        double mbps = g_winBytes[i] * 8.0 / window.GetSeconds() / 1e6;
        g_tputCsv << std::fixed << std::setprecision(3) << now << "," << i << "," << Imsi(i)
                  << "," << g_group[i] << "," << ServingCell(i) << "," << g_winBytes[i] << ","
                  << std::setprecision(4) << mbps << "," << g_offeredMbps[i] << "\n";
        g_winBytes[i] = 0;
    }
    std::vector<uint16_t> serving(g_ueDevs.GetN());
    for (uint32_t i = 0; i < g_ueDevs.GetN(); i++)
    {
        serving[i] = ServingCell(i);
    }
    xapp::MetricsSampleWindow(serving);
    Simulator::Schedule(window, &SampleThroughput, window);
}

void
NotifyHandoverEndOkEnb(std::string context, uint64_t imsi, uint16_t cellid, uint16_t rnti)
{
    std::cout << Simulator::Now().As(Time::S) << " eNB CellId " << cellid
              << ": completed handover of UE with IMSI " << imsi << std::endl;
    g_hoCsv << std::fixed << std::setprecision(4) << Simulator::Now().GetSeconds() << "," << imsi
            << "," << cellid << "\n";
}

void
LogPowerChange(uint16_t cell, double oldDbm, double newDbm, const std::string& source)
{
    std::cout << Simulator::Now().As(Time::S) << " cell " << cell << " TxPower " << oldDbm
              << " -> " << newDbm << " dBm (" << source << ")" << std::endl;
    g_powerCsv << std::fixed << std::setprecision(4) << Simulator::Now().GetSeconds() << ","
               << cell << "," << oldDbm << "," << newDbm << "," << source << "\n";
}

// Scripted power step (no RIC)
void
SetEnbTxPower(Ptr<LteEnbNetDevice> enb, uint16_t cell, double dbm)
{
    double oldDbm = enb->GetPhy()->GetTxPower();
    enb->GetPhy()->SetTxPower(dbm);
    LogPowerChange(cell, oldDbm, dbm, "script");
    xapp::MetricsCtrlEvent("script_power_set", cell, -1, -1, -1, NAN, dbm, oldDbm, dbm, "applied");
}

// Power change executed by the eNB E2 terminator after a RIC command
void
RicPowerChange(uint16_t cell, double oldDbm, double newDbm)
{
    LogPowerChange(cell, oldDbm, newDbm, "ric");
}

void
QueryRcSink(std::string query, std::string args, int rc)
{
    std::cout << Simulator::Now().GetSeconds() << " Query "
              << ((rc == SQLITE_OK || rc == SQLITE_DONE) ? "OK" : "ERROR") << "(" << rc << "): \""
              << query << "\"";
    if (!args.empty())
    {
        std::cout << " (" << args << ")";
    }
    std::cout << std::endl;
}

/* S5 prep: UE PHY measurement -> ns3-oran RSRP/RSRQ reporter. ns-3 can report RSRQ = 0/0 (NaN) for a cell with
 * RSRP samples but no RSRQ samples (lte-ue-phy.cc ReportUeMeasurements notes this); the ns3-oran reporter then
 * aborts on the attribute checker. RSRP is forwarded unchanged; a non-finite RSRQ (never used by the xApps) is
 * replaced by the sentinel -1000 dB and every occurrence is logged as a control event. Finite values are forwarded
 * exactly as before. */
void
ForwardUeMeasurement(Ptr<OranReporterLteUeRsrpRsrq> rep, uint16_t rnti, uint16_t cellId, double rsrp, double rsrq,
                     bool serving, uint8_t cc)
{
    if (!std::isfinite(rsrq))
    {
        xapp::MetricsCtrlEvent("rsrq_unavailable_sentinel", cellId, -1, -1, rnti, NAN, NAN, NAN, NAN,
                               "rsrp=" + std::to_string(rsrp));
        rsrq = -1000.0;
    }
    rep->ReportRsrpRsrq(rnti, cellId, rsrp, rsrq, serving, cc);
}

Ptr<Object> g_xappLm; // S4: the xApp LM (for recording its resolved attributes)

// ---------------- S1 helpers ----------------
std::string
JsonEscape(const std::string& in)
{
    std::ostringstream o;
    for (char c : in)
    {
        switch (c)
        {
        case '"':
            o << "\\\"";
            break;
        case '\\':
            o << "\\\\";
            break;
        case '\n':
            o << "\\n";
            break;
        default:
            o << c;
        }
    }
    return o.str();
}

/** Read a fixed UE layout: header "ue,group,x,y,z,initial_cell", one row per UE in order. */
void
ReadLayout(const std::string& file,
           std::vector<Vector>& pos,
           std::vector<uint32_t>& enb,
           std::vector<std::string>& group,
           std::vector<double>& offered)
{
    std::ifstream in(file);
    NS_ABORT_MSG_IF(!in.is_open(), "cannot open layout file " << file);
    std::string line;
    while (std::getline(in, line) && !line.empty() && line[0] == '#')
    {
        // leading comment lines (provenance notes) are allowed before the header
    }
    bool hasOffered = (line == "ue,group,x,y,z,initial_cell,offered_mbps");
    NS_ABORT_MSG_IF(line != "ue,group,x,y,z,initial_cell" && !hasOffered,
                    "layout header must be ue,group,x,y,z,initial_cell[,offered_mbps] (got '"
                        << line << "')");
    uint32_t expect = 0;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::stringstream ss(line);
        std::string f[7];
        for (int k = 0; k < (hasOffered ? 7 : 6); k++)
        {
            NS_ABORT_MSG_IF(!std::getline(ss, f[k], ','), "malformed layout row: " << line);
        }
        NS_ABORT_MSG_IF(std::stoul(f[0]) != expect, "layout rows must be ue = 0,1,2,... in order");
        uint32_t cell = std::stoul(f[5]);
        NS_ABORT_MSG_IF(cell < 1 || cell > 2, "initial_cell must be 1 or 2");
        group.push_back(f[1]);
        pos.emplace_back(std::stod(f[2]), std::stod(f[3]), std::stod(f[4]));
        enb.push_back(cell - 1);
        offered.push_back(hasOffered ? std::stod(f[6]) : -1.0); // -1: use --rate-a/--rate-b
        expect++;
    }
    NS_ABORT_MSG_IF(expect == 0, "layout file has no UEs");
}

std::string
B5ParamString()
{
    xapp::b5::Params p;
    std::ostringstream o;
    o << "kappa=" << p.kappa << ";alpha=" << p.alpha << ";zeta=" << p.zeta << ";qin=" << p.qinDb
      << ";low_demand_mbps=" << p.lowDemandMbps << ";vulnerable_db=" << p.vulnerableDb << ";eps_vul=" << p.epsVulnerable
      << ";eps_low=" << p.epsLowDemand << ";eps_av=" << p.epsAlreadyViolating << ";eps_other=" << p.epsOther
      << ";pessimism=all_low_confidence_provisional";
    return o.str();
}

// ---------------- S5: estimator probe (calibration / validation harness; prediction only, no decision) ----
struct EstProbeCfg
{
    double t{-1};
    uint32_t tsUe{3};
    uint16_t tsTarget{2}, tpcCell{2};
    double stepDb{3}, kappa{1.0}, nominalDbm{30};
    bool kappaGrid{false}; //!< evaluate kappa = 0.50, 0.51, ..., 1.50 (calibration grid) instead of kappa
    std::string out;
};

void
EstimatorProbe(EstProbeCfg cfg)
{
    using namespace xapp;
    const double now = Simulator::Now().GetSeconds();
    policy::Snapshot s = ric::BuildSnapshot(now);
    static est::AmcLut lut = est::BuildAmcLut();
    uint64_t tsImsi = 0;
    if (auto d = DynamicCast<LteUeNetDevice>(cmdlife::UeDevice(cfg.tsUe)))
    {
        tsImsi = d->GetImsi();
    }
    est::Input in;
    in.kappa = cfg.kappa;
    for (const auto& [c, q] : s.load)
    {
        if (!q.empty())
        {
            in.rhoInit[c] = q[0].prb;
        }
    }
    std::ofstream f(cfg.out + "-estimate.csv");
    f << "probe_s,candidate,imsi,verifiable,why,cell_now,cell_after,svc_age_s,serving_age_s,demand_mean_mbps,"
         "served_mbps,harq_loss_share,i3b_now,rsrp_c1,rsrp_c2,gamma_ctrl_now_db,gamma_ctrl_after_db,gamma_data_db,"
         "eta_bytes_rb,cap_mbps,rb_time,tau,pred_mbps,rho_c1,rho_c2,load_c1,load_c2,prb_rep_c1,prb_rep_c2,"
         "hol_rep_c1,hol_rep_c2,power_rep_c1,power_rep_c2,newest_load_t_c1,newest_load_t_c2,iterations,converged,"
         "kappa,compute_us\n";
    struct Info
    {
        const policy::UeView* u;
        bool ok;
        std::string why;
    };
    std::vector<Info> info;
    // a stale view (serving information older than 1 s) of an IMSI that also has a fresh view is a leftover RIC
    // entry (e.g. RSRP reports sent as E2 node 0 before registration); it is not a separate UE
    std::map<uint64_t, bool> freshImsi;
    for (const auto& u : s.ues)
    {
        if (u.imsi != 0 && u.servingAge <= 1.0 + 1e-9)
            freshImsi[u.imsi] = true;
    }
    for (const auto& u : s.ues)
    {
        std::string why;
        if (u.imsi != 0 && u.servingAge > 1.0 + 1e-9 && freshImsi.count(u.imsi))
        {
            info.push_back({&u, false, "stale_duplicate_view"});
            continue;
        }
        if (u.imsi == 0)
            why = "identity_unknown";
        else if (!u.svc || now - u.svc->t > 1.0 + 1e-9 || u.svc->cell != u.servingCell)
            why = "no_fresh_service_report";
        else if (!u.rsrpDbm.count(u.servingCell) || u.servingAge > 1.0 + 1e-9)
            why = "no_fresh_rsrp";
        info.push_back({&u, why.empty(), why});
        if (why.empty())
        {
            est::UeIn e;
            e.imsi = u.imsi;
            e.cell = u.servingCell;
            e.demandMbps = u.svc->demandMeanMbps > 0 ? u.svc->demandMeanMbps : u.svc->demandMbps;
            e.rsrpDbm = u.rsrpDbm;
            in.ues.push_back(e);
        }
    }
    std::vector<est::Candidate> cands(4);
    cands[0].name = "N";
    cands[1].name = "T";
    cands[1].moveImsi = tsImsi;
    cands[1].moveTo = cfg.tsTarget;
    cands[2].name = "P";
    cands[2].deltaDb[cfg.tpcCell] = -cfg.stepDb;
    cands[3] = cands[1];
    cands[3].name = "B";
    cands[3].deltaDb[cfg.tpcCell] = -cfg.stepDb;
    std::vector<double> kappas;
    if (cfg.kappaGrid)
    {
        for (int k = 50; k <= 150; k++)
            kappas.push_back(k / 100.0);
    }
    else
    {
        kappas.push_back(cfg.kappa);
    }
    auto g = [](const std::map<uint16_t, double>& m, uint16_t c) {
        auto it = m.find(c);
        return it == m.end() ? std::nan("") : it->second;
    };
    auto rep = [&](uint16_t c, int what) {
        auto it = s.load.find(c);
        if (it == s.load.end() || it->second.empty())
            return std::nan("");
        const auto& l = it->second[0];
        return what == 0 ? l.prb : what == 1 ? l.holMs : what == 2 ? l.txPowerDbm : l.t;
    };
    f << std::fixed << std::setprecision(6);
    for (double kappa : kappas)
    {
    in.kappa = kappa;
    // computation of one estimator call set (4 candidates), measured per kappa (laptop time, not RIC latency)
    auto t0 = std::chrono::steady_clock::now();
    std::vector<est::Output> outs;
    for (const auto& a : cands)
    {
        outs.push_back(est::Predict(in, a, lut));
    }
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    for (size_t k = 0; k < cands.size(); k++)
    {
        const auto& a = cands[k];
        const auto& o = outs[k];
        for (const auto& x : info)
        {
            const auto& u = *x.u;
            const est::UeOut* eo = nullptr;
            for (const auto& r : o.ues)
            {
                if (r.imsi == u.imsi)
                    eo = &r;
            }
            uint16_t after = (a.moveImsi != 0 && u.imsi == a.moveImsi) ? a.moveTo : u.servingCell;
            double dC = a.deltaDb.count(cfg.tpcCell) ? a.deltaDb.at(cfg.tpcCell) : 0.0;
            double gNow = policy::GammaCtrl(u, u.servingCell, -123.24);
            double gAfter = policy::GammaCtrl(u, after, -123.24, cfg.tpcCell, dC);
            f << now << "," << a.name << "," << u.imsi << "," << x.ok << "," << x.why << "," << u.servingCell << ","
              << after << "," << (u.svc ? now - u.svc->t : std::nan("")) << "," << u.servingAge << ","
              << (u.svc ? u.svc->demandMeanMbps : std::nan("")) << "," << (u.svc ? u.svc->servedMbps : std::nan(""))
              << "," << (u.svc ? u.svc->harqLossShare : std::nan("")) << ","
              << (u.svc ? int(policy::I3Flag(*u.svc, 0.95)) : -1) << "," << g(u.rsrpDbm, 1) << ","
              << g(u.rsrpDbm, 2) << "," << gNow << "," << gAfter << ","
              << (eo ? eo->gammaDb : std::nan("")) << "," << (eo ? eo->etaBytes : std::nan("")) << ","
              << (eo ? eo->capMbps : std::nan("")) << "," << (eo ? eo->rbTime : std::nan("")) << ","
              << (eo ? eo->tau : std::nan("")) << "," << (eo ? eo->rateMbps : std::nan("")) << ","
              << g(o.rho, 1) << "," << g(o.rho, 2) << "," << g(o.load, 1) << "," << g(o.load, 2) << ","
              << rep(1, 0) << "," << rep(2, 0) << "," << rep(1, 1) << "," << rep(2, 1) << "," << rep(1, 2) << ","
              << rep(2, 2) << "," << rep(1, 3) << "," << rep(2, 3) << "," << o.iterations << "," << o.converged
              << "," << kappa << "," << us << "\n";
        }
    }
    }
}

int
main(int argc, char* argv[])
{
    std::vector<std::string> argvCopy(argv, argv + argc);
    // ---------------- Layout and radio ----------------
    uint32_t nNearA = 3;   // UEs close to eNB A (cell 1)
    uint32_t nEdge = 3;    // UEs in the overlap zone, attached to A
    uint32_t nNearB = 3;   // UEs close to eNB B (cell 2)
    double isd = 500.0;    // inter-site distance (m)
    double nearFrac = 0.25; // near-UE circle radius as a fraction of isd
    double edgeFrac = 0.45; // x position of edge UEs as a fraction of isd (<0.5 = closer to A)
    double posJitter = 10.0; // +/- uniform jitter on UE x and y (m); changes with RngRun
    double enbTxPower = 30.0; // dBm
    double pathlossExp = 3.0; // log-distance exponent
    uint16_t bandwidthRb = 25; // 25 RBs = 5 MHz
    std::string scheduler = "ns3::PfFfMacScheduler"; // proportional fair (was round robin)

    // Scripted power step (Phase 2 test, no RIC): at tx-step-time, add tx-step-db to cell tx-step-cell
    double txStepTime = -1; // seconds; negative = no step
    uint16_t txStepCell = 2;
    double txStepDb = -6.0;
    double txMin = 10.0; // dBm, lowest power a RIC command may set
    double txMax = 40.0; // dBm, highest power a RIC command may set
    std::string lm = "distance"; // default xApp: "distance" (placeholder) or "txstep" (Phase 2b test)

    // ---------------- Traffic ----------------
    double rateA = 1.0;        // offered downlink rate per UE attached to cell A (Mbps)
    double rateB = 1.0;        // offered downlink rate per UE attached to cell B (Mbps)
    uint32_t packetSize = 1024; // bytes
    Time trafficStart = Seconds(3);
    Time warmup = Seconds(2); // statistics start at trafficStart + warmup
    Time window = MilliSeconds(500);
    Time simTime = Seconds(30);

    // ---------------- RIC ----------------
    bool useRic = true;
    bool verbose = false;
    Time maxWaitTime = Seconds(0.010);
    std::string processingDelayRv = "ns3::NormalRandomVariable[Mean=0.005|Variance=0.000031]";
    Time lmQueryInterval = Seconds(1);
    std::string lateCommandPolicy = "DROP";
    bool dbLog = false;

    // ---------------- Output ----------------
    std::string out = "run";
    std::string dbFileName = "oran-repository.db";

    // ---------------- S1 (opt-in; defaults reproduce the S0 baseline) ----------------
    std::string layoutFile = "";   // fixed UE layout (empty = legacy generator + jitter)
    int64_t assignStreams = -1;    // explicit RNG stream base (-1 = ns-3 automatic, legacy)
    Time acctGuard = Seconds(1);   // loss accounting guard (provisional, D-MET-5)
    bool tracePrbSubframes = false;
    uint32_t testRngPerturb = 0;   // TEST ONLY: negative control for stream isolation

    // ---------------- S2 (opt-in; defaults reproduce the S0/S1 baseline) ----------------
    bool anr = true;                  // LteHelper::AnrEnabled (ns-3 default true; D-MET-13)
    std::string radio = "R0";         // R0 = LogDistance (legacy); R1 = 3GPP UMa NLOS + shadowing
    double enbHeight = 30.0;          // eNB antenna height (m); TR 38.901 UMa specifies 25 m
    double rateStepTime = -1;         // s; <0: no load step
    double rateAAfter = -1;           // Mbps per cell-1 UE after the step (<0: unchanged)
    double rateBAfter = -1;           // Mbps per cell-2 UE after the step (<0: unchanged)
    double hoTime = -1;               // s; <0: no scripted handover
    uint32_t hoUe = 0;                // UE index for the scripted handover
    uint16_t hoTargetCell = 2;        // target cell of the scripted handover
    bool telemetry = false;           // modelled E2 telemetry (RSRP + eNB load) + RIC sink log
    double telemetryInterval = 1.0;   // s; eNB load reporter trigger interval
    double e2SendInterval = 1.0;      // s; E2 node terminator send interval (legacy 1 s)
    double targetRate = -1;           // Mbps for UEs of layout group "target" (<0: cell rate)
    std::string attach = "fixed";     // fixed: attach to layout/legacy cell; best: ns-3 cell selection
    bool r1Shadowing = true;          // R1 only: log-normal shadowing on (R1 definition) / off (reference)
    std::string r1ShadowingMode = "independent"; // R1: independent (ns-3 default: separate DL/UL draws)
                                                 //     or link (one draw per eNB-UE link, TR 38.901 7.6.5)

    // ---------------- S3 (opt-in except the lifecycle log, which only adds <out>-cmdlife.csv) ----
    bool cmdLifecycle = true;         // per-command lifecycle log + V1 guard on RIC handovers
    std::string cmdScript = "";       // --lm=script: scripted test commands (OranLmScriptedCommands)
    double procDelay = -1;            // s; >=0: constant LM processing delay (overrides processing-delay-rv)
    double e2CmdDelay = -1;           // s; >=0: constant RIC->node E2 command transport delay
    double coordDelay = 0;            // s; constant hold in the CMM before dispatch
    double ricStart = 1.0;            // s; P3 report/query phase: RIC start (query grid = ricStart + k*interval)
    std::string attachLayoutFile = ""; // S5 prep: UE positions during initial attachment (retained connection)
    double attachMoveTime = 1.0;       // s; UEs move from the attach layout to --layout-file at this time
    std::string coordMode = "none";    // S5: none (OranCmmNoopLogged) | b3 | b4-ts | b4-tpc (OranCmmCoord)
    bool coordReverse = false;         // S5: B3 order sensitivity (all TPC before all TS)
    std::string dualExec = "none";     // --lm=dual: which xApp executes (none|ts|tpc); the other shadows
    bool ueServiceReport = false;     // S4 prep: per-UE DL service E2 report (needs --telemetry)
    std::string ctrlActivation = "none"; // S5 C-A: none | warmup (controllers use only windows starting >= T_A)
    std::string saDef = "combined";    // S5 state-aware modes: combined | win_2 | win_5
    bool b5Shadow = false;             // S5 b5sa: evaluate + log, never act (estimator tests)
    bool interactLog = false;          // S5: shadow interaction diagnostic <out>-interact.csv (labels only)
    EstProbeCfg estProbe;              // S5: estimator probe (calibration / validation harness)
    int testDeactivateCell = -1;      // TEST ONLY: deactivate this cell's E2 terminator ...
    double testDeactivateTime = -1;   // ... at this time (s)

    CommandLine cmd(__FILE__);
    cmd.AddValue("n-near-a", "UEs near eNB A (cell 1)", nNearA);
    cmd.AddValue("n-edge", "UEs in the overlap zone, attached to cell 1", nEdge);
    cmd.AddValue("n-near-b", "UEs near eNB B (cell 2)", nNearB);
    cmd.AddValue("isd", "Inter-site distance (m)", isd);
    cmd.AddValue("near-frac", "Near-UE radius as a fraction of isd", nearFrac);
    cmd.AddValue("edge-frac", "Edge-UE x position as a fraction of isd", edgeFrac);
    cmd.AddValue("pos-jitter", "Uniform +/- jitter on UE positions (m)", posJitter);
    cmd.AddValue("enb-tx-power", "eNB transmit power (dBm)", enbTxPower);
    cmd.AddValue("pathloss-exp", "Log-distance pathloss exponent", pathlossExp);
    cmd.AddValue("bandwidth-rb", "DL/UL bandwidth in resource blocks", bandwidthRb);
    cmd.AddValue("scheduler", "MAC scheduler type, e.g. ns3::PfFfMacScheduler", scheduler);
    cmd.AddValue("tx-step-time", "Time (s) of a scripted power step; <0 = none", txStepTime);
    cmd.AddValue("tx-step-cell", "Cell (1 or 2) whose power is stepped", txStepCell);
    cmd.AddValue("tx-step-db", "Power change in dB (negative = lower)", txStepDb);
    cmd.AddValue("tx-min", "Lowest power a RIC command may set (dBm)", txMin);
    cmd.AddValue("tx-max", "Highest power a RIC command may set (dBm)", txMax);
    cmd.AddValue("lm",
                 "Default xApp: distance | txstep (txstep sends the tx-step-* change as a RIC "
                 "command instead of a script)",
                 lm);
    cmd.AddValue("rate-a","Offered DL rate per UE in cell 1 (Mbps)", rateA);
    cmd.AddValue("rate-b", "Offered DL rate per UE in cell 2 (Mbps)", rateB);
    cmd.AddValue("packet-size", "UDP packet size (bytes)", packetSize);
    cmd.AddValue("traffic-start", "Time traffic starts", trafficStart);
    cmd.AddValue("warmup", "Warm-up after traffic start, excluded from the summary", warmup);
    cmd.AddValue("window", "Throughput measurement window", window);
    cmd.AddValue("sim-time", "The amount of time to simulate", simTime);
    cmd.AddValue("ric", "Enable the Near-RT RIC", useRic);
    cmd.AddValue("verbose", "Verbose ORAN helper output", verbose);
    cmd.AddValue("max-wait-time", "LM deadline (0 = no deadline)", maxWaitTime);
    cmd.AddValue("processing-delay-rv", "LM processing delay random variable", processingDelayRv);
    cmd.AddValue("lm-query-interval", "Control period of the RIC", lmQueryInterval);
    cmd.AddValue("late-command-policy", "\"DROP\" or \"SAVE\"", lateCommandPolicy);
    cmd.AddValue("db-log", "Print SQL query results", dbLog);
    cmd.AddValue("out", "Prefix for CSV output files", out);
    cmd.AddValue("db-file", "SQLite repository file (unique per parallel run)", dbFileName);
    cmd.AddValue("layout-file", "Fixed UE layout CSV (ue,group,x,y,z,initial_cell)", layoutFile);
    cmd.AddValue("assign-streams", "Explicit RNG stream base (>=0), -1 = automatic", assignStreams);
    cmd.AddValue("acct-guard", "Loss accounting excludes packets sent in the last T", acctGuard);
    cmd.AddValue("trace-prb-subframes", "Write per-frame PRB and MAC scheduling traces",
                 tracePrbSubframes);
    cmd.AddValue("anr", "LteHelper::AnrEnabled (ns-3 default true)", anr);
    cmd.AddValue("radio", "Radio class: R0 (LogDistance, legacy) or R1 (3GPP UMa NLOS + shadowing)",
                 radio);
    cmd.AddValue("enb-height", "eNB antenna height (m)", enbHeight);
    cmd.AddValue("rate-step-time", "Time (s) of a per-cell offered-load step; <0 = none", rateStepTime);
    cmd.AddValue("rate-a-after", "Offered Mbps per cell-1 UE after the step (<0 = unchanged)", rateAAfter);
    cmd.AddValue("rate-b-after", "Offered Mbps per cell-2 UE after the step (<0 = unchanged)", rateBAfter);
    cmd.AddValue("cmd-lifecycle", "S3: per-command lifecycle log <out>-cmdlife.csv and V1 guard on RIC handovers", cmdLifecycle);
    cmd.AddValue("cmd-script", "S3 test LM script (with --lm=script), see oran-lm-scripted-commands.h", cmdScript);
    cmd.AddValue("proc-delay", "S3: constant LM processing delay in s (<0: use processing-delay-rv)", procDelay);
    cmd.AddValue("e2-cmd-delay", "S3: constant E2 command transport delay RIC->node in s (<0: ns3-oran default 0)", e2CmdDelay);
    cmd.AddValue("coord-delay", "S3: constant CMM hold before dispatch in s", coordDelay);
    cmd.AddValue("ric-start", "P3: RIC start time in s (default 1; 1.001 = query 1 ms after same-instant reports)", ricStart);
    cmd.AddValue("attach-layout-file", "S5 prep: UE positions used for initial attachment; UEs move to --layout-file at --attach-move-time", attachLayoutFile);
    cmd.AddValue("attach-move-time", "S5 prep: time (s) of the move from the attach layout to the scenario layout", attachMoveTime);
    cmd.AddValue("coord", "S5: none | b3 | b4-ts | b4-tpc | b5 | b4sa-ts | b4sa-tpc | b5sa (coordination CMM)", coordMode);
    cmd.AddValue("sa-def", "S5 state-aware detector: combined (primary) | win_2 | win_5 (sensitivity)", saDef);
    cmd.AddValue("b5-shadow", "S5 b5sa: evaluate and log only, never act (estimator tests)", b5Shadow);
    cmd.AddValue("coord-reverse", "S5: dispatch all TPC before all TS (B3 order sensitivity)", coordReverse);
    cmd.AddValue("dual-exec", "S5 prep, --lm=dual: none | ts | tpc (the executing xApp; the other only logs shadow proposals)", dualExec);
    cmd.AddValue("ue-service-report", "S4 prep: per-UE DL service E2 report from each eNB (needs --telemetry)", ueServiceReport);
    cmd.AddValue("ctrl-activation", "S5 C-A: none | warmup (xApps use only reports whose window starts at or after traffic-start + warmup)", ctrlActivation);
    cmd.AddValue("interact-log", "S5: shadow interaction diagnostic <out>-interact.csv (labels only; with --coord)", interactLog);
    cmd.AddValue("est-probe-time", "S5: time (s) of the estimator probe (<0: none); writes <out>-estimate.csv", estProbe.t);
    cmd.AddValue("est-ts-ue", "S5 probe: UE index of the TS candidate", estProbe.tsUe);
    cmd.AddValue("est-ts-target", "S5 probe: TS target cell", estProbe.tsTarget);
    cmd.AddValue("est-tpc-cell", "S5 probe: TPC cell", estProbe.tpcCell);
    cmd.AddValue("est-step-db", "S5 probe: TPC reduction (dB)", estProbe.stepDb);
    cmd.AddValue("est-kappa", "S5 probe: estimator efficiency factor kappa", estProbe.kappa);
    cmd.AddValue("est-kappa-grid", "S5 probe: evaluate kappa = 0.50..1.50 step 0.01 (calibration grid)", estProbe.kappaGrid);
    cmd.AddValue("test-deactivate-cell", "TEST ONLY: deactivate this cell's E2 terminator", testDeactivateCell);
    cmd.AddValue("test-deactivate-time", "TEST ONLY: time (s) of the deactivation", testDeactivateTime);
    cmd.AddValue("ho-time", "Time (s) of a scripted X2 handover; <0 = none", hoTime);
    cmd.AddValue("ho-ue", "UE index of the scripted handover", hoUe);
    cmd.AddValue("ho-target-cell", "Target cell of the scripted handover (0 = the other cell)",
                 hoTargetCell);
    cmd.AddValue("telemetry", "Modelled E2 telemetry (UE RSRP/RSRQ, eNB load) + RIC sink log", telemetry);
    cmd.AddValue("telemetry-interval", "eNB load reporter trigger interval (s)", telemetryInterval);
    cmd.AddValue("e2-send-interval", "E2 node terminator send interval (s)", e2SendInterval);
    cmd.AddValue("target-rate", "Offered Mbps for layout group 'target' (<0 = cell rate)", targetRate);
    cmd.AddValue("attach", "Initial attachment: fixed (layout/legacy cell) or best (ns-3 automatic "
                 "initial cell selection)", attach);
    cmd.AddValue("r1-shadowing", "R1 only: enable log-normal shadowing (false = validation reference)",
                 r1Shadowing);
    cmd.AddValue("r1-shadowing-mode",
                 "R1 only: independent (ns-3 default, separate DL/UL shadowing) or link (one shadowing "
                 "value per eNB-UE link shared by DL and UL, 3GPP TR 38.901 cl. 7.6.5)",
                 r1ShadowingMode);
    cmd.AddValue("test-rng-perturb",
                 "TEST ONLY: create+draw K automatic-stream RVs before LTE install (mimics a "
                 "policy object consuming randomness; used by the matched-run acceptance test)",
                 testRngPerturb);
    cmd.Parse(argc, argv);
    std::map<std::string, std::string> streamMap; // name -> stream range, for config.json

    LogComponentEnable("OranNearRtRic", (LogLevel)(LOG_PREFIX_TIME | LOG_WARN));

    Config::SetDefault("ns3::LteHelper::UseIdealRrc", BooleanValue(false));
    Config::SetDefault("ns3::LteEnbPhy::TxPower", DoubleValue(enbTxPower));
    Config::SetDefault("ns3::LteEnbNetDevice::DlBandwidth", UintegerValue(bandwidthRb));
    Config::SetDefault("ns3::LteEnbNetDevice::UlBandwidth", UintegerValue(bandwidthRb));

    // ---------------- LTE + EPC ----------------
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    lteHelper->SetEpcHelper(epcHelper);
    lteHelper->SetSchedulerType(scheduler);
    lteHelper->SetHandoverAlgorithmType("ns3::NoOpHandoverAlgorithm"); // only the RIC hands over
    NS_ABORT_MSG_IF(radio != "R0" && radio != "R1", "--radio must be R0 or R1");
    if (radio == "R0")
    {
        lteHelper->SetAttribute("PathlossModel", StringValue("ns3::LogDistancePropagationLossModel"));
        lteHelper->SetPathlossModelAttribute("Exponent", DoubleValue(pathlossExp));
    }
    else
    {
        // R1 (provisionally approved D-RAD-3): TR 38.901 UMa path loss, NLOS fixed, log-normal
        // shadowing (sigma 6 dB NLOS), outdoor UEs (no building penetration). Frequency is set by
        // LteHelper from the DL/UL EARFCN. Parameter ranges are checked by the scenario for the
        // eNB-UE links (EnforceParameterRanges would also abort on the irrelevant eNB-eNB and
        // UE-UE links that the spectrum channel evaluates).
        lteHelper->SetAttribute("PathlossModel", StringValue("ns3::ThreeGppUmaPropagationLossModel"));
        NS_ABORT_MSG_IF(r1ShadowingMode != "independent" && r1ShadowingMode != "link",
                        "--r1-shadowing-mode must be independent or link");
        // link mode: the UMa instances' own (per-direction) shadowing is off; one shared
        // LinkShadowingLossModel is chained after both DL and UL instances below
        lteHelper->SetPathlossModelAttribute(
            "ShadowingEnabled",
            BooleanValue(r1Shadowing && r1ShadowingMode == "independent"));
        lteHelper->SetPathlossModelAttribute("BuildingPenetrationLossesEnabled", BooleanValue(false));
        lteHelper->SetPathlossModelAttribute(
            "ChannelConditionModel",
            PointerValue(CreateObject<NeverLosChannelConditionModel>()));
    }
    if (!anr)
    {
        lteHelper->SetAttribute("AnrEnabled", BooleanValue(false));
    }

    Ptr<Node> pgw = epcHelper->GetPgwNode();

    // Remote host that sends the downlink traffic
    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);
    InternetStackHelper internet;
    internet.Install(remoteHostContainer);

    PointToPointHelper p2ph;
    p2ph.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Gb/s")));
    p2ph.SetDeviceAttribute("Mtu", UintegerValue(1500));
    p2ph.SetChannelAttribute("Delay", TimeValue(MilliSeconds(10)));
    NetDeviceContainer internetDevices = p2ph.Install(pgw, remoteHost);
    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.0.0.0");
    ipv4h.Assign(internetDevices);

    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(remoteHost->GetObject<Ipv4>());
    remoteHostStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

    // ---------------- Nodes and positions ----------------
    std::vector<Vector> uePos;
    std::vector<uint32_t> ueEnb; // index of the eNB each UE attaches to initially
    std::vector<double> layoutOffered;
    if (!layoutFile.empty())
    {
        ReadLayout(layoutFile, uePos, ueEnb, g_group, layoutOffered);
    }
    uint32_t numberOfUes = layoutFile.empty() ? nNearA + nEdge + nNearB : uePos.size();
    NodeContainer enbNodes;
    NodeContainer ueNodes;
    enbNodes.Create(2);
    ueNodes.Create(numberOfUes);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(enbNodes);
    mobility.Install(ueNodes);
    enbNodes.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0, 0, enbHeight));
    enbNodes.Get(1)->GetObject<MobilityModel>()->SetPosition(Vector(isd, 0, enbHeight));

    if (!layoutFile.empty())
    {
        for (uint32_t i = 0; i < numberOfUes; i++)
        {
            ueNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(uePos[i]);
        }
        if (!attachLayoutFile.empty())
        {
            // S5 prep, retained connection: attach at the attach-layout positions (same UEs, same configured
            // cells), then move to the scenario positions before telemetry, traffic and any control.
            std::vector<Vector> aPos;
            std::vector<uint32_t> aEnb;
            std::vector<std::string> aGroup;
            std::vector<double> aOff;
            ReadLayout(attachLayoutFile, aPos, aEnb, aGroup, aOff);
            NS_ABORT_MSG_IF(aPos.size() != numberOfUes || aEnb != ueEnb || aGroup != g_group,
                            "attach layout must have the same UEs, groups and configured cells");
            NS_ABORT_MSG_IF(attachMoveTime <= 0 || attachMoveTime >= trafficStart.GetSeconds(),
                            "--attach-move-time must lie before traffic start");
            for (uint32_t i = 0; i < numberOfUes; i++)
            {
                Ptr<MobilityModel> mm = ueNodes.Get(i)->GetObject<MobilityModel>();
                mm->SetPosition(aPos[i]);
                if (CalculateDistance(aPos[i], uePos[i]) > 1e-9)
                {
                    Vector target = uePos[i];
                    Simulator::Schedule(Seconds(attachMoveTime), [mm, target, i]() {
                        mm->SetPosition(target);
                        xapp::MetricsCtrlEvent("ue_moved_to_scenario_position", -1, Imsi(i), -1, -1, NAN, NAN,
                                               NAN, NAN, "x=" + std::to_string(target.x));
                    });
                }
            }
        }
    }
    else
    {
    Ptr<UniformRandomVariable> jitter = CreateObject<UniformRandomVariable>();
    jitter->SetAttribute("Min", DoubleValue(-posJitter));
    jitter->SetAttribute("Max", DoubleValue(posJitter));
    if (assignStreams >= 0)
    {
        jitter->SetStream(assignStreams + 0);
        streamMap["layout_jitter"] = std::to_string(assignStreams + 0);
    }

    // UE order: near-A [0, nNearA), edge [nNearA, nNearA+nEdge), near-B [rest]
    double r = nearFrac * isd;
    for (uint32_t k = 0; k < nNearA; k++)
    {
        double a = 2 * M_PI * (k + 0.5) / nNearA;
        uePos.emplace_back(r * std::cos(a), r * std::sin(a), 1.5);
        ueEnb.push_back(0);
        g_group.emplace_back("nearA");
    }
    for (uint32_t k = 0; k < nEdge; k++)
    {
        double spread = 0.3 * isd;
        double y = (nEdge > 1) ? (-spread / 2 + spread * k / (nEdge - 1)) : 0.0;
        uePos.emplace_back(edgeFrac * isd, y, 1.5);
        ueEnb.push_back(0);
        g_group.emplace_back("edge");
    }
    for (uint32_t k = 0; k < nNearB; k++)
    {
        double a = 2 * M_PI * (k + 0.5) / nNearB;
        uePos.emplace_back(isd + r * std::cos(a), r * std::sin(a), 1.5);
        ueEnb.push_back(1);
        g_group.emplace_back("nearB");
    }
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        Vector p = uePos[i];
        p.x += jitter->GetValue();
        p.y += jitter->GetValue();
        uePos[i] = p;
        ueNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(p);
    }
    } // legacy layout generator

    // ---------------- LTE devices, IP, attach ----------------
    std::vector<Ptr<UniformRandomVariable>> perturb; // kept alive until the end of main
    for (uint32_t k = 0; k < testRngPerturb; k++)
    {
        perturb.push_back(CreateObject<UniformRandomVariable>());
        perturb.back()->GetValue();
    }
    NetDeviceContainer enbLteDevs = lteHelper->InstallEnbDevice(enbNodes);
    NetDeviceContainer ueLteDevs = lteHelper->InstallUeDevice(ueNodes);
    g_ueDevs = ueLteDevs;
    if (assignStreams >= 0)
    {
        // LTE PHY (TB error draws), UE MAC (RA preambles) and EPC: fixed explicit streams that
        // the automatic allocation used by policy-side objects (e.g. ns3-oran) cannot shift.
        int64_t s0 = assignStreams + 100;
        int64_t n1 = lteHelper->AssignStreams(enbLteDevs, s0);
        int64_t n2 = lteHelper->AssignStreams(ueLteDevs, s0 + n1);
        streamMap["lte_enb_devices"] = std::to_string(s0) + ".." + std::to_string(s0 + n1 - 1);
        streamMap["lte_ue_devices"] =
            std::to_string(s0 + n1) + ".." + std::to_string(s0 + n1 + n2 - 1);
        if (radio == "R1")
        {
            // Not covered by LteHelper::AssignStreams (S1 report §4): DL and UL path-loss models.
            int64_t sd = assignStreams + 50;
            int64_t nd = lteHelper->GetDownlinkSpectrumChannel()->GetPropagationLossModel()->AssignStreams(sd);
            int64_t su = assignStreams + 60;
            int64_t nu = lteHelper->GetUplinkSpectrumChannel()->GetPropagationLossModel()->AssignStreams(su);
            streamMap["pathloss_dl"] = std::to_string(sd) + ".." + std::to_string(sd + nd - 1);
            streamMap["pathloss_ul"] = std::to_string(su) + ".." + std::to_string(su + nu - 1);
        }
    }
    Ptr<LinkShadowingLossModel> linkShadowing;
    if (radio == "R1" && r1Shadowing && r1ShadowingMode == "link")
    {
        linkShadowing = CreateObject<LinkShadowingLossModel>();
        lteHelper->GetDownlinkSpectrumChannel()->GetPropagationLossModel()->SetNext(linkShadowing);
        lteHelper->GetUplinkSpectrumChannel()->GetPropagationLossModel()->SetNext(linkShadowing);
        if (assignStreams >= 0)
        {
            linkShadowing->AssignStreams(assignStreams + 70);
            streamMap["link_shadowing"] = std::to_string(assignStreams + 70);
        }
    }
    double dlFrequencyHz = -1;
    {
        Ptr<PropagationLossModel> pl = lteHelper->GetDownlinkSpectrumChannel()->GetPropagationLossModel();
        DoubleValue f;
        if (pl && pl->GetAttributeFailSafe("Frequency", f))
        {
            dlFrequencyHz = f.Get();
        }
    }
    double ulFrequencyHz = -1;
    {
        Ptr<PropagationLossModel> pl = lteHelper->GetUplinkSpectrumChannel()->GetPropagationLossModel();
        DoubleValue f;
        if (pl && pl->GetAttributeFailSafe("Frequency", f))
        {
            ulFrequencyHz = f.Get();
        }
    }
    if (radio == "R1")
    {
        // TR 38.901 Table 7.4.1-1 UMa applicability: h_BS = 25 m, 1.5 <= h_UT <= 22.5 m,
        // 10 m <= d2D <= 5 km, 0.5-100 GHz (as implemented in three-gpp-propagation-loss-model.cc)
        NS_ABORT_MSG_IF(dlFrequencyHz < 0.5e9 || dlFrequencyHz > 100e9, "R1 frequency out of range");
        NS_ABORT_MSG_IF(enbHeight != 25.0, "R1 requires --enb-height=25 (TR 38.901 UMa)");
        for (uint32_t i = 0; i < numberOfUes; i++)
        {
            NS_ABORT_MSG_IF(uePos[i].z < 1.5 || uePos[i].z > 22.5, "R1 UE height out of range");
            for (uint32_t k = 0; k < 2; k++)
            {
                Vector e = enbNodes.Get(k)->GetObject<MobilityModel>()->GetPosition();
                double d2 = std::hypot(uePos[i].x - e.x, uePos[i].y - e.y);
                NS_ABORT_MSG_IF(d2 < 10.0 || d2 > 5000.0,
                                "R1 2D distance out of range for UE " << i << " (" << d2 << " m)");
            }
        }
    }

    internet.Install(ueNodes);
    Ipv4InterfaceContainer ueIpIfaces = epcHelper->AssignUeIpv4Address(NetDeviceContainer(ueLteDevs));
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        Ptr<Ipv4StaticRouting> ueStaticRouting =
            ipv4RoutingHelper.GetStaticRouting(ueNodes.Get(i)->GetObject<Ipv4>());
        ueStaticRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
        NS_ABORT_MSG_IF(attach != "fixed" && attach != "best", "--attach must be fixed or best");
        if (attach == "fixed")
        {
            lteHelper->Attach(ueLteDevs.Get(i), enbLteDevs.Get(ueEnb[i]));
        }
        else
        {
            lteHelper->Attach(ueLteDevs.Get(i)); // UE performs initial cell selection itself
        }
    }
    lteHelper->AddX2Interface(enbNodes);

    NS_ABORT_MSG_IF(lm != "distance" && lm != "txstep" && lm != "noop" && lm != "script" && lm != "ts" &&
                        lm != "tpc" && lm != "dual",
                    "--lm must be distance, txstep, noop, script, ts, tpc or dual");
    NS_ABORT_MSG_IF(dualExec != "none" && dualExec != "ts" && dualExec != "tpc" && dualExec != "both",
                    "--dual-exec must be none, ts, tpc or both");
    NS_ABORT_MSG_IF(coordMode != "none" && coordMode != "b3" && coordMode != "b4-ts" && coordMode != "b4-tpc" &&
                        coordMode != "b5" && coordMode != "b4sa-ts" && coordMode != "b4sa-tpc" && coordMode != "b5sa",
                    "--coord must be none, b3, b4-ts, b4-tpc, b5, b4sa-ts, b4sa-tpc or b5sa");
    NS_ABORT_MSG_IF((coordMode == "b5" || coordMode == "b5sa") && !(telemetry && ueServiceReport),
                    "--coord=b5/b5sa needs --telemetry and --ue-service-report");
    NS_ABORT_MSG_IF(saDef != "combined" && saDef != "win_2" && saDef != "win_5", "--sa-def must be combined, win_2 or win_5");
    NS_ABORT_MSG_IF(b5Shadow && coordMode != "b5sa", "--b5-shadow needs --coord=b5sa");
    NS_ABORT_MSG_IF(coordMode != "none" && !cmdLifecycle, "--coord needs --cmd-lifecycle");
    const bool xappLm = lm == "ts" || lm == "tpc" || lm == "dual";
    NS_ABORT_MSG_IF(ctrlActivation != "none" && ctrlActivation != "warmup", "--ctrl-activation must be none or warmup");
    NS_ABORT_MSG_IF(interactLog && coordMode == "none", "--interact-log needs --coord");
    if (ctrlActivation == "warmup")
    {
        // T_A must be a boundary of the report windows so that "window starts at or after T_A" is exact
        double tA = (trafficStart + warmup).GetSeconds();
        double k = tA / telemetryInterval;
        NS_ABORT_MSG_IF(std::fabs(k - std::round(k)) > 1e-9, "C-A: traffic-start + warmup must be a multiple of telemetry-interval");
        xapp::ric::SetAdmissionStart(tA);
    }
    NS_ABORT_MSG_IF(xappLm && !(useRic && telemetry && ueServiceReport && cmdLifecycle),
                    "--lm=ts|tpc needs --ric --telemetry --ue-service-report and --cmd-lifecycle");
    bool stepViaRic = useRic && lm == "txstep";
    if (txStepTime >= 0 && !stepViaRic)
    {
        NS_ABORT_MSG_IF(txStepCell < 1 || txStepCell > 2, "tx-step-cell must be 1 or 2");
        Simulator::Schedule(Seconds(txStepTime),
                            &SetEnbTxPower,
                            enbLteDevs.Get(txStepCell - 1)->GetObject<LteEnbNetDevice>(),
                            txStepCell,
                            enbTxPower + txStepDb);
    }

    // ---------------- Traffic: CBR/UDP downlink, one flow per UE ----------------
    g_winBytes.assign(numberOfUes, 0);
    g_totBytes.assign(numberOfUes, 0);
    g_offeredMbps.resize(numberOfUes);
    g_statsStart = trafficStart + warmup;

    Ptr<UniformRandomVariable> startJitter = CreateObject<UniformRandomVariable>();
    startJitter->SetAttribute("Min", DoubleValue(0.0));
    startJitter->SetAttribute("Max", DoubleValue(0.1));
    if (assignStreams >= 0)
    {
        startJitter->SetStream(assignStreams + 1);
        streamMap["traffic_start_jitter"] = std::to_string(assignStreams + 1);
    }

    // S1 passive metrics (must be opened before any Attach)
    {
        xapp::MetricsConfig mc;
        mc.out = out;
        mc.nUe = numberOfUes;
        mc.window = window;
        mc.windowOrigin = trafficStart;
        mc.telemetry = telemetry && useRic;
        mc.statsStart = trafficStart + warmup;
        mc.simTime = simTime;
        mc.acctGuard = acctGuard;
        mc.nRbConfig = bandwidthRb;
        // PF Type-0 allocation (pf-ff-mac-scheduler.cc:40-45, 469-470): RBG size from
        // 36.213 Table 7.1.6.1-1, and only floor(N_RB / RBG) whole RBGs are allocated.
        uint16_t rbg = bandwidthRb < 10 ? 1 : bandwidthRb < 26 ? 2 : bandwidthRb < 63 ? 3 : 4;
        mc.nRbSchedulable = (scheduler == "ns3::PfFfMacScheduler")
                                ? static_cast<uint16_t>((bandwidthRb / rbg) * rbg)
                                : bandwidthRb;
        mc.rawPrb = tracePrbSubframes;
        for (uint32_t i = 0; i < numberOfUes; i++)
        {
            mc.imsi.push_back(Imsi(i));
        }
        xapp::MetricsOpen(mc);
    }

    uint16_t basePort = 10000;
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        g_offeredMbps[i] = (ueEnb[i] == 0) ? rateA : rateB;
        if (!layoutOffered.empty() && layoutOffered[i] >= 0)
        {
            g_offeredMbps[i] = layoutOffered[i];
        }
        if (targetRate >= 0 && g_group[i] == "target")
        {
            g_offeredMbps[i] = targetRate;
        }

        UdpServerHelper server(basePort + i);
        ApplicationContainer serverApp = server.Install(ueNodes.Get(i));
        serverApp.Start(Seconds(0.5));
        serverApp.Stop(simTime);
        DynamicCast<UdpServer>(serverApp.Get(0))
            ->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxPacket, i));

        if (g_offeredMbps[i] <= 0)
        {
            xapp::MetricsAttachFlow(i, nullptr, DynamicCast<UdpServer>(serverApp.Get(0)));
            continue;
        }
        UdpClientHelper client(ueIpIfaces.GetAddress(i), basePort + i);
        client.SetAttribute("Interval",
                            TimeValue(Seconds(packetSize * 8.0 / (g_offeredMbps[i] * 1e6))));
        client.SetAttribute("PacketSize", UintegerValue(packetSize));
        client.SetAttribute("MaxPackets", UintegerValue(std::numeric_limits<uint32_t>::max()));
        ApplicationContainer clientApp = client.Install(remoteHost);
        clientApp.Start(trafficStart + Seconds(startJitter->GetValue()));
        clientApp.Stop(simTime);
        xapp::MetricsAttachFlow(i,
                                DynamicCast<UdpClient>(clientApp.Get(0)),
                                DynamicCast<UdpServer>(serverApp.Get(0)));
        double after = (ueEnb[i] == 0) ? rateAAfter : rateBAfter;
        if (rateStepTime >= 0 && after >= 0)
        {
            Ptr<Application> app = clientApp.Get(0);
            if (after == 0)
            {
                app->SetStopTime(Seconds(rateStepTime));
            }
            else
            {
                // UdpClient reschedules each send with the current m_interval (udp-client.cc:249)
                Simulator::Schedule(Seconds(rateStepTime),
                                    [app, after, packetSize]() {
                                        app->SetAttribute(
                                            "Interval",
                                            TimeValue(Seconds(packetSize * 8.0 / (after * 1e6))));
                                    });
            }
            xapp::MetricsCtrlEvent("load_step_scheduled", ueEnb[i] + 1, Imsi(i), -1, -1,
                                   rateStepTime, g_offeredMbps[i], NAN, after, "");
        }
    }

    // S1 radio, PRB and handover instrumentation
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        xapp::MetricsAttachUeRadio(i, ueLteDevs.Get(i)->GetObject<LteUeNetDevice>());
    }
    xapp::MetricsAttachDlChannel(lteHelper->GetDownlinkSpectrumChannel());
    for (uint32_t k = 0; k < enbLteDevs.GetN(); k++)
    {
        Ptr<LteEnbNetDevice> enb = enbLteDevs.Get(k)->GetObject<LteEnbNetDevice>();
        xapp::MetricsAttachEnbMac(enb->GetCellId(), enb);
    }
    xapp::MetricsAttachHandoverTraces();
    for (uint32_t k = 0; k < enbLteDevs.GetN(); k++)
    {
        xapp::MetricsAttachEnbRlc(enbLteDevs.Get(k)->GetObject<LteEnbNetDevice>());
        xapp::MetricsAttachEnbUlSinr(enbLteDevs.Get(k)->GetObject<LteEnbNetDevice>());
    }
    if (hoTime >= 0)
    {
        NS_ABORT_MSG_IF(hoUe >= numberOfUes, "--ho-ue out of range");
        // Guarded: the source is the UE's CURRENT serving cell; the request is skipped (and
        // logged) if the UE is not connected or already served by the target.
        uint16_t hoTargetCellArg = hoTargetCell; // 0 = the cell the UE is not served by
        Simulator::Schedule(Seconds(hoTime), [lteHelper, ueLteDevs, enbLteDevs, hoUe, hoTargetCellArg]() {
            Ptr<LteUeRrc> rrc = ueLteDevs.Get(hoUe)->GetObject<LteUeNetDevice>()->GetRrc();
            uint16_t src = rrc->GetCellId();
            uint16_t hoTargetCell = hoTargetCellArg != 0 ? hoTargetCellArg : (src == 1 ? 2 : 1);
            Ptr<NetDevice> srcDev;
            for (uint32_t k = 0; k < enbLteDevs.GetN(); k++)
            {
                if (enbLteDevs.Get(k)->GetObject<LteEnbNetDevice>()->GetCellId() == src)
                {
                    srcDev = enbLteDevs.Get(k);
                }
            }
            // Validity rule V1 (docs/methodology/command-semantics.md): ns-3 aborts on a handover
            // request unless BOTH the UE-side RRC and the source eNB's UeManager are
            // CONNECTED_NORMALLY (lte-enb-rrc.cc PrepareHandover); the two can disagree transiently.
            std::string reason;
            if (src == hoTargetCell)
            {
                reason = "ho_target_is_serving";
            }
            else if (rrc->GetState() != LteUeRrc::CONNECTED_NORMALLY)
            {
                reason = "ue_state_not_connected_normally";
            }
            else if (!srcDev)
            {
                reason = "source_cell_unknown";
            }
            else
            {
                Ptr<LteEnbRrc> enbRrc = srcDev->GetObject<LteEnbNetDevice>()->GetRrc();
                uint16_t rnti = rrc->GetRnti();
                if (!enbRrc->HasUeManager(rnti))
                {
                    reason = "ue_context_absent";
                }
                else if (enbRrc->GetUeManager(rnti)->GetState() != UeManager::CONNECTED_NORMALLY)
                {
                    reason = "enb_state_not_connected_normally";
                }
            }
            if (!reason.empty())
            {
                xapp::MetricsCtrlEvent("scripted_ho_rejected", src, Imsi(hoUe), hoTargetCell, -1, NAN,
                                       NAN, NAN, NAN, reason);
                return;
            }
            xapp::MetricsCtrlEvent("scripted_ho_request", src, Imsi(hoUe), hoTargetCell, -1, NAN,
                                   NAN, NAN, NAN, "");
            lteHelper->HandoverRequest(Seconds(0), ueLteDevs.Get(hoUe), srcDev, hoTargetCell);
        });
    }

    // ---------------- Output files ----------------
    g_tputCsv.open(out + "-tput.csv");
    g_tputCsv << "time_s,ue,imsi,group,cell_id,rx_bytes,tput_mbps,offered_mbps\n";
    g_hoCsv.open(out + "-ho.csv");
    g_hoCsv << "time_s,imsi,target_cell\n";
    g_powerCsv.open(out + "-power.csv");
    g_powerCsv << "time_s,cell,old_dbm,new_dbm,source\n";
    g_powerCsv << "0.0000,1," << enbTxPower << "," << enbTxPower << ",initial\n";
    g_powerCsv << "0.0000,2," << enbTxPower << "," << enbTxPower << ",initial\n";
    {
        std::ofstream ues(out + "-ues.csv");
        ues << "ue,imsi,group,x,y,initial_cell,offered_mbps\n";
        for (uint32_t i = 0; i < numberOfUes; i++)
        {
            ues << i << "," << Imsi(i) << "," << g_group[i] << "," << uePos[i].x << ","
                << uePos[i].y << "," << (ueEnb[i] + 1) << "," << g_offeredMbps[i] << "\n";
        }
    }
    Simulator::Schedule(trafficStart + window, &SampleThroughput, window);

    // ---------------- ORAN models ----------------
    if (useRic)
    {
        Ptr<OranNearRtRic> nearRtRic = nullptr;
        OranE2NodeTerminatorContainer e2NodeTerminatorsEnbs;
        OranE2NodeTerminatorContainer e2NodeTerminatorsUes;
        Ptr<OranHelper> oranHelper = CreateObject<OranHelper>();
        std::ostringstream sendRvS;
        sendRvS << "ns3::ConstantRandomVariable[Constant=" << e2SendInterval << "]";
        std::string sendRv = sendRvS.str();

        oranHelper->SetAttribute("Verbose", BooleanValue(verbose));
        oranHelper->SetAttribute("LmQueryInterval", TimeValue(lmQueryInterval));
        oranHelper->SetAttribute("E2NodeInactivityThreshold", TimeValue(Seconds(2)));
        oranHelper->SetAttribute("E2NodeInactivityIntervalRv",
                                 StringValue("ns3::ConstantRandomVariable[Constant=2]"));
        oranHelper->SetAttribute("LmQueryMaxWaitTime", TimeValue(maxWaitTime));
        oranHelper->SetAttribute("LmQueryLateCommandPolicy", StringValue(lateCommandPolicy));
        if (procDelay >= 0)
        {
            std::ostringstream pd;
            pd << std::setprecision(17) << "ns3::ConstantRandomVariable[Constant=" << procDelay << "]";
            processingDelayRv = pd.str();
        }
        if (e2CmdDelay >= 0)
        {
            std::ostringstream td;
            td << std::setprecision(17) << "ns3::ConstantRandomVariable[Constant=" << e2CmdDelay << "]";
            oranHelper->SetAttribute("RicTransmissionDelayRv", StringValue(td.str()));
        }

        if (!dbFileName.empty())
        {
            std::remove(dbFileName.c_str());
        }
        oranHelper->SetDataRepository("ns3::OranDataRepositorySqlite",
                                      "DatabaseFile",
                                      StringValue(dbFileName));
        if (xappLm)
        {
            // S4 independent xApps (decision logic: simulation/xapps/xapp-policy.cc); parameters via
            // --ns3::OranLmXappTs::<Attr>=... / --ns3::OranLmXappTpc::<Attr>=..., recorded in config.json
            if (lm == "dual")
            {
                // S5 prep diagnostic: both xApps decide on the same telemetry; at most one executes, the other
                // logs shadow proposals only. No arbitration is involved (OranCmmNoopLogged passes commands).
                oranHelper->SetDefaultLogicModule("ns3::OranLmXappTs", "ProcessingDelayRv", StringValue(processingDelayRv),
                                                  "ShadowOnly", BooleanValue(dualExec != "ts" && dualExec != "both"));
                oranHelper->AddLogicModule("ns3::OranLmXappTpc", "ProcessingDelayRv", StringValue(processingDelayRv),
                                           "ShadowOnly", BooleanValue(dualExec != "tpc" && dualExec != "both"));
            }
            else
            {
                oranHelper->SetDefaultLogicModule(lm == "ts" ? "ns3::OranLmXappTs" : "ns3::OranLmXappTpc",
                                                  "ProcessingDelayRv",
                                                  StringValue(processingDelayRv));
            }
            xapp::ric::OpenDecisionLog(out);
        }
        else if (lm == "script")
        {
            // S3 test fixture: scripted commands for actuator / validity / timing tests
            Config::SetDefault("ns3::OranLmScriptedCommands::Script", StringValue(cmdScript));
            oranHelper->SetDefaultLogicModule("ns3::OranLmScriptedCommands",
                                              "ProcessingDelayRv",
                                              StringValue(processingDelayRv));
        }
        else if (lm == "txstep")
        {
            // Phase 2b test xApp: one power command to tx-step-cell at the first cycle >= tx-step-time
            Config::SetDefault("ns3::OranLmTxPowerStep::StepTime",
                               TimeValue(Seconds(txStepTime >= 0 ? txStepTime : 15.0)));
            Config::SetDefault("ns3::OranLmTxPowerStep::TargetCellId", UintegerValue(txStepCell));
            Config::SetDefault("ns3::OranLmTxPowerStep::TxPowerDbm",
                               DoubleValue(enbTxPower + txStepDb));
            oranHelper->SetDefaultLogicModule("ns3::OranLmTxPowerStep",
                                              "ProcessingDelayRv",
                                              StringValue(processingDelayRv));
        }
        else if (lm == "noop")
        {
            // S1 matched-run control: RIC fully active (reports, queries) but issues no command.
            oranHelper->SetDefaultLogicModule("ns3::OranLmNoop",
                                              "ProcessingDelayRv",
                                              StringValue(processingDelayRv));
        }
        else
        {
            // Placeholder xApp for Phase 1: hands a UE to its closest eNB.
            // Replaced by the TS and TPC xApps in Phase 3.
            oranHelper->SetDefaultLogicModule("ns3::OranLmLte2LteDistanceHandover",
                                              "ProcessingDelayRv",
                                              StringValue(processingDelayRv));
        }
        if (cmdLifecycle)
        {
            // OranCmmNoop behaviour (same output list) + lifecycle logging + optional fixed hold
            if (coordMode == "none")
            {
                oranHelper->SetConflictMitigationModule("ns3::OranCmmNoopLogged",
                                                        "CoordinationDelay",
                                                        TimeValue(Seconds(coordDelay)));
            }
            else
            {
                // S5: B3 / B4 (detector + resolution in simulation/xapps/coord-policy.cc)
                oranHelper->SetConflictMitigationModule("ns3::OranCmmCoord", "Mode", StringValue(coordMode),
                                                        "ReverseOrder", BooleanValue(coordReverse),
                                                        "CoordinationDelay", TimeValue(Seconds(coordDelay)),
                                                        "InteractionLog", StringValue(interactLog ? out + "-interact.csv" : ""),
                                                        "B5Log", StringValue(coordMode == "b5" ? out + "-b5.csv" : ""));
                // S5 state-aware variants (attributes beyond the helper's arity)
                const bool saMode = coordMode == "b4sa-ts" || coordMode == "b4sa-tpc" || coordMode == "b5sa";
                Config::SetDefault("ns3::OranCmmCoord::SaDefinition", StringValue(saDef));
                Config::SetDefault("ns3::OranCmmCoord::SaLog", StringValue(saMode ? out + "-sa.csv" : ""));
                Config::SetDefault("ns3::OranCmmCoord::B5saLog", StringValue(coordMode == "b5sa" ? out + "-b5sa.csv" : ""));
                Config::SetDefault("ns3::OranCmmCoord::B5saPredLog",
                                   StringValue(coordMode == "b5sa" ? out + "-b5sapred.csv" : ""));
                Config::SetDefault("ns3::OranCmmCoord::B5Shadow", BooleanValue(b5Shadow));
            }
            xapp::cmdlife::Open(out);
            xapp::cmdlife::RegisterDevices(ueLteDevs, enbLteDevs);
            xapp::cmdlife::RegisterX2(1, 2); // AddX2Interface(enbNodes): the two eNBs
            xapp::cmdlife::AttachHandoverTraces();
            xapp::MetricsSetPhyPowerHook(&xapp::cmdlife::PhyPowerObserved);
        }
        else
        {
            NS_ABORT_MSG_IF(coordDelay > 0, "--coord-delay needs --cmd-lifecycle=true");
            oranHelper->SetConflictMitigationModule("ns3::OranCmmNoop");
        }
        if (telemetry)
        {
            oranHelper->AddQueryTrigger("telemetry-sink", "ns3::OranQueryTriggerTelemetrySink");
        }

        nearRtRic = oranHelper->CreateNearRtRic();
        if (estProbe.t >= 0)
        {
            // scheduled at set-up, so at t it runs before any report arriving at the same instant: the probe sees
            // exactly what an xApp querying at t sees (decision-time input age as in the study)
            NS_ABORT_MSG_IF(!(telemetry && ueServiceReport && cmdLifecycle),
                            "--est-probe-time needs --telemetry --ue-service-report --cmd-lifecycle");
            estProbe.out = out;
            Simulator::Schedule(Seconds(estProbe.t), &EstimatorProbe, estProbe);
        }
        if (xappLm)
        {
            PointerValue pv;
            nearRtRic->GetAttribute("DefaultLogicModule", pv);
            g_xappLm = pv.Get<Object>();
        }

        oranHelper->SetE2NodeTerminator("ns3::OranE2NodeTerminatorLteUe",
                                        "RegistrationIntervalRv",
                                        StringValue("ns3::ConstantRandomVariable[Constant=1]"),
                                        "SendIntervalRv",
                                        StringValue(sendRv));
        oranHelper->AddReporter("ns3::OranReporterLocation",
                                "Trigger",
                                StringValue("ns3::OranReportTriggerPeriodic"));
        oranHelper->AddReporter(
            "ns3::OranReporterLteUeCellInfo",
            "Trigger",
            StringValue("ns3::OranReportTriggerLteUeHandover[InitialReport=true]"));

        e2NodeTerminatorsUes.Add(oranHelper->DeployTerminators(nearRtRic, ueNodes));
        if (telemetry)
        {
            // UE PHY RSRP/RSRQ measurements (every UeMeasurementsFilterPeriod, 200 ms), buffered by
            // the reporter and handed to the terminator on each periodic trigger. ns3-oran does not
            // connect this reporter to the PHY itself; wired as in the NIST example
            // oran-lte-2-lte-rsrp-handover-lm-example.cc (lines 375-395).
            if (ueServiceReport)
            {
                // S4: UE measurement reports on the same period as the eNB reports (the reporter's
                // default periodic trigger is 1 s); legacy runs keep the 1 s default
                std::ostringstream iv;
                iv << "ns3::ConstantRandomVariable[Constant=" << telemetryInterval << "]";
                Config::SetDefault("ns3::OranReportTriggerPeriodic::IntervalRv", StringValue(iv.str()));
            }
            for (uint32_t i = 0; i < e2NodeTerminatorsUes.GetN(); i++)
            {
                Ptr<OranE2NodeTerminator> term = e2NodeTerminatorsUes.Get(i);
                Ptr<OranReporterLteUeRsrpRsrq> rep = CreateObject<OranReporterLteUeRsrpRsrq>();
                rep->SetAttribute("Terminator", PointerValue(term));
                term->AddReporter(rep);
                Ptr<Node> n = term->GetNode();
                for (uint32_t d = 0; d < n->GetNDevices(); d++)
                {
                    if (auto ueDev = DynamicCast<LteUeNetDevice>(n->GetDevice(d)))
                    {
                        ueDev->GetPhy()->TraceConnectWithoutContext(
                            "ReportUeMeasurements",
                            MakeBoundCallback(&ForwardUeMeasurement, rep));
                    }
                }
            }
        }

        if (telemetry && ueServiceReport)
        {
            Config::SetDefault("ns3::OranReportTriggerPeriodic::IntervalRv",
                               StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        }
        // eNB terminator that also executes transmit-power commands (handover still works)
        Config::SetDefault("ns3::OranE2NodeTerminatorLteEnbTpc::MinTxPowerDbm", DoubleValue(txMin));
        Config::SetDefault("ns3::OranE2NodeTerminatorLteEnbTpc::MaxTxPowerDbm", DoubleValue(txMax));
        oranHelper->SetE2NodeTerminator("ns3::OranE2NodeTerminatorLteEnbTpc",
                                        "RegistrationIntervalRv",
                                        StringValue("ns3::ConstantRandomVariable[Constant=1]"),
                                        "SendIntervalRv",
                                        StringValue(sendRv));
        oranHelper->AddReporter("ns3::OranReporterLocation",
                                "Trigger",
                                StringValue("ns3::OranReportTriggerPeriodic"));
        if (telemetry)
        {
            // Nested attribute strings are not parsed through AddReporter's PointerValue, so the
            // periodic-trigger interval is set as a default for the eNB deployment only (it also
            // applies to the eNB location reporter; eNBs are static) and restored afterwards.
            std::ostringstream iv;
            iv << "ns3::ConstantRandomVariable[Constant=" << telemetryInterval << "]";
            Config::SetDefault("ns3::OranReportTriggerPeriodic::IntervalRv", StringValue(iv.str()));
            Config::SetDefault("ns3::OranReporterLteEnbLoad::ReportTxPower", BooleanValue(ueServiceReport));
            oranHelper->AddReporter("ns3::OranReporterLteEnbLoad",
                                    "Trigger",
                                    StringValue("ns3::OranReportTriggerPeriodic"));
            if (ueServiceReport)
            {
                // S4 prep: per-UE DL service telemetry (same trigger interval as the load report)
                oranHelper->AddReporter("ns3::OranReporterLteUeService",
                                        "Trigger",
                                        StringValue("ns3::OranReportTriggerPeriodic"));
            }
        }
        e2NodeTerminatorsEnbs.Add(oranHelper->DeployTerminators(nearRtRic, enbNodes));
        if (telemetry)
        {
            Config::SetDefault("ns3::OranReportTriggerPeriodic::IntervalRv",
                               StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        }
        for (uint32_t i = 0; i < e2NodeTerminatorsEnbs.GetN(); i++)
        {
            e2NodeTerminatorsEnbs.Get(i)->TraceConnectWithoutContext(
                "TxPowerChange",
                MakeCallback(&RicPowerChange));
            e2NodeTerminatorsEnbs.Get(i)->TraceConnectWithoutContext(
                "TxPowerCommand",
                MakeCallback(&xapp::MetricsTpcCommand));
        }

        if (dbLog)
        {
            nearRtRic->Data()->TraceConnectWithoutContext("QueryRc", MakeCallback(&QueryRcSink));
        }

        if (testDeactivateCell >= 0 && testDeactivateTime >= 0)
        {
            for (uint32_t i = 0; i < e2NodeTerminatorsEnbs.GetN(); i++)
            {
                Ptr<OranE2NodeTerminator> t = e2NodeTerminatorsEnbs.Get(i);
                auto ed = DynamicCast<LteEnbNetDevice>(t->GetNode()->GetDevice(0));
                for (uint32_t d = 0; !ed && d < t->GetNode()->GetNDevices(); d++)
                {
                    ed = DynamicCast<LteEnbNetDevice>(t->GetNode()->GetDevice(d));
                }
                if (ed && ed->GetCellId() == testDeactivateCell)
                {
                    auto tpc = DynamicCast<OranE2NodeTerminatorLteEnbTpc>(t);
                    NS_ABORT_MSG_IF(!tpc, "test-deactivate needs the TPC eNB terminator");
                    Simulator::Schedule(Seconds(testDeactivateTime),
                                        &OranE2NodeTerminatorLteEnbTpc::TestSetInactive, tpc);
                }
            }
        }
        NS_ABORT_MSG_IF(ricStart <= 0 || ricStart >= 1.5, "--ric-start must lie in (0, 1.5) s (before E2 node activation)");
        Simulator::Schedule(Seconds(ricStart), &OranHelper::ActivateAndStartNearRtRic, oranHelper, nearRtRic);
        Simulator::Schedule(Seconds(1.5),
                            &OranHelper::ActivateE2NodeTerminators,
                            oranHelper,
                            e2NodeTerminatorsEnbs);
        Simulator::Schedule(Seconds(2),
                            &OranHelper::ActivateE2NodeTerminators,
                            oranHelper,
                            e2NodeTerminatorsUes);
    }

    Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                    MakeCallback(&NotifyHandoverEndOkEnb));

    // ---------------- S1: resolved configuration and RNG provenance ----------------
    {
        std::ofstream cj(out + "-config.json");
        cj << "{\n  \"program\": \"xapp-coord\",\n  \"argv\": [";
        for (size_t k = 0; k < argvCopy.size(); k++)
        {
            cj << (k ? ", " : "") << "\"" << JsonEscape(argvCopy[k]) << "\"";
        }
        cj << "],\n  \"params\": {\n";
        std::vector<std::pair<std::string, std::string>> kv = {
            {"n_near_a", std::to_string(nNearA)}, {"n_edge", std::to_string(nEdge)},
            {"n_near_b", std::to_string(nNearB)}, {"isd_m", std::to_string(isd)},
            {"near_frac", std::to_string(nearFrac)}, {"edge_frac", std::to_string(edgeFrac)},
            {"pos_jitter_m", std::to_string(posJitter)},
            {"enb_tx_power_dbm", std::to_string(enbTxPower)},
            {"pathloss_model", radio == "R0" ? "ns3::LogDistancePropagationLossModel" : "see radio"},
            {"pathloss_exp", std::to_string(pathlossExp)},
            {"bandwidth_rb", std::to_string(bandwidthRb)}, {"scheduler", scheduler},
            {"handover_algorithm", "ns3::NoOpHandoverAlgorithm"},
            {"tx_step_time_s", std::to_string(txStepTime)},
            {"tx_step_cell", std::to_string(txStepCell)}, {"tx_step_db", std::to_string(txStepDb)},
            {"tx_min_dbm", std::to_string(txMin)}, {"tx_max_dbm", std::to_string(txMax)},
            {"lm", lm}, {"rate_a_mbps", std::to_string(rateA)},
            {"rate_b_mbps", std::to_string(rateB)}, {"packet_size_bytes", std::to_string(packetSize)},
            {"traffic_start_s", std::to_string(trafficStart.GetSeconds())},
            {"warmup_s", std::to_string(warmup.GetSeconds())},
            {"window_s", std::to_string(window.GetSeconds())},
            {"sim_time_s", std::to_string(simTime.GetSeconds())}, {"ric", useRic ? "true" : "false"},
            {"max_wait_time_s", std::to_string(maxWaitTime.GetSeconds())},
            {"processing_delay_rv", processingDelayRv},
            {"lm_query_interval_s", std::to_string(lmQueryInterval.GetSeconds())},
            {"late_command_policy", lateCommandPolicy}, {"layout_file", layoutFile},
            {"acct_guard_s", std::to_string(acctGuard.GetSeconds())},
            {"trace_prb_subframes", tracePrbSubframes ? "true" : "false"},
            {"test_rng_perturb", std::to_string(testRngPerturb)},
            {"anr", anr ? "true" : "false"}, {"radio", radio},
            {"pathloss_model_effective",
             radio == "R0" ? "ns3::LogDistancePropagationLossModel"
                           : "ns3::ThreeGppUmaPropagationLossModel+NeverLos+Shadowing(no BPL)"},
            {"enb_height_m", std::to_string(enbHeight)},
            {"dl_frequency_hz", std::to_string(dlFrequencyHz)},
            {"ul_frequency_hz", std::to_string(ulFrequencyHz)},
            {"rate_step_time_s", std::to_string(rateStepTime)},
            {"rate_a_after_mbps", std::to_string(rateAAfter)},
            {"rate_b_after_mbps", std::to_string(rateBAfter)},
            {"ho_time_s", std::to_string(hoTime)}, {"ho_ue", std::to_string(hoUe)},
            {"ho_target_cell", std::to_string(hoTargetCell)},
            {"telemetry", telemetry ? "true" : "false"},
            {"telemetry_interval_s", std::to_string(telemetryInterval)},
            {"e2_send_interval_s", std::to_string(e2SendInterval)},
            {"target_rate_mbps", std::to_string(targetRate)}, {"attach", attach},
            {"r1_shadowing", r1Shadowing ? "true" : "false"}, {"r1_shadowing_mode", r1ShadowingMode},
            {"window_origin_s", std::to_string(trafficStart.GetSeconds())},
            {"n_ue", std::to_string(numberOfUes)},
            {"ue_service_report", ueServiceReport ? "true" : "false"},
            {"cmd_lifecycle", cmdLifecycle ? "true" : "false"},
            {"proc_delay_s", std::to_string(procDelay)}, {"e2_cmd_delay_s", std::to_string(e2CmdDelay)},
            {"coord_delay_s", std::to_string(coordDelay)},
            {"attach_layout_file", attachLayoutFile}, {"attach_move_time_s", std::to_string(attachMoveTime)},
            {"dual_exec", dualExec}, {"coord", coordMode}, {"coord_reverse", coordReverse ? "true" : "false"},
            {"ctrl_activation", ctrlActivation},
            {"ctrl_admission_start_s", ctrlActivation == "warmup" ? std::to_string((trafficStart + warmup).GetSeconds()) : "none"},
            {"interact_log", interactLog ? "true" : "false"}, {"sa_def", saDef}, {"b5_shadow", b5Shadow ? "true" : "false"},
            {"est_probe_time_s", std::to_string(estProbe.t)}, {"est_ts_ue", std::to_string(estProbe.tsUe)},
            {"est_ts_target", std::to_string(estProbe.tsTarget)}, {"est_tpc_cell", std::to_string(estProbe.tpcCell)},
            {"est_step_db", std::to_string(estProbe.stepDb)}, {"est_kappa", std::to_string(estProbe.kappa)},
            {"est_kappa_grid", estProbe.kappaGrid ? "true" : "false"},
            {"b5_params", B5ParamString()}};
        if (ricStart != 1.0)
        {
            kv.emplace_back("ric_start_s", std::to_string(ricStart)); // recorded only when non-default (byte identity)
        }
        if (g_xappLm)
        {
            // every attribute of the xApp LM, as resolved at run time
            TypeId tid = g_xappLm->GetInstanceTypeId();
            for (uint32_t a = 0; a < tid.GetAttributeN(); a++)
            {
                TypeId::AttributeInformation info = tid.GetAttribute(a);
                Ptr<AttributeValue> v = info.checker->Create();
                if (g_xappLm->GetAttributeFailSafe(info.name, *v))
                {
                    kv.emplace_back("xapp." + tid.GetName() + "." + info.name, v->SerializeToString(info.checker));
                }
            }
        }
        for (size_t k = 0; k < kv.size(); k++)
        {
            cj << "    \"" << kv[k].first << "\": \"" << JsonEscape(kv[k].second) << "\""
               << (k + 1 < kv.size() ? ",\n" : "\n");
        }
        cj << "  },\n  \"rng\": {\n    \"seed\": " << RngSeedManager::GetSeed()
           << ",\n    \"run\": " << RngSeedManager::GetRun()
           << ",\n    \"assign_streams_base\": " << assignStreams << ",\n    \"stream_map\": {";
        size_t k = 0;
        for (auto& [name, range] : streamMap)
        {
            cj << (k++ ? ", " : "") << "\"" << name << "\": \"" << range << "\"";
        }
        cj << "},\n    \"automatic_streams_note\": \"processes not in stream_map (ns3-oran RVs, "
              "and everything when assign_streams_base = -1) use ns-3 automatic streams\"\n  }\n}\n";
    }

    Simulator::Stop(simTime);
    Simulator::Run();
    xapp::cmdlife::Close(); // closure: every command ends with a logged outcome
    xapp::ric::CloseDecisionLog();

    // ---------------- Summary (after warm-up) ----------------
    double statsSec = (simTime - g_statsStart).GetSeconds();
    std::map<uint16_t, double> cellOffered;
    std::map<uint16_t, double> cellDelivered;
    std::cout << "\nUE  group  cell  offered_Mbps  delivered_Mbps\n";
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        double delivered = g_totBytes[i] * 8.0 / statsSec / 1e6;
        uint16_t cell = ServingCell(i);
        cellOffered[cell] += g_offeredMbps[i];
        cellDelivered[cell] += delivered;
        std::cout << std::setw(2) << i << "  " << std::setw(5) << g_group[i] << "  " << std::setw(4)
                  << cell << "  " << std::setw(12) << std::fixed << std::setprecision(3)
                  << g_offeredMbps[i] << "  " << std::setw(14) << delivered << "\n";
    }
    std::cout << "\nCell  offered_Mbps  delivered_Mbps\n";
    for (auto& [cell, offered] : cellOffered)
    {
        std::cout << std::setw(4) << cell << "  " << std::setw(12) << offered << "  "
                  << std::setw(14) << cellDelivered[cell] << "\n";
    }

    xapp::MetricsFinish(g_group, g_offeredMbps);
    if (linkShadowing)
    {
        std::ofstream sh(out + "-shadowing.csv");
        linkShadowing->Dump(sh);
    }
    g_tputCsv.close();
    g_hoCsv.close();
    g_powerCsv.close();
    Simulator::Destroy();
    return 0;
}
