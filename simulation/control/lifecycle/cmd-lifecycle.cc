/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S3: per-command lifecycle log (see header).
 */
#include "cmd-lifecycle.h"

#include "oran-command-lte-tx-power.h"

#include "ns3/config.h"
#include "ns3/lte-enb-rrc.h"
#include "ns3/lte-ue-net-device.h"
#include "ns3/lte-ue-rrc.h"
#include "ns3/oran-command-lte-2-lte-handover.h"
#include "ns3/simulator.h"

#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

using namespace ns3;

namespace xapp
{
namespace cmdlife
{
namespace
{

struct Rec
{
    Ptr<OranCommand> cmd;
    uint64_t id{0};
    std::string lm;
    int64_t cycleNs{-1};
    std::string type;
    uint64_t e2Node{0};
    int cell{-1};       // power: target cell; handover: source cell (once known)
    int targetCell{-1}; // handover only
    int rnti{-1};
    int64_t imsi{-1};
    int64_t intendedImsi{-1}; // stable identity of the UE the proposal was generated for (V1(f))
    double value{std::numeric_limits<double>::quiet_NaN()};
    double appliedDbm{std::numeric_limits<double>::quiet_NaN()};
    std::set<std::string> stages;
    bool final{false};
};

std::ofstream g_f;
bool g_open = false;
uint64_t g_nextId = 1;
std::vector<Rec> g_recs;                    // index = id - 1
std::map<const OranCommand*, uint64_t> g_ids; // object identity -> id
std::map<uint16_t, std::deque<uint64_t>> g_powerPending; // cell -> applied, not yet radiated
std::map<uint64_t, uint64_t> g_hoPending;    // imsi -> id
NetDeviceContainer g_ues;
NetDeviceContainer g_enbs;
std::set<std::pair<uint16_t, uint16_t>> g_x2;

const char* kEnbStates[] = {"INITIAL_RANDOM_ACCESS", "CONNECTION_SETUP", "CONNECTION_REJECTED",
                            "ATTACH_REQUEST", "CONNECTED_NORMALLY", "CONNECTION_RECONFIGURATION",
                            "CONNECTION_REESTABLISHMENT", "HANDOVER_PREPARATION",
                            "HANDOVER_JOINING", "HANDOVER_PATH_SWITCH", "HANDOVER_LEAVING"};
const char* kUeStates[] = {"IDLE_START", "IDLE_CELL_SEARCH", "IDLE_WAIT_MIB_SIB1", "IDLE_WAIT_MIB",
                           "IDLE_WAIT_SIB1", "IDLE_CAMPED_NORMALLY", "IDLE_WAIT_SIB2",
                           "IDLE_RANDOM_ACCESS", "IDLE_CONNECTING", "CONNECTED_NORMALLY",
                           "CONNECTED_HANDOVER", "CONNECTED_PHY_PROBLEM",
                           "CONNECTED_REESTABLISHING"};

std::string
Num(double v, int p = 4)
{
    if (std::isnan(v))
    {
        return "";
    }
    std::ostringstream s;
    s << std::fixed << std::setprecision(p) << v;
    return s.str();
}

std::string
Int(int64_t v)
{
    return v < 0 ? std::string() : std::to_string(v);
}

void
Row(const Rec& r, const std::string& stage, const std::string& outcome, const std::string& reason,
    const std::string& detail, double oldDbm = NAN, double newDbm = NAN)
{
    if (!g_open)
    {
        return;
    }
    int64_t ns = Simulator::Now().GetNanoSeconds();
    auto clean = [](std::string x) {
        for (auto& ch : x)
        {
            if (ch == ',' || ch == '\n')
            {
                ch = ';'; // keep the CSV well-formed
            }
        }
        return x;
    };
    g_f << ns << "," << Num(ns * 1e-9, 9) << "," << r.id << "," << stage << "," << r.lm << ","
        << Int(r.cycleNs) << "," << r.type << "," << r.e2Node << "," << Int(r.cell) << ","
        << Int(r.targetCell) << "," << Int(r.rnti) << "," << Int(r.imsi) << "," << Num(r.value)
        << "," << Num(oldDbm) << "," << Num(newDbm) << "," << outcome << "," << clean(reason) << ","
        << clean(detail) << "\n";
}

Rec&
Get(Ptr<OranCommand> c)
{
    auto it = g_ids.find(PeekPointer(c));
    if (it != g_ids.end())
    {
        return g_recs[it->second - 1];
    }
    // A command from an LM that is not instrumented (e.g. the NIST distance LM): first seen here.
    Rec r;
    r.cmd = c;
    r.id = g_nextId++;
    r.lm = "uninstrumented";
    r.e2Node = c->GetTargetE2NodeId();
    if (auto ho = DynamicCast<OranCommandLte2LteHandover>(c))
    {
        r.type = "handover";
        r.targetCell = ho->GetTargetCellId();
        r.rnti = ho->GetTargetRnti();
    }
    else if (auto tp = DynamicCast<OranCommandLteTxPower>(c))
    {
        r.type = "tx_power";
        r.value = tp->GetTxPowerDbm();
    }
    else
    {
        r.type = c->GetInstanceTypeId().GetName();
    }
    g_recs.push_back(r);
    g_ids[PeekPointer(c)] = r.id;
    Row(g_recs.back(), "first_seen", "", "", "creation not logged by its LM");
    return g_recs.back();
}

void
FinalRec(Rec& r, const std::string& outcome, const std::string& reason, const std::string& detail)
{
    if (r.final)
    {
        return;
    }
    r.final = true;
    r.stages.insert("final");
    Row(r, "final", outcome, reason, detail);
}

Rec*
ById(uint64_t id)
{
    return (id >= 1 && id <= g_recs.size()) ? &g_recs[id - 1] : nullptr;
}

// ---- handover traces ----
Rec*
PendingHo(uint64_t imsi)
{
    auto it = g_hoPending.find(imsi);
    return it == g_hoPending.end() ? nullptr : ById(it->second);
}

void
HoDone(uint64_t imsi)
{
    g_hoPending.erase(imsi);
}

void
EnbHoStart(uint64_t imsi, uint16_t cellId, uint16_t rnti, uint16_t target)
{
    if (Rec* r = PendingHo(imsi))
    {
        r->stages.insert("ho_start_enb");
        Row(*r, "ho_start_enb", "", "", "source " + std::to_string(cellId));
    }
}

void
UeHoStart(uint64_t imsi, uint16_t cellId, uint16_t rnti, uint16_t target)
{
    if (Rec* r = PendingHo(imsi))
    {
        r->stages.insert("ho_start_ue");
        Row(*r, "ho_start_ue", "", "", "UE received the handover command");
    }
}

void
UeHoEndOk(uint64_t imsi, uint16_t cellId, uint16_t rnti)
{
    if (Rec* r = PendingHo(imsi))
    {
        r->stages.insert("ho_completed_ue");
        Row(*r, "ho_completed_ue", cellId == r->targetCell ? "ok" : "other_cell", "",
            "UE connected to cell " + std::to_string(cellId));
        FinalRec(*r, cellId == r->targetCell ? "completed" : "ho_failed",
                 cellId == r->targetCell ? "" : "completed_at_other_cell", "");
        // keep tracking for the eNB-side completion (path switch)
    }
}

void
EnbHoEndOk(uint64_t imsi, uint16_t cellId, uint16_t rnti)
{
    if (Rec* r = PendingHo(imsi))
    {
        r->stages.insert("ho_completed_enb");
        Row(*r, "ho_completed_enb", "", "", "target eNB path switch done");
        HoDone(imsi);
    }
}

void
HoFail(uint64_t imsi, const std::string& reason)
{
    if (Rec* r = PendingHo(imsi))
    {
        if (!r->final)
        {
            Row(*r, "ho_failure", "", reason, "");
            FinalRec(*r, "ho_failed", reason, "");
        }
        HoDone(imsi);
    }
}

void
UeHoEndError(uint64_t imsi, uint16_t, uint16_t)
{
    HoFail(imsi, "ue_handover_end_error");
}

void
UeRlf(uint64_t imsi, uint16_t, uint16_t)
{
    HoFail(imsi, "rlf_before_completion");
}

void
UeConnEstablished(uint64_t imsi, uint16_t cellId, uint16_t)
{
    HoFail(imsi, "reestablished_cell_" + std::to_string(cellId));
}

void
EnbFailNoPreamble(uint64_t imsi, uint16_t, uint16_t)
{
    HoFail(imsi, "enb_no_preamble");
}

void
EnbFailMaxRach(uint64_t imsi, uint16_t, uint16_t)
{
    HoFail(imsi, "enb_max_rach");
}

void
EnbFailLeaving(uint64_t imsi, uint16_t, uint16_t)
{
    HoFail(imsi, "enb_leaving_timeout");
}

void
EnbFailJoining(uint64_t imsi, uint16_t, uint16_t)
{
    HoFail(imsi, "enb_joining_timeout");
}

} // namespace

void
Open(const std::string& outPrefix)
{
    g_f.open(outPrefix + "-cmdlife.csv");
    g_f << "time_ns,time_s,cmd_id,stage,lm,cycle_ns,cmd_type,target_e2_node,cell,target_cell,rnti,"
           "imsi,value_dbm,old_dbm,new_dbm,outcome,reason,detail\n";
    g_open = true;
}

bool
IsOpen()
{
    return g_open;
}

void
Close()
{
    if (!g_open)
    {
        return;
    }
    for (auto& r : g_recs)
    {
        if (r.final)
        {
            continue;
        }
        if (!r.stages.count("dispatched"))
        {
            FinalRec(r, "not_dispatched_by_end", "", "");
        }
        else if (!r.stages.count("e2_received"))
        {
            FinalRec(r, "not_received_by_end", "", "");
        }
        else if (r.type == "handover")
        {
            FinalRec(r, "not_completed_by_end", "", "");
        }
        else
        {
            FinalRec(r, "not_effective_by_end", "", "");
        }
    }
    g_f.close();
    g_open = false;
}

void
RegisterDevices(const NetDeviceContainer& ues, const NetDeviceContainer& enbs)
{
    g_ues = ues;
    g_enbs = enbs;
}

void
RegisterX2(uint16_t a, uint16_t b)
{
    g_x2.insert({a, b});
    g_x2.insert({b, a});
}

Ptr<NetDevice>
UeDevice(uint32_t i)
{
    return i < g_ues.GetN() ? g_ues.Get(i) : nullptr;
}

uint32_t
NumUes()
{
    return g_ues.GetN();
}

Ptr<LteEnbNetDevice>
EnbByCell(uint16_t cellId)
{
    for (uint32_t k = 0; k < g_enbs.GetN(); k++)
    {
        auto d = g_enbs.Get(k)->GetObject<LteEnbNetDevice>();
        if (d && d->GetCellId() == cellId)
        {
            return d;
        }
    }
    return nullptr;
}

uint64_t
Created(Ptr<OranCommand> c, const std::string& lm, int64_t imsi, const std::string& detail)
{
    Rec r;
    r.cmd = c;
    r.id = g_nextId++;
    r.lm = lm;
    r.cycleNs = Simulator::Now().GetNanoSeconds();
    r.e2Node = c->GetTargetE2NodeId();
    r.imsi = imsi;
    r.intendedImsi = imsi;
    if (auto ho = DynamicCast<OranCommandLte2LteHandover>(c))
    {
        r.type = "handover";
        r.targetCell = ho->GetTargetCellId();
        r.rnti = ho->GetTargetRnti();
    }
    else if (auto tp = DynamicCast<OranCommandLteTxPower>(c))
    {
        r.type = "tx_power";
        r.value = tp->GetTxPowerDbm();
    }
    else
    {
        r.type = c->GetInstanceTypeId().GetName();
    }
    r.stages.insert("created");
    g_recs.push_back(r);
    g_ids[PeekPointer(c)] = r.id;
    Row(g_recs.back(), "created", "", "", detail);
    return r.id;
}

void
Stage(Ptr<OranCommand> c, const std::string& stage, const std::string& outcome,
      const std::string& reason, const std::string& detail)
{
    Rec& r = Get(c);
    r.stages.insert(stage);
    Row(r, stage, outcome, reason, detail);
}

void
Final(Ptr<OranCommand> c, const std::string& outcome, const std::string& reason,
      const std::string& detail)
{
    FinalRec(Get(c), outcome, reason, detail);
}

bool
HasStage(Ptr<OranCommand> c, const std::string& stage)
{
    return Get(c).stages.count(stage) > 0;
}

bool
IsFinal(Ptr<OranCommand> c)
{
    return Get(c).final;
}

uint64_t
IdOf(Ptr<OranCommand> c)
{
    return Get(c).id;
}

int64_t
IntendedImsi(Ptr<OranCommand> c)
{
    return Get(c).intendedImsi;
}

void
PowerApplied(Ptr<OranCommand> c, uint16_t cell, double requestedDbm, double oldDbm, double newDbm,
             const std::string& status)
{
    Rec& r = Get(c);
    r.cell = cell;
    r.appliedDbm = newDbm;
    bool noop = std::fabs(newDbm - oldDbm) < 1e-9;
    std::string outcome = noop ? "noop_same_value" : status;
    r.stages.insert("applied");
    Row(r, "applied", outcome, "", "PHY transmit-power attribute set; radiated from the next frame",
        oldDbm, newDbm);
    if (noop)
    {
        // Nothing will change on the air. An earlier pending command with the same value keeps
        // its own pending state.
        FinalRec(r, "noop_same_value", "", "");
        return;
    }
    auto& q = g_powerPending[cell];
    for (uint64_t id : q)
    {
        if (Rec* p = ById(id); p && !p->final)
        {
            p->stages.insert("superseded");
            Row(*p, "superseded", "", "", "cmd " + std::to_string(r.id) + " applied first");
            FinalRec(*p, "superseded", "", "replaced by cmd " + std::to_string(r.id));
        }
    }
    q.clear();
    q.push_back(r.id);
}

void
PhyPowerObserved(uint16_t cell, double oldDbm, double newDbm)
{
    auto it = g_powerPending.find(cell);
    if (it == g_powerPending.end())
    {
        return;
    }
    auto& q = it->second;
    // The oldest pending command whose applied value is now on the air (a change that matches
    // no pending command, e.g. a scripted power step, is not attributed to any command).
    for (auto k = q.begin(); k != q.end(); ++k)
    {
        Rec* r = ById(*k);
        if (r && !r->final && std::fabs(newDbm - r->appliedDbm) < 0.01)
        {
            r->stages.insert("phy_effect");
            Row(*r, "phy_effect", "ok", "", "first radiated frame at the new power", oldDbm,
                newDbm);
            FinalRec(*r, "effect_observed", "", "");
            q.erase(k);
            return;
        }
    }
}

void
HoRequested(Ptr<OranCommand> c, int64_t imsi, uint16_t sourceCell, uint16_t targetCell,
            uint16_t rnti)
{
    Rec& r = Get(c);
    if (r.imsi >= 0 && imsi >= 0 && r.imsi != imsi)
    {
        // The RNTI now belongs to a different UE than the one the proposal was generated for
        // (e.g. a stale RNTI after an earlier handover). Logged, not blocked: V1 has no identity
        // rule (decision-sheet item 12).
        Row(r, "identity_mismatch", "", "rnti_now_other_ue",
            "intended imsi " + std::to_string(r.imsi) + "; executed on imsi " + std::to_string(imsi));
    }
    r.imsi = imsi;
    r.cell = sourceCell;
    r.stages.insert("applied");
    Row(r, "applied", "ho_requested", "", "LteEnbRrc::SendHandoverRequest called");
    if (imsi >= 0)
    {
        g_hoPending[imsi] = r.id;
    }
}

void
AttachHandoverTraces()
{
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverStart",
                                  MakeCallback(&EnbHoStart));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                                  MakeCallback(&EnbHoEndOk));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/HandoverStart",
                                  MakeCallback(&UeHoStart));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/HandoverEndOk",
                                  MakeCallback(&UeHoEndOk));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/HandoverEndError",
                                  MakeCallback(&UeHoEndError));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/RadioLinkFailure",
                                  MakeCallback(&UeRlf));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/ConnectionEstablished",
                                  MakeCallback(&UeConnEstablished));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureNoPreamble",
                                  MakeCallback(&EnbFailNoPreamble));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureMaxRach",
                                  MakeCallback(&EnbFailMaxRach));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureLeaving",
                                  MakeCallback(&EnbFailLeaving));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverFailureJoining",
                                  MakeCallback(&EnbFailJoining));
}

std::string
CheckV1(Ptr<LteEnbNetDevice> source, uint16_t rnti, uint16_t targetCell, int64_t intendedImsi,
        int64_t* imsi)
{
    uint16_t src = source->GetCellId();
    Ptr<LteEnbRrc> enbRrc = source->GetRrc();
    // (a) target differs from the serving (= source) cell
    if (targetCell == src)
    {
        return "ho_target_is_serving";
    }
    // (b) the source eNB holds a context for this RNTI
    if (!enbRrc->HasUeManager(rnti))
    {
        return "ue_context_absent";
    }
    Ptr<UeManager> um = enbRrc->GetUeManager(rnti);
    if (imsi)
    {
        *imsi = static_cast<int64_t>(um->GetImsi());
    }
    // (f1) identity: evaluated at execution (command arrival), before any state check, so a stale
    // RNTI can never act on another UE: the context behind (source cell, RNTI) must belong to the
    // intended UE. Commands whose LM recorded no intended UE (NIST LMs, legacy regression only) are
    // not checked; the terminator logs them as identity_unverified.
    if (intendedImsi >= 0 && static_cast<int64_t>(um->GetImsi()) != intendedImsi)
    {
        return "ue_identity_changed";
    }
    // (c) eNB-side state
    if (um->GetState() != UeManager::CONNECTED_NORMALLY)
    {
        int s = static_cast<int>(um->GetState());
        return std::string("enb_state_") + (s >= 0 && s < 11 ? kEnbStates[s] : "UNKNOWN");
    }
    // (f2) identity, UE side: the intended UE must still be served by the source cell with this RNTI
    // (checked after (c) so a handover pending at the source eNB keeps its enb_state_* reason).
    if (intendedImsi >= 0)
    {
        for (uint32_t i = 0; i < g_ues.GetN(); i++)
        {
            auto d = g_ues.Get(i)->GetObject<LteUeNetDevice>();
            if (d && static_cast<int64_t>(d->GetImsi()) == intendedImsi &&
                (d->GetRrc()->GetCellId() != src || d->GetRrc()->GetRnti() != rnti))
            {
                return "ue_identity_changed";
            }
        }
    }
    // (d) UE-side state (the UE served by this cell with this RNTI)
    Ptr<LteUeRrc> ueRrc;
    for (uint32_t i = 0; i < g_ues.GetN(); i++)
    {
        auto d = g_ues.Get(i)->GetObject<LteUeNetDevice>();
        if (d && d->GetImsi() == um->GetImsi())
        {
            ueRrc = d->GetRrc();
        }
    }
    if (!ueRrc)
    {
        return "ue_state_UNKNOWN";
    }
    if (ueRrc->GetState() != LteUeRrc::CONNECTED_NORMALLY)
    {
        int s = static_cast<int>(ueRrc->GetState());
        return std::string("ue_state_") + (s >= 0 && s < 13 ? kUeStates[s] : "UNKNOWN");
    }
    // (e) target exists and has an X2 relation with the source
    if (!EnbByCell(targetCell) || !g_x2.count({src, targetCell}))
    {
        return "no_x2_neighbour";
    }
    return "";
}

} // namespace cmdlife
} // namespace xapp
