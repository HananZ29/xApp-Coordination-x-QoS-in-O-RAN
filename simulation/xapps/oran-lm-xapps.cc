/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S4: TS / TPC xApp Logic Modules (see header).
 */
#include "oran-lm-xapps.h"

#include "cmd-lifecycle.h"
#include "oran-command-lte-tx-power.h"
#include "ric-telemetry-store.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/integer.h"
#include "ns3/log.h"
#include "ns3/oran-command-lte-2-lte-handover.h"
#include "ns3/oran-data-repository.h"
#include "ns3/oran-near-rt-ric.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranLmXapps");
NS_OBJECT_ENSURE_REGISTERED(OranLmXappTs);
NS_OBJECT_ENSURE_REGISTERED(OranLmXappTpc);

namespace
{
std::map<uint16_t, uint64_t>
E2OfCell(Ptr<OranNearRtRic> ric)
{
    std::map<uint16_t, uint64_t> m;
    Ptr<OranDataRepository> data = ric->Data();
    for (auto id : data->GetLteEnbE2NodeIds())
    {
        bool found;
        uint16_t cell;
        std::tie(found, cell) = data->GetLteEnbCellInfo(id);
        if (found)
        {
            m[cell] = id;
        }
    }
    return m;
}

/** C-A: no decision while no admissible windowed report has reached the RIC (logged once per cycle). */
bool
InactiveWarmup(double now, const std::string& xapp)
{
    if (xapp::ric::AdmissionStart() < -1e17 || xapp::ric::AdmissibleReports() > 0)
    {
        return false;
    }
    xapp::ric::LogDecisions(now, {{xapp, "all", "inactive_warmup", "c_a",
                                   "no report with interval start >= " + std::to_string(xapp::ric::AdmissionStart())}});
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------------ TS
TypeId
OranLmXappTs::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranLmXappTs")
            .SetParent<OranLm>()
            .AddConstructor<OranLmXappTs>()
            .AddAttribute("ShadowOnly", "Decide and log shadow proposals only (no commands).", BooleanValue(false), MakeBooleanAccessor(&OranLmXappTs::m_shadowOnly), MakeBooleanChecker())
            .AddAttribute("HolOnMs", "Congestion: HOL at report time >= this (ms).", DoubleValue(78.0), MakeDoubleAccessor(&OranLmXappTs::m_holOnMs), MakeDoubleChecker<double>())
            .AddAttribute("PrbOn", "Congestion: prb_util_sched >= this.", DoubleValue(0.95), MakeDoubleAccessor(&OranLmXappTs::m_prbOn), MakeDoubleChecker<double>())
            .AddAttribute("KOn", "Consecutive congested reports.", IntegerValue(2), MakeIntegerAccessor(&OranLmXappTs::m_kOn), MakeIntegerChecker<int>())
            .AddAttribute("PrbTargetMax", "Target capacity: prb < this.", DoubleValue(0.959), MakeDoubleAccessor(&OranLmXappTs::m_prbTargetMax), MakeDoubleChecker<double>())
            .AddAttribute("HolTargetMaxMs", "Target capacity: HOL < this (ms).", DoubleValue(9.6), MakeDoubleAccessor(&OranLmXappTs::m_holTargetMaxMs), MakeDoubleChecker<double>())
            .AddAttribute("QinDb", "C2 threshold (ns-3 Qin, dB).", DoubleValue(-3.9), MakeDoubleAccessor(&OranLmXappTs::m_qinDb), MakeDoubleChecker<double>())
            .AddAttribute("NoisePerReDbm", "Noise per RE (dBm).", DoubleValue(-123.24), MakeDoubleAccessor(&OranLmXappTs::m_nReDbm), MakeDoubleChecker<double>())
            .AddAttribute("DwellS", "Per-UE dwell after a cell change (s).", DoubleValue(5.0), MakeDoubleAccessor(&OranLmXappTs::m_dwellS), MakeDoubleChecker<double>())
            .AddAttribute("ReturnBlockS", "No return to the previous cell within (s).", DoubleValue(10.0), MakeDoubleAccessor(&OranLmXappTs::m_returnBlockS), MakeDoubleChecker<double>())
            .AddAttribute("CooldownS", "Source-cell cooldown (s).", DoubleValue(1.0), MakeDoubleAccessor(&OranLmXappTs::m_cooldownS), MakeDoubleChecker<double>())
            .AddAttribute("MaxAgeS", "Inputs older than this are not used (s).", DoubleValue(1.0), MakeDoubleAccessor(&OranLmXappTs::m_maxAgeS), MakeDoubleChecker<double>())
            .AddAttribute("PendingS", "Pending-handover confirmation timeout (s).", DoubleValue(1.0), MakeDoubleAccessor(&OranLmXappTs::m_pendingS), MakeDoubleChecker<double>())
            .AddAttribute("MaxFailures", "Failed proposals before back-off.", IntegerValue(2), MakeIntegerAccessor(&OranLmXappTs::m_maxFailures), MakeIntegerChecker<int>())
            .AddAttribute("BackoffS", "Back-off after max failures (s).", DoubleValue(10.0), MakeDoubleAccessor(&OranLmXappTs::m_backoffS), MakeDoubleChecker<double>());
    return tid;
}

OranLmXappTs::OranLmXappTs()
    : OranLm()
{
    m_name = "XappTs";
}

OranLmXappTs::~OranLmXappTs() = default;

xapp::policy::TsParams
OranLmXappTs::Params() const
{
    xapp::policy::TsParams p;
    p.holOnMs = m_holOnMs;
    p.prbOn = m_prbOn;
    p.kOn = m_kOn;
    p.prbTargetMax = m_prbTargetMax;
    p.holTargetMaxMs = m_holTargetMaxMs;
    p.qinDb = m_qinDb;
    p.nReDbm = m_nReDbm;
    p.dwellS = m_dwellS;
    p.returnBlockS = m_returnBlockS;
    p.cooldownS = m_cooldownS;
    p.maxAgeS = m_maxAgeS;
    p.pendingS = m_pendingS;
    p.maxFailures = m_maxFailures;
    p.backoffS = m_backoffS;
    return p;
}

std::vector<Ptr<OranCommand>>
OranLmXappTs::Run()
{
    std::vector<Ptr<OranCommand>> out;
    m_inFlight.clear();
    if (!m_active)
    {
        return out;
    }
    double now = Simulator::Now().GetSeconds();
    if (InactiveWarmup(now, "ts"))
    {
        return out;
    }
    xapp::policy::Snapshot s = xapp::ric::BuildSnapshot(now);
    std::vector<xapp::policy::LogEntry> log;
    auto props = xapp::policy::TsDecide(s, Params(), m_st, log);
    if (m_shadowOnly)
    {
        // nothing is executed: undo the pending state a real proposal creates (the source cooldown is kept,
        // so the same opportunity is not re-logged every cycle) and mark the log entries as shadow
        for (const auto& p : props)
        {
            m_st.ue[p.imsi].pending = false;
        }
        for (auto& e : log)
        {
            e.decision = "shadow_" + e.decision;
        }
        xapp::ric::LogDecisions(now, log);
        return out;
    }
    auto e2 = E2OfCell(m_nearRtRic);
    for (const auto& p : props)
    {
        if (!e2.count(p.source))
        {
            log.push_back({"ts", "imsi " + std::to_string(p.imsi), "proposal_dropped", "source_not_registered", ""});
            continue;
        }
        auto cmd = CreateObject<OranCommandLte2LteHandover>();
        cmd->SetAttribute("TargetE2NodeId", UintegerValue(e2[p.source]));
        cmd->SetAttribute("TargetCellId", UintegerValue(p.target));
        cmd->SetAttribute("TargetRnti", UintegerValue(p.rnti));
        uint64_t id = xapp::cmdlife::Created(cmd, m_name, static_cast<int64_t>(p.imsi),
                                             "ts cell " + std::to_string(p.source) + "->" +
                                                 std::to_string(p.target) + " gamma_target " +
                                                 std::to_string(p.gammaTargetDb));
        // evidence about the target cell: the UE's RSRP of the target and the target's newest load report
        double ev = now;
        for (const auto& u : s.ues)
        {
            if (u.imsi == p.imsi)
            {
                auto a = u.rsrpAge.find(p.target);
                if (a != u.rsrpAge.end())
                {
                    ev = std::min(ev, now - a->second);
                }
            }
        }
        auto ly = s.load.find(p.target);
        if (ly != s.load.end() && !ly->second.empty())
        {
            ev = std::min(ev, ly->second[0].t0);
        }
        xapp::ric::RegisterEvidence(id, ev);
        out.push_back(cmd);
    }
    xapp::ric::LogDecisions(now, log);
    m_inFlight = out;
    return out;
}

void
OranLmXappTs::FinishRun()
{
    for (auto& c : m_inFlight)
    {
        xapp::cmdlife::Stage(c, "lm_output", "", "", "processing done");
    }
    m_inFlight.clear();
    OranLm::FinishRun();
}

// ----------------------------------------------------------------------------------------- TPC
TypeId
OranLmXappTpc::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranLmXappTpc")
            .SetParent<OranLm>()
            .AddConstructor<OranLmXappTpc>()
            .AddAttribute("ShadowOnly", "Decide and log shadow proposals only (no commands).", BooleanValue(false), MakeBooleanAccessor(&OranLmXappTpc::m_shadowOnly), MakeBooleanChecker())
            .AddAttribute("NominalDbm", "Nominal (maximum) transmit power (dBm).", DoubleValue(30.0), MakeDoubleAccessor(&OranLmXappTpc::m_pNomDbm), MakeDoubleChecker<double>())
            .AddAttribute("MinDbm", "Lowest permitted policy power (dBm).", DoubleValue(24.0), MakeDoubleAccessor(&OranLmXappTpc::m_pMinDbm), MakeDoubleChecker<double>())
            .AddAttribute("StepDb", "Power step (dB).", DoubleValue(3.0), MakeDoubleAccessor(&OranLmXappTpc::m_stepDb), MakeDoubleChecker<double>())
            .AddAttribute("PrbLow", "Low-load condition: prb_util_sched < this.", DoubleValue(0.5), MakeDoubleAccessor(&OranLmXappTpc::m_prbLow), MakeDoubleChecker<double>())
            .AddAttribute("HolHeadroomMs", "Headroom: HOL < this (ms) and no RLC drops.", DoubleValue(9.6), MakeDoubleAccessor(&OranLmXappTpc::m_holHeadroomMs), MakeDoubleChecker<double>())
            .AddAttribute("KDown", "Consecutive quiet reports before a reduction.", IntegerValue(4), MakeIntegerAccessor(&OranLmXappTpc::m_kDown), MakeIntegerChecker<int>())
            .AddAttribute("DwellS", "Minimum time between a change and an ordinary reduction (s).", DoubleValue(2.0), MakeDoubleAccessor(&OranLmXappTpc::m_dwellS), MakeDoubleChecker<double>())
            .AddAttribute("Alpha", "Per-UE QoS target for the I3 indicator.", DoubleValue(0.95), MakeDoubleAccessor(&OranLmXappTpc::m_alpha), MakeDoubleChecker<double>())
            .AddAttribute("QinDb", "C1/C4 threshold (ns-3 Qin, dB).", DoubleValue(-3.9), MakeDoubleAccessor(&OranLmXappTpc::m_qinDb), MakeDoubleChecker<double>())
            .AddAttribute("NoisePerReDbm", "Noise per RE (dBm).", DoubleValue(-123.24), MakeDoubleAccessor(&OranLmXappTpc::m_nReDbm), MakeDoubleChecker<double>())
            .AddAttribute("MaxAgeS", "Inputs older than this are not used (s).", DoubleValue(1.0), MakeDoubleAccessor(&OranLmXappTpc::m_maxAgeS), MakeDoubleChecker<double>());
    return tid;
}

OranLmXappTpc::OranLmXappTpc()
    : OranLm()
{
    m_name = "XappTpc";
}

OranLmXappTpc::~OranLmXappTpc() = default;

xapp::policy::TpcParams
OranLmXappTpc::Params() const
{
    xapp::policy::TpcParams p;
    p.pNomDbm = m_pNomDbm;
    p.pMinDbm = m_pMinDbm;
    p.stepDb = m_stepDb;
    p.prbLow = m_prbLow;
    p.holHeadroomMs = m_holHeadroomMs;
    p.kDown = m_kDown;
    p.dwellS = m_dwellS;
    p.alpha = m_alpha;
    p.qinDb = m_qinDb;
    p.nReDbm = m_nReDbm;
    p.maxAgeS = m_maxAgeS;
    return p;
}

std::vector<Ptr<OranCommand>>
OranLmXappTpc::Run()
{
    std::vector<Ptr<OranCommand>> out;
    m_inFlight.clear();
    if (!m_active)
    {
        return out;
    }
    double now = Simulator::Now().GetSeconds();
    if (InactiveWarmup(now, "tpc"))
    {
        return out;
    }
    xapp::policy::Snapshot s = xapp::ric::BuildSnapshot(now);
    std::vector<xapp::policy::LogEntry> log;
    xapp::policy::TpcState before = m_st;
    auto props = xapp::policy::TpcDecide(s, Params(), m_st, log);
    if (m_shadowOnly)
    {
        // nothing is executed: the cell power and dwell clock are unchanged; keep only "evidence used" so a
        // report yields at most one shadow proposal
        for (const auto& p : props)
        {
            double used = m_st.cell[p.cell].lastActedReport;
            m_st.cell[p.cell] = before.cell[p.cell];
            m_st.cell[p.cell].lastActedReport = used;
        }
        for (auto& e : log)
        {
            e.decision = "shadow_" + e.decision;
        }
        xapp::ric::LogDecisions(now, log);
        return out;
    }
    auto e2 = E2OfCell(m_nearRtRic);
    for (const auto& p : props)
    {
        if (!e2.count(p.cell))
        {
            log.push_back({"tpc", "cell " + std::to_string(p.cell), "proposal_dropped", "cell_not_registered", ""});
            continue;
        }
        auto cmd = CreateObject<OranCommandLteTxPower>();
        cmd->SetAttribute("TargetE2NodeId", UintegerValue(e2[p.cell]));
        cmd->SetAttribute("TxPowerDbm", DoubleValue(p.targetDbm));
        cmd->SetAttribute("RequestId", UintegerValue(m_nextRequestId++));
        cmd->SetAttribute("CreatedAt", TimeValue(Simulator::Now()));
        uint64_t id = xapp::cmdlife::Created(cmd, m_name, -1, "tpc " + p.kind + " (" + p.reason + ")");
        // evidence: a reduction uses the KDown newest load reports of the cell (TpcDecide, Consecutive);
        // a restoration the newest report (or nothing, if it restores because inputs are stale)
        const auto& rc = s.load[p.cell];
        double ev = now;
        if (p.kind == "reduce" && static_cast<int>(rc.size()) >= m_kDown)
        {
            ev = rc[m_kDown - 1].t0;
        }
        else if (!rc.empty())
        {
            ev = rc[0].t0;
        }
        xapp::ric::RegisterEvidence(id, ev);
        out.push_back(cmd);
    }
    xapp::ric::LogDecisions(now, log);
    m_inFlight = out;
    return out;
}

void
OranLmXappTpc::FinishRun()
{
    for (auto& c : m_inFlight)
    {
        xapp::cmdlife::Stage(c, "lm_output", "", "", "processing done");
    }
    m_inFlight.clear();
    OranLm::FinishRun();
}

} // namespace ns3
