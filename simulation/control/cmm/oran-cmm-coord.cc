/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: coordination CMM for B3 / B4 (see header).
 */
#include "oran-cmm-coord.h"

#include "cmd-lifecycle.h"
#include "b5-policy.h"
#include "b5sa-policy.h"
#include "coord-policy.h"
#include "rate-estimator.h"
#include "oran-command-lte-tx-power.h"
#include "ric-telemetry-store.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/oran-command-lte-2-lte-handover.h"
#include "ns3/oran-data-repository.h"
#include "ns3/oran-near-rt-ric-e2terminator.h"
#include "ns3/oran-near-rt-ric.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <set>
#include <tuple>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranCmmCoord");
NS_OBJECT_ENSURE_REGISTERED(OranCmmCoord);

TypeId
OranCmmCoord::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranCmmCoord")
            .SetParent<OranCmm>()
            .AddConstructor<OranCmmCoord>()
            .AddAttribute("Mode", "b3 | b4-ts | b4-tpc", StringValue("b3"),
                          MakeStringAccessor(&OranCmmCoord::m_mode), MakeStringChecker())
            .AddAttribute("ReverseOrder", "B3 sensitivity: dispatch all TPC before all TS.", BooleanValue(false),
                          MakeBooleanAccessor(&OranCmmCoord::m_reverse), MakeBooleanChecker())
            .AddAttribute("NominalDbm", "Cell power assumed when the eNB has not reported one.", DoubleValue(30.0),
                          MakeDoubleAccessor(&OranCmmCoord::m_nominalDbm), MakeDoubleChecker<double>())
            .AddAttribute("InteractionLog", "Shadow interaction diagnostic CSV (empty = off). Labels only.",
                          StringValue(""), MakeStringAccessor(&OranCmmCoord::m_interactionLog), MakeStringChecker())
            .AddAttribute("DiagPendingS", "Diagnostic: handover pending timeout at the RIC (TS PendingS).",
                          DoubleValue(1.0), MakeDoubleAccessor(&OranCmmCoord::m_diagPendingS),
                          MakeDoubleChecker<double>())
            .AddAttribute("B5Log", "B5 per-decision log CSV (empty = off).", StringValue(""),
                          MakeStringAccessor(&OranCmmCoord::m_b5Log), MakeStringChecker())
            .AddAttribute("SaDefinition", "State-aware modes: combined | win_2 | win_5", StringValue("combined"),
                          MakeStringAccessor(&OranCmmCoord::m_saDef), MakeStringChecker())
            .AddAttribute("SaLog", "State-aware per-proposal log CSV (empty = off).", StringValue(""),
                          MakeStringAccessor(&OranCmmCoord::m_saLog), MakeStringChecker())
            .AddAttribute("B5saLog", "B5sa per-decision log CSV (empty = off).", StringValue(""),
                          MakeStringAccessor(&OranCmmCoord::m_b5saLog), MakeStringChecker())
            .AddAttribute("B5saPredLog", "B5sa per-UE predictions CSV (empty = off).", StringValue(""),
                          MakeStringAccessor(&OranCmmCoord::m_b5saPredLog), MakeStringChecker())
            .AddAttribute("B5Shadow", "b5sa: evaluate and log, never act (estimator tests only).", BooleanValue(false),
                          MakeBooleanAccessor(&OranCmmCoord::m_b5Shadow), MakeBooleanChecker())
            .AddAttribute("MaxUncertain", "b5sa: max uncertain state items per decision (more: only N).",
                          UintegerValue(3), MakeUintegerAccessor(&OranCmmCoord::m_maxUncertain),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("CoordinationDelay", "Fixed hold between CMM input and dispatch (0 = none).",
                          TimeValue(Seconds(0)), MakeTimeAccessor(&OranCmmCoord::m_coordinationDelay),
                          MakeTimeChecker());
    return tid;
}

OranCmmCoord::OranCmmCoord()
    : OranCmm()
{
    m_name = "OranCmmCoord";
}

OranCmmCoord::~OranCmmCoord() = default;

std::vector<Ptr<OranCommand>>
OranCmmCoord::Filter(std::map<std::tuple<std::string, bool>, std::vector<Ptr<OranCommand>>> inputCommands)
{
    NS_ABORT_MSG_IF(m_mode != "b3" && m_mode != "b4-ts" && m_mode != "b4-tpc" && m_mode != "b5" && !StateAware(),
                    "OranCmmCoord: bad Mode " << m_mode);
    NS_ABORT_MSG_IF(m_nearRtRic == nullptr, "OranCmmCoord without RIC");
    Ptr<OranDataRepository> data = m_nearRtRic->Data();
    std::vector<Ptr<OranCommand>> cmds;
    std::vector<xapp::coord::Prop> props;
    for (const auto& kv : inputCommands)
    {
        for (const auto& c : kv.second)
        {
            xapp::cmdlife::Stage(c, "cmm_in", "", "", "from " + std::get<0>(kv.first) + " mode " + m_mode +
                                                           (m_reverse ? " reversed" : ""));
            xapp::coord::Prop p;
            bool found;
            uint16_t cell = 0;
            std::tie(found, cell) = data->GetLteEnbCellInfo(c->GetTargetE2NodeId());
            if (auto ho = DynamicCast<OranCommandLte2LteHandover>(c))
            {
                p.type = xapp::coord::Prop::TS;
                int64_t im = xapp::cmdlife::IntendedImsi(c);
                p.imsi = im > 0 ? static_cast<uint64_t>(im) : 0;
                p.src = found ? cell : 0;
                p.dst = ho->GetTargetCellId();
            }
            else if (auto tp = DynamicCast<OranCommandLteTxPower>(c))
            {
                p.type = xapp::coord::Prop::TPC;
                p.cell = found ? cell : 0;
                p.requestedDbm = tp->GetTxPowerDbm();
                double cur = xapp::ric::LatestTxPowerDbm(p.cell);
                p.currentDbm = std::isnan(cur) ? m_nominalDbm : cur;
            }
            cmds.push_back(c);
            props.push_back(p);
        }
    }
    for (size_t i = 0; i < cmds.size(); i++)
    {
        props[i].id = xapp::cmdlife::IdOf(cmds[i]);
        m_propOf[props[i].id] = props[i];
    }
    if (!m_interactionLog.empty())
    {
        Diagnose(cmds, props);
    }
    std::vector<xapp::coord::Prop> sp = props; // state-aware: TPC judged against the RIC-believed power
    if (StateAware())
    {
        for (auto& p : sp)
        {
            if (p.type == xapp::coord::Prop::TPC)
            {
                p.currentDbm = xapp::coord::BelievedPower(m_hist, p.cell, xapp::ric::LatestTxPowerDbm(p.cell),
                                                          xapp::ric::LatestLoadReportTime(p.cell), m_nominalDbm);
            }
        }
        props = sp;
    }
    xapp::coord::Mode mode = (m_mode == "b3" || m_mode == "b5" || StateAware()) ? xapp::coord::Mode::B3
                             : m_mode == "b4-ts" ? xapp::coord::Mode::B4_TS
                                                 : xapp::coord::Mode::B4_TPC;
    xapp::coord::Resolution r = xapp::coord::Resolve(props, mode, m_reverse);
    if (m_mode == "b5")
    {
        // B5: dispatch order as B3; each flagged pair (approved same-cycle detector) resolved by xapp::b5::Decide
        std::set<size_t> drop;
        std::vector<std::pair<size_t, std::string>> rej;
        for (const auto& pc : r.pairs)
        {
            if (!pc.flagged || drop.count(pc.ts) || drop.count(pc.tpc))
            {
                continue;
            }
            std::string choice = B5Resolve(props[pc.ts], props[pc.tpc]);
            std::string why = m_lastB5Reason;
            for (const auto& c : {cmds[pc.ts], cmds[pc.tpc]})
            {
                xapp::cmdlife::Stage(c, "b5_decision", choice, why, m_lastB5Detail);
            }
            if (choice == "N" || choice == "P")
            {
                drop.insert(pc.ts);
                rej.emplace_back(pc.ts, why);
            }
            if (choice == "N" || choice == "T")
            {
                drop.insert(pc.tpc);
                rej.emplace_back(pc.tpc, why);
            }
        }
        std::vector<size_t> keep;
        for (size_t i : r.dispatch)
        {
            if (!drop.count(i))
            {
                keep.push_back(i);
            }
        }
        r.dispatch = keep;
        r.rejected = rej;
    }
    if (StateAware())
    {
        std::set<size_t> drop;
        std::vector<std::pair<size_t, std::string>> rej;
        StateAwareResolve(cmds, sp, drop, rej);
        std::vector<size_t> keep;
        for (size_t i : r.dispatch)
        {
            if (!drop.count(i))
            {
                keep.push_back(i);
            }
        }
        r.dispatch = keep;
        r.rejected = rej;
    }
    // complete record of detected AND undetected TS x TPC interactions of this cycle
    std::vector<int> paired(cmds.size(), 0);
    for (const auto& pc : r.pairs)
    {
        paired[pc.ts] = paired[pc.tpc] = 1;
        std::string d = "ts cmd " + std::to_string(xapp::cmdlife::IdOf(cmds[pc.ts])) + " (imsi " +
                        std::to_string(props[pc.ts].imsi) + " " + std::to_string(props[pc.ts].src) + "->" +
                        std::to_string(props[pc.ts].dst) + ") x tpc cmd " +
                        std::to_string(xapp::cmdlife::IdOf(cmds[pc.tpc])) + " (cell " +
                        std::to_string(props[pc.tpc].cell) + " " + std::to_string(props[pc.tpc].currentDbm) + "->" +
                        std::to_string(props[pc.tpc].requestedDbm) + ")";
        const std::string o = pc.flagged ? "potential_conflict" : "not_flagged";
        xapp::cmdlife::Stage(cmds[pc.ts], "detector", o, pc.pattern, d);
        xapp::cmdlife::Stage(cmds[pc.tpc], "detector", o, pc.pattern, d);
    }
    for (size_t i = 0; i < cmds.size(); i++)
    {
        if (!paired[i])
        {
            xapp::cmdlife::Stage(cmds[i], "detector", "no_pair", "", "no proposal of the other xApp this cycle");
        }
    }
    for (const auto& [i, why] : r.rejected)
    {
        xapp::cmdlife::Stage(cmds[i], "cmm_resolution", "rejected", why, "not deferred or rescheduled");
        xapp::cmdlife::Final(cmds[i], "rejected", why);
    }
    std::vector<Ptr<OranCommand>> out;
    for (size_t i : r.dispatch)
    {
        out.push_back(cmds[i]);
    }
    if (m_coordinationDelay > Seconds(0))
    {
        Simulator::Schedule(m_coordinationDelay, &OranCmmCoord::Dispatch, this, out);
        return {};
    }
    size_t k = 0;
    for (const auto& c : out)
    {
        xapp::cmdlife::Stage(c, "dispatched", "", "", "list position " + std::to_string(k++));
    }
    Record(out);
    return out;
}

bool
OranCmmCoord::StateAware() const
{
    return m_mode == "b4sa-ts" || m_mode == "b4sa-tpc" || m_mode == "b5sa";
}

void
OranCmmCoord::Record(const std::vector<Ptr<OranCommand>>& dispatched)
{
    if (m_interactionLog.empty() && !StateAware())
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    for (const auto& c : dispatched)
    {
        auto it = m_propOf.find(xapp::cmdlife::IdOf(c));
        if (it == m_propOf.end())
        {
            continue;
        }
        const auto& p = it->second;
        xapp::coord::HistCmd h;
        h.type = p.type;
        h.id = p.id;
        h.tDisp = now;
        h.imsi = p.imsi;
        h.src = p.src;
        h.dst = p.dst;
        h.cell = p.cell;
        h.requestedDbm = p.requestedDbm;
        if (p.type == xapp::coord::Prop::TPC)
        {
            h.believedBeforeDbm = xapp::coord::BelievedPower(m_hist, p.cell, xapp::ric::LatestTxPowerDbm(p.cell),
                                                             xapp::ric::LatestLoadReportTime(p.cell), m_nominalDbm);
        }
        m_hist.push_back(h);
    }
}

xapp::coord::LabelInput
OranCmmCoord::MakeLabelInput(const std::vector<xapp::coord::Prop>& props) const
{
    // RIC-side only: proposals of this cycle (ids set), evidence starts registered by the issuing xApps, the CMM's
    // record of dispatched commands, the RIC's latest per-UE service observation and the cells' power reports
    double now = Simulator::Now().GetSeconds();
    xapp::coord::LabelInput in;
    in.now = now;
    in.nominalDbm = m_nominalDbm;
    in.props = props;
    for (const auto& p : props)
    {
        in.evidenceStart.push_back(xapp::ric::EvidenceStart(p.id));
    }
    in.history = m_hist;
    for (const auto& h : m_hist)
    {
        auto [cell, t] = xapp::ric::LatestServiceObservation(h.imsi);
        in.hoPending.push_back(xapp::coord::HoPending(h, cell, t, now, m_diagPendingS));
    }
    std::set<uint16_t> cells;
    for (const auto& p : props)
    {
        cells.insert(p.type == xapp::coord::Prop::TS ? p.dst : p.cell);
    }
    for (uint16_t c : cells)
    {
        in.believedDbm.emplace_back(c, xapp::coord::BelievedPower(m_hist, c, xapp::ric::LatestTxPowerDbm(c),
                                                                   xapp::ric::LatestLoadReportTime(c), m_nominalDbm));
    }
    return in;
}

void
OranCmmCoord::Diagnose(const std::vector<Ptr<OranCommand>>& cmds, const std::vector<xapp::coord::Prop>& props)
{
    if (!m_diag.is_open())
    {
        m_diag.open(m_interactionLog);
        m_diag << "time_s,cmd_id,prop_type,cell_y,imsi,src,requested_dbm,evidence_start_s,definition,flagged,"
                  "related_cmd,related_t_s,gap_s,ts_first_would_reject,tpc_first_would_reject,basis\n";
    }
    double now = Simulator::Now().GetSeconds();
    xapp::coord::LabelInput in = MakeLabelInput(props);
    for (const auto& l : xapp::coord::LabelInteractions(in))
    {
        const auto& p = in.props[l.prop];
        bool ts = p.type == xapp::coord::Prop::TS;
        std::string basis = l.basis;
        for (auto& ch : basis)
        {
            if (ch == ',')
                ch = ';';
        }
        m_diag << std::fixed << std::setprecision(6) << now << "," << p.id << "," << (ts ? "ts" : "tpc_reduce") << ","
               << (ts ? p.dst : p.cell) << "," << p.imsi << "," << p.src << "," << (ts ? 0.0 : p.requestedDbm) << ","
               << in.evidenceStart[l.prop] << "," << l.def << "," << l.flagged << ","
               << (l.relatedId >= 0 ? std::to_string(l.relatedId) : "") << ","
               << (l.relatedId >= 0 ? std::to_string(l.relatedT) : "") << ","
               << (l.relatedId >= 0 ? std::to_string(now - l.relatedT) : "") << "," << l.tsFirstWouldReject << ","
               << l.tpcFirstWouldReject << "," << basis << "\n";
    }
    m_diag.flush();
}

namespace
{
std::string
Csv(std::string v)
{
    for (auto& ch : v)
    {
        if (ch == ',')
            ch = ';';
    }
    return v;
}
} // namespace

void
OranCmmCoord::StateAwareResolve(const std::vector<Ptr<OranCommand>>& cmds, const std::vector<xapp::coord::Prop>& sp,
                                std::set<size_t>& drop, std::vector<std::pair<size_t, std::string>>& rej)
{
    using xapp::coord::Prop;
    NS_ABORT_MSG_IF(m_saDef != "combined" && m_saDef != "win_2" && m_saDef != "win_5",
                    "OranCmmCoord: SaDefinition must be combined, win_2 or win_5");
    const double now = Simulator::Now().GetSeconds();
    xapp::coord::LabelInput in = MakeLabelInput(sp);
    in.windows = {2.0, 5.0};
    const bool b5sa = m_mode == "b5sa";
    // B4sa: the priority rule. B5sa: the same labels (flags), resolved by B5sa units instead
    std::vector<xapp::coord::SaDecision> sd = xapp::coord::ResolveStateAwareB4(in, m_saDef, m_mode != "b4sa-tpc");
    std::map<size_t, std::string> action, reason, note;
    if (!b5sa)
    {
        for (const auto& d : sd)
        {
            if (d.reject)
            {
                drop.insert(d.prop);
                rej.emplace_back(d.prop, d.reason);
                action[d.prop] = "reject";
                reason[d.prop] = d.reason;
            }
            note[d.prop] = d.note;
        }
    }
    else
    {
        static xapp::est::AmcLut lut = xapp::est::BuildAmcLut();
        std::map<size_t, const xapp::coord::SaDecision*> lab;
        for (const auto& d : sd)
        {
            lab[d.prop] = &d;
        }
        struct U
        {
            xapp::b5sa::Unit unit;
            int64_t ts{-1}, tpc{-1}; //!< proposal indices (-1: not a proposal of this cycle)
            int64_t hist{-1};
        };
        std::vector<U> units;
        std::set<size_t> inPair;
        for (const auto& pc : xapp::coord::Detect(sp))
        {
            if (!pc.flagged || inPair.count(pc.ts) || inPair.count(pc.tpc))
                continue; // same-cycle P1 on believed powers: always flagged under every definition
            U u;
            u.unit.kind = "pair";
            u.unit.tsImsi = sp[pc.ts].imsi;
            u.unit.src = sp[pc.ts].src;
            u.unit.dst = sp[pc.ts].dst;
            u.unit.tpcCell = sp[pc.tpc].cell;
            u.unit.cands = {"N", "T", "P", "B"};
            u.ts = pc.ts;
            u.tpc = pc.tpc;
            inPair.insert(pc.ts);
            inPair.insert(pc.tpc);
            units.push_back(u);
        }
        for (const auto& d : sd)
        {
            if (!d.label.flagged || inPair.count(d.prop))
                continue;
            const Prop& p = sp[d.prop];
            U u;
            u.hist = d.label.histId;
            if (p.type == Prop::TS)
            {
                u.unit.kind = "ts";
                u.unit.tsImsi = p.imsi;
                u.unit.src = p.src;
                u.unit.dst = p.dst;
                u.unit.tpcCell = p.dst;
                u.unit.cands = {"N", "T"};
                u.ts = d.prop;
            }
            else
            {
                u.unit.kind = "tpc";
                auto it = m_propOf.find(static_cast<uint64_t>(std::max<int64_t>(d.label.histId, 0)));
                if (d.label.histId >= 0 && it != m_propOf.end())
                {
                    u.unit.tsImsi = it->second.imsi;
                    u.unit.src = it->second.src;
                    u.unit.dst = it->second.dst;
                }
                u.unit.tpcCell = p.cell;
                u.unit.cands = {"N", "P"};
                u.tpc = d.prop;
            }
            units.push_back(u);
        }
        for (const auto& u : units)
        {
            auto t0 = std::chrono::steady_clock::now();
            std::set<uint16_t> cells{u.unit.dst, u.unit.tpcCell};
            if (u.unit.src)
                cells.insert(u.unit.src);
            cells.erase(0);
            double req = u.tpc >= 0 ? sp[u.tpc].requestedDbm : std::nan("");
            std::string block, desc;
            double newest = -1e18;
            int nUes = 0, nUnver = 0;
            auto scen = BuildScenarios(now, cells, req, u.unit.tpcCell, block, desc, newest, nUes, nUnver);
            xapp::b5sa::Decision d = xapp::b5sa::Decide(scen, block, u.unit, xapp::b5::Params{}, lut);
            double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t k = ++m_b5saN;
            std::ostringstream det;
            for (const auto& c : d.cands)
                det << c.name << ":f" << c.feasible << "s" << c.safe << "v" << c.vhat << (c.why.empty() ? "" : ":" + c.why) << " ";
            const bool rejTs = u.ts >= 0 && (d.choice == "N" || d.choice == "P");
            const bool rejTpc = u.tpc >= 0 && (d.choice == "N" || d.choice == "T");
            for (int64_t i : {u.ts, u.tpc})
            {
                if (i < 0)
                    continue;
                xapp::cmdlife::Stage(cmds[i], "b5_decision", d.choice, d.reason,
                                     "b5sa " + u.unit.kind + " #" + std::to_string(k) + " scenarios " + desc + " " +
                                         det.str() + (m_b5Shadow ? " SHADOW(not enforced)" : ""));
                bool rj = (i == u.ts && rejTs) || (i == u.tpc && rejTpc);
                action[i] = rj ? (m_b5Shadow ? "shadow_reject" : "reject") : "pass";
                reason[i] = d.reason;
                if (rj && !m_b5Shadow)
                {
                    drop.insert(i);
                    rej.emplace_back(i, d.reason);
                }
            }
            if (!m_b5saLog.empty())
            {
                if (!m_b5sa.is_open())
                {
                    m_b5sa.open(m_b5saLog);
                    m_b5sa << "decision,time_s,unit,ts_cmd,tpc_cmd,hist_cmd,ts_imsi,src,dst,tpc_cell,requested_dbm,"
                              "n_scenarios,scenarios,block,n_ues,n_unverifiable,newest_input_t_s,choice,reason,shadow";
                    for (const char* n : {"N", "T", "P", "B"})
                        m_b5sa << "," << n << "_evaluated," << n << "_feasible," << n << "_safe," << n << "_vhat," << n
                               << "_c_src," << n << "_J," << n << "_why";
                    m_b5sa << ",compute_us\n";
                }
                m_b5sa << k << "," << std::fixed << std::setprecision(6) << now << "," << u.unit.kind << ","
                       << (u.ts >= 0 ? std::to_string(sp[u.ts].id) : "") << ","
                       << (u.tpc >= 0 ? std::to_string(sp[u.tpc].id) : "") << ","
                       << (u.hist >= 0 ? std::to_string(u.hist) : "") << "," << u.unit.tsImsi << "," << u.unit.src
                       << "," << u.unit.dst << "," << u.unit.tpcCell << "," << req << "," << scen.size() << ","
                       << Csv(desc) << "," << block << "," << nUes << "," << nUnver << "," << newest << "," << d.choice
                       << "," << d.reason << "," << m_b5Shadow;
                for (const char* n : {"N", "T", "P", "B"})
                {
                    const xapp::b5::CandEval* c = nullptr;
                    for (const auto& x : d.cands)
                        if (x.name == n)
                            c = &x;
                    if (c)
                        m_b5sa << ",1," << c->feasible << "," << c->safe << "," << c->vhat << "," << c->cSrc << ","
                               << c->J << "," << Csv(c->why);
                    else
                        m_b5sa << ",0,,,,,,";
                }
                m_b5sa << "," << us << "\n";
                m_b5sa.flush();
            }
            if (!m_b5saPredLog.empty())
            {
                if (!m_b5saPred.is_open())
                {
                    m_b5saPred.open(m_b5saPredLog);
                    m_b5saPred << "decision,time_s,scenario,candidate,imsi,cell_now,cell_after,demand_mbps,i3b,pred_mbps\n";
                }
                for (const auto& pr : d.preds)
                {
                    const xapp::b5sa::Scenario* sc = nullptr;
                    for (const auto& x : scen)
                        if (x.name == pr.scenario)
                            sc = &x;
                    for (const auto& o : pr.ues)
                    {
                        const xapp::b5::UeInfo* ui = nullptr;
                        for (const auto& x : sc->ues)
                            if (x.ue.imsi == o.imsi)
                                ui = &x;
                        m_b5saPred << k << "," << std::fixed << std::setprecision(6) << now << "," << pr.scenario << ","
                                   << pr.cand << "," << o.imsi << "," << ui->ue.cell << "," << o.cellAfter << ","
                                   << ui->ue.demandMbps << "," << ui->i3b << "," << o.rateMbps << "\n";
                    }
                }
                m_b5saPred.flush();
            }
        }
    }
    if (!m_saLog.empty())
    {
        if (!m_sa.is_open())
        {
            m_sa.open(m_saLog);
            m_sa << "time_s,mode,definition,cmd_id,prop_type,cell_y,imsi,src,believed_dbm,requested_dbm,"
                    "evidence_start_s,flagged,same_cycle_cp,hist_cmd,hist_t_s,gap_s,action,reason,note,basis\n";
        }
        for (const auto& d : sd)
        {
            const Prop& p = sp[d.prop];
            const bool ts = p.type == Prop::TS;
            std::string act = action.count(d.prop) ? action[d.prop] : "pass";
            m_sa << std::fixed << std::setprecision(6) << now << "," << m_mode << "," << m_saDef << "," << p.id << ","
                 << (ts ? "ts" : "tpc_reduce") << "," << (ts ? p.dst : p.cell) << "," << p.imsi << "," << p.src << ","
                 << (ts ? 0.0 : p.currentDbm) << "," << (ts ? 0.0 : p.requestedDbm) << ","
                 << in.evidenceStart[d.prop] << "," << d.label.flagged << "," << d.label.sameCycleCp << ","
                 << (d.label.histId >= 0 ? std::to_string(d.label.histId) : "") << ","
                 << (d.label.histId >= 0 ? std::to_string(d.label.histT) : "") << ","
                 << (d.label.histId >= 0 ? std::to_string(now - d.label.histT) : "") << "," << act << ","
                 << (reason.count(d.prop) ? reason[d.prop] : "") << "," << note[d.prop] << "," << Csv(d.label.basis)
                 << "\n";
        }
        m_sa.flush();
    }
    for (const auto& d : sd)
    {
        std::string det = "def " + m_saDef + (d.label.sameCycleCp ? " same-cycle counterpart" : "") +
                          (d.label.histId >= 0 ? " dispatched counterpart cmd " + std::to_string(d.label.histId) +
                                                     " at " + std::to_string(d.label.histT)
                                               : "") +
                          (note[d.prop].empty() ? "" : " note " + note[d.prop]);
        xapp::cmdlife::Stage(cmds[d.prop], "sa_detector", d.label.flagged ? "flagged" : "not_flagged",
                             d.label.basis.empty() ? "" : Csv(d.label.basis), det);
    }
}

std::vector<xapp::b5sa::Scenario>
OranCmmCoord::BuildScenarios(double now, const std::set<uint16_t>& cells, double tpcRequested, uint16_t tpcCell,
                             std::string& block, std::string& desc, double& newest, int& nUes, int& nUnver) const
{
    using xapp::coord::HistCmd;
    using xapp::coord::Prop;
    xapp::policy::Snapshot s = xapp::ric::BuildSnapshot(now);
    // verifiable UEs: the B5 rule (fresh serving-cell service report and serving RSRP; stale duplicate views of an
    // IMSI that also has a fresh view are leftover RIC entries)
    std::map<uint64_t, bool> fresh;
    for (const auto& u : s.ues)
    {
        if (u.imsi != 0 && u.servingAge <= 1.0 + 1e-9)
            fresh[u.imsi] = true;
    }
    struct V
    {
        const xapp::policy::UeView* u;
    };
    std::vector<V> ver;
    bool unverInvolved = false;
    for (const auto& u : s.ues)
    {
        if (u.imsi != 0 && u.servingAge > 1.0 + 1e-9 && fresh.count(u.imsi))
            continue;
        bool ok = u.imsi != 0 && u.svc && now - u.svc->t <= 1.0 + 1e-9 && u.svc->cell == u.servingCell &&
                  u.rsrpDbm.count(u.servingCell) && u.servingAge <= 1.0 + 1e-9;
        if (!ok)
        {
            nUnver++;
            if (cells.count(u.servingCell))
                unverInvolved = true;
            continue;
        }
        newest = std::max(newest, u.svc->t);
        for (const auto& [c, a] : u.rsrpAge)
            newest = std::max(newest, now - a);
        ver.push_back({&u});
    }
    nUes = static_cast<int>(ver.size());
    std::map<uint16_t, double> rho;
    for (const auto& [c, q] : s.load)
    {
        if (!q.empty())
        {
            rho[c] = q[0].prb;
            newest = std::max(newest, q[0].t);
        }
    }
    // believed power per cell and the last dispatched TPC command of the cell
    std::set<uint16_t> allCells;
    for (const auto& v : ver)
        for (const auto& [c, r] : v.u->rsrpDbm)
            allCells.insert(c);
    for (const auto& [c, q] : rho)
        allCells.insert(c);
    std::map<uint16_t, double> pBel;
    std::map<uint16_t, const HistCmd*> last;
    for (uint16_t c : allCells)
    {
        pBel[c] = xapp::coord::BelievedPower(m_hist, c, xapp::ric::LatestTxPowerDbm(c),
                                             xapp::ric::LatestLoadReportTime(c), m_nominalDbm);
        for (const auto& h : m_hist)
        {
            newest = std::max(newest, h.tDisp);
            if (h.type == Prop::TPC && h.cell == c && (!last.count(c) || h.tDisp >= last[c]->tDisp))
                last[c] = &h;
        }
    }
    // power of cell c at measurement time tm, as believed by the RIC (dispatch = effect); tm after the last dispatch:
    // the scenario's current power
    auto pAt = [&](uint16_t c, double tm, double pNowC) {
        const HistCmd* before = nullptr;
        const HistCmd* after = nullptr;
        for (const auto& h : m_hist)
        {
            if (h.type != Prop::TPC || h.cell != c)
                continue;
            if (h.tDisp <= tm + 1e-9)
            {
                if (!before || h.tDisp >= before->tDisp)
                    before = &h;
            }
            else if (!after || h.tDisp < after->tDisp)
            {
                after = &h;
            }
        }
        if (!after)
            return pNowC;
        return before ? before->requestedDbm : after->believedBeforeDbm;
    };
    // uncertain items: (i) unconfirmed power command of an involved cell; (ii) pending handover touching one
    struct Item
    {
        bool ho;
        const HistCmd* h;
        std::string name;
    };
    std::vector<Item> items;
    for (uint16_t c : cells)
    {
        if (last.count(c) && !(xapp::ric::LatestLoadReportTime(c) > last[c]->tDisp + 1e-9))
            items.push_back({false, last[c], "pwr" + std::to_string(c) + "_cmd" + std::to_string(last[c]->id) + "_not_applied"});
    }
    std::map<uint64_t, const HistCmd*> lastHo;
    for (const auto& h : m_hist)
    {
        if (h.type == Prop::TS && (!lastHo.count(h.imsi) || h.tDisp >= lastHo[h.imsi]->tDisp))
            lastHo[h.imsi] = &h;
    }
    for (const auto& [imsi, h] : lastHo)
    {
        if (!cells.count(h->src) && !cells.count(h->dst))
            continue;
        auto [oc, ot] = xapp::ric::LatestServiceObservation(imsi);
        if (!xapp::coord::HoPending(*h, oc, ot, now, m_diagPendingS))
            continue;
        bool isVer = false;
        for (const auto& v : ver)
            isVer = isVer || v.u->imsi == imsi;
        if (isVer)
            items.push_back({true, h, "ho" + std::to_string(imsi) + "_cmd" + std::to_string(h->id) + "_not_done"});
    }
    if (unverInvolved)
        block = "unverifiable";
    else if (items.size() > m_maxUncertain)
        block = "uncertain_overflow";
    const size_t n = items.size() > m_maxUncertain ? 0 : items.size();
    std::vector<xapp::b5sa::Scenario> out;
    for (size_t mask = 0; mask < (size_t(1) << n); mask++)
    {
        xapp::b5sa::Scenario sc;
        std::map<uint16_t, double> pNow = pBel;
        std::map<uint64_t, uint16_t> moved; // believed-completed handovers not yet observed
        for (size_t i = 0; i < n; i++)
        {
            bool notDone = mask & (size_t(1) << i);
            if (!notDone)
            {
                if (items[i].ho)
                    moved[items[i].h->imsi] = items[i].h->dst;
                continue;
            }
            sc.name += (sc.name.empty() ? "" : "+") + items[i].name;
            if (!items[i].ho)
                pNow[items[i].h->cell] = items[i].h->believedBeforeDbm;
        }
        if (sc.name.empty())
            sc.name = "believed";
        for (const auto& v : ver)
        {
            xapp::b5::UeInfo x;
            x.ue.imsi = v.u->imsi;
            x.ue.cell = moved.count(v.u->imsi) ? moved[v.u->imsi] : v.u->servingCell;
            x.ue.demandMbps = v.u->svc->demandMeanMbps > 0 ? v.u->svc->demandMeanMbps : v.u->svc->demandMbps;
            for (const auto& [c, r] : v.u->rsrpDbm)
            {
                double tm = now - v.u->rsrpAge.at(c);
                double pn = pNow.count(c) ? pNow[c] : m_nominalDbm;
                x.ue.rsrpDbm[c] = r + pn - pAt(c, tm, pn);
            }
            x.i3b = xapp::policy::I3Flag(*v.u->svc, 0.95);
            sc.ues.push_back(x);
        }
        sc.rho = rho;
        sc.tpcDeltaDb = std::isnan(tpcRequested) ? 0.0 : tpcRequested - (pNow.count(tpcCell) ? pNow[tpcCell] : m_nominalDbm);
        out.push_back(sc);
    }
    for (const auto& sc : out)
        desc += (desc.empty() ? "" : "|") + sc.name;
    if (n < items.size())
        desc += "|overflow:" + std::to_string(items.size());
    return out;
}

void
OranCmmCoord::Dispatch(std::vector<Ptr<OranCommand>> commands)
{
    size_t k = 0;
    for (const auto& c : commands)
    {
        xapp::cmdlife::Stage(c, "dispatched", "", "", "list position " + std::to_string(k++) + " after coordination hold");
    }
    Record(commands);
    m_nearRtRic->GetE2Terminator()->ProcessCommands(commands);
}

std::string
OranCmmCoord::B5Resolve(const xapp::coord::Prop& ts, const xapp::coord::Prop& tpc)
{
    static xapp::est::AmcLut lut = xapp::est::BuildAmcLut(); // built once, outside the timed region
    const double now = Simulator::Now().GetSeconds();
    auto t0 = std::chrono::steady_clock::now();
    xapp::policy::Snapshot s = xapp::ric::BuildSnapshot(now);
    // verifiable UEs, as in the estimator probe: fresh service report from the serving cell and fresh serving RSRP;
    // a stale view of an IMSI that also has a fresh view is a leftover RIC entry, not a UE
    std::map<uint64_t, bool> fresh;
    for (const auto& u : s.ues)
    {
        if (u.imsi != 0 && u.servingAge <= 1.0 + 1e-9)
            fresh[u.imsi] = true;
    }
    std::vector<xapp::b5::UeInfo> ues;
    int unver = 0;
    bool unverInvolved = false;
    double newest = -1e18;
    for (const auto& u : s.ues)
    {
        if (u.imsi != 0 && u.servingAge > 1.0 + 1e-9 && fresh.count(u.imsi))
            continue;
        bool ok = u.imsi != 0 && u.svc && now - u.svc->t <= 1.0 + 1e-9 && u.svc->cell == u.servingCell &&
                  u.rsrpDbm.count(u.servingCell) && u.servingAge <= 1.0 + 1e-9;
        if (!ok)
        {
            unver++;
            if (u.servingCell == ts.src || u.servingCell == ts.dst || u.servingCell == tpc.cell)
                unverInvolved = true;
            continue;
        }
        newest = std::max(newest, u.svc->t);
        xapp::b5::UeInfo x;
        x.ue.imsi = u.imsi;
        x.ue.cell = u.servingCell;
        x.ue.demandMbps = u.svc->demandMeanMbps > 0 ? u.svc->demandMeanMbps : u.svc->demandMbps;
        x.ue.rsrpDbm = u.rsrpDbm;
        x.i3b = xapp::policy::I3Flag(*u.svc, 0.95);
        ues.push_back(x);
    }
    std::map<uint16_t, double> rho;
    for (const auto& [c, q] : s.load)
    {
        if (!q.empty())
        {
            rho[c] = q[0].prb;
            newest = std::max(newest, q[0].t);
        }
    }
    xapp::b5::Pair pr;
    pr.tsImsi = ts.imsi;
    pr.src = ts.src;
    pr.dst = ts.dst;
    pr.tpcCell = tpc.cell;
    pr.deltaDb = tpc.requestedDbm - tpc.currentDbm;
    xapp::b5::Decision d = xapp::b5::Decide(ues, unverInvolved, rho, pr, xapp::b5::Params{}, lut);
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    m_lastB5Reason = d.reason;
    std::ostringstream det;
    for (const auto& c : d.cands)
        det << c.name << ":f" << c.feasible << "s" << c.safe << "v" << c.vhat << " ";
    m_lastB5Detail = det.str();
    if (!m_b5Log.empty())
    {
        if (!m_b5.is_open())
        {
            m_b5.open(m_b5Log);
            m_b5 << "time_s,ts_cmd,tpc_cmd,ts_imsi,src,dst,tpc_cell,delta_db,n_ues,n_unverifiable,unverifiable_involved,"
                    "newest_input_t_s,choice,reason";
            for (const char* n : {"N", "T", "P", "B"})
                m_b5 << "," << n << "_feasible," << n << "_safe," << n << "_vhat," << n << "_c_src," << n << "_J," << n
                     << "_why";
            m_b5 << ",compute_us\n";
        }
        m_b5 << std::fixed << std::setprecision(6) << now << "," << ts.id << "," << tpc.id << "," << ts.imsi << ","
             << ts.src << "," << ts.dst << "," << tpc.cell << "," << pr.deltaDb << "," << ues.size() << "," << unver
             << "," << unverInvolved << "," << newest << "," << d.choice << "," << d.reason;
        for (const auto& c : d.cands)
            m_b5 << "," << c.feasible << "," << c.safe << "," << c.vhat << "," << c.cSrc << "," << c.J << "," << c.why;
        m_b5 << "," << us << "\n";
        m_b5.flush();
    }
    return d.choice;
}

} // namespace ns3
