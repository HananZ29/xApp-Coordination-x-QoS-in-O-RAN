/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S3 TEST FIXTURE (see header).
 */
#include "oran-lm-scripted-commands.h"

#include "ric-telemetry-store.h"

#include "cmd-lifecycle.h"
#include "oran-command-lte-tx-power.h"

#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/log.h"
#include "ns3/lte-ue-net-device.h"
#include "ns3/lte-ue-rrc.h"
#include "ns3/oran-command-lte-2-lte-handover.h"
#include "ns3/oran-data-repository.h"
#include "ns3/oran-near-rt-ric.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <sstream>
#include <tuple>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranLmScriptedCommands");
NS_OBJECT_ENSURE_REGISTERED(OranLmScriptedCommands);

TypeId
OranLmScriptedCommands::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranLmScriptedCommands")
            .SetParent<OranLm>()
            .AddConstructor<OranLmScriptedCommands>()
            .AddAttribute("Script",
                          "Command script (see header).",
                          StringValue(""),
                          MakeStringAccessor(&OranLmScriptedCommands::m_script),
                          MakeStringChecker());
    return tid;
}

OranLmScriptedCommands::OranLmScriptedCommands()
    : OranLm()
{
    m_name = "OranLmScriptedCommands";
}

OranLmScriptedCommands::~OranLmScriptedCommands() = default;

void
OranLmScriptedCommands::Parse()
{
    m_parsed = true;
    std::stringstream ss(m_script);
    std::string item;
    while (std::getline(ss, item, ';'))
    {
        if (item.empty())
        {
            continue;
        }
        std::stringstream is(item);
        std::string f;
        std::vector<std::string> fs;
        while (std::getline(is, f, ':'))
        {
            fs.push_back(f);
        }
        NS_ABORT_MSG_IF(fs.size() < 3, "bad script entry: " << item);
        Entry e;
        e.text = item;
        e.t = std::stod(fs[0]);
        e.kind = fs[1];
        NS_ABORT_MSG_IF(e.kind != "ho" && e.kind != "tpc", "bad command kind: " << item);
        for (size_t k = 2; k < fs.size(); k++)
        {
            auto p = fs[k].find('=');
            NS_ABORT_MSG_IF(p == std::string::npos, "bad field: " << item);
            std::string key = fs[k].substr(0, p);
            std::string v = fs[k].substr(p + 1);
            if (key == "ue")
                e.ue = std::stoi(v);
            else if (key == "cell")
                e.cell = std::stoi(v);
            else if (key == "rnti")
                e.rnti = std::stoi(v);
            else if (key == "src")
                e.src = std::stoi(v);
            else if (key == "dbm")
                e.dbm = (v == "nan") ? std::nan("") : std::stod(v);
            else if (key == "ev")
                e.ev = std::stod(v);
            else
                NS_ABORT_MSG("unknown field " << key << " in " << item);
        }
        m_entries.push_back(e);
    }
}

std::vector<Ptr<OranCommand>>
OranLmScriptedCommands::Run()
{
    if (!m_parsed)
    {
        Parse();
    }
    m_runCycle = Simulator::Now();
    std::vector<Ptr<OranCommand>> out;
    // The RIC cancels a run that has not finished by the next query (OranLm::CancelRun is not
    // virtual): commands still in flight here were lost with it.
    for (auto& c : m_inFlight)
    {
        xapp::cmdlife::Stage(c, "lm_cancelled", "", "run_not_finished_by_next_query");
        xapp::cmdlife::Final(c, "lm_cancelled", "run_not_finished_by_next_query");
    }
    m_inFlight.clear();
    if (!m_active)
    {
        return out;
    }
    Ptr<OranDataRepository> data = m_nearRtRic->Data();
    std::map<uint16_t, uint64_t> e2OfCell;
    for (auto id : data->GetLteEnbE2NodeIds())
    {
        bool found;
        uint16_t cell;
        std::tie(found, cell) = data->GetLteEnbCellInfo(id);
        if (found)
        {
            e2OfCell[cell] = id;
        }
    }
    double now = Simulator::Now().GetSeconds();
    for (auto& e : m_entries)
    {
        if (e.issued || now + 1e-12 < e.t)
        {
            continue;
        }
        if (e.kind == "tpc")
        {
            if (!e2OfCell.count(e.cell))
            {
                continue; // eNB not registered yet: retry next cycle
            }
            auto cmd = CreateObject<OranCommandLteTxPower>();
            cmd->SetAttribute("TargetE2NodeId", UintegerValue(e2OfCell[e.cell]));
            cmd->SetAttribute("TxPowerDbm", DoubleValue(e.dbm));
            cmd->SetAttribute("RequestId", UintegerValue(m_nextRequestId++));
            cmd->SetAttribute("CreatedAt", TimeValue(Simulator::Now()));
            xapp::cmdlife::Created(cmd, m_name, -1, "script " + e.text);
            if (e.ev > -1e299)
                xapp::ric::RegisterEvidence(xapp::cmdlife::IdOf(cmd), e.ev);
            out.push_back(cmd);
        }
        else
        {
            auto dev = xapp::cmdlife::UeDevice(e.ue);
            NS_ABORT_MSG_IF(!dev, "script UE index out of range: " << e.text);
            auto ue = dev->GetObject<LteUeNetDevice>();
            uint16_t src = e.src >= 0 ? e.src : ue->GetRrc()->GetCellId();
            uint16_t rnti = e.rnti >= 0 ? e.rnti : ue->GetRrc()->GetRnti();
            if (!e2OfCell.count(src))
            {
                continue;
            }
            auto cmd = CreateObject<OranCommandLte2LteHandover>();
            cmd->SetAttribute("TargetE2NodeId", UintegerValue(e2OfCell[src]));
            cmd->SetAttribute("TargetCellId", UintegerValue(e.cell));
            cmd->SetAttribute("TargetRnti", UintegerValue(rnti));
            xapp::cmdlife::Created(cmd, m_name, static_cast<int64_t>(ue->GetImsi()),
                                   "script " + e.text + " src=" + std::to_string(src));
            if (e.ev > -1e299)
                xapp::ric::RegisterEvidence(xapp::cmdlife::IdOf(cmd), e.ev);
            data->LogCommandLm(m_name, cmd);
            out.push_back(cmd);
        }
        e.issued = true;
    }
    m_inFlight = out;
    return out;
}

void
OranLmScriptedCommands::FinishRun()
{
    TimeValue mw;
    EnumValue<OranNearRtRic::LateCommandPolicy> pol;
    m_nearRtRic->GetAttribute("LmQueryMaxWaitTime", mw);
    m_nearRtRic->GetAttribute("LmQueryLateCommandPolicy", pol);
    Time elapsed = Simulator::Now() - m_runCycle;
    // The RIC deadline event is scheduled before this LM's finish event, so at equal time the
    // deadline fires first: late iff elapsed >= max wait (> 0).
    bool late = mw.Get() > Seconds(0) && elapsed >= mw.Get();
    for (auto& c : m_inFlight)
    {
        xapp::cmdlife::Stage(c, "lm_output", "", "", "processing done");
        if (late)
        {
            bool drop = pol.Get() == OranNearRtRic::DROP;
            xapp::cmdlife::Stage(c, "lm_late", drop ? "dropped_late" : "saved_late",
                                 "after_ric_deadline");
            if (drop)
            {
                xapp::cmdlife::Final(c, "dropped_late", "after_ric_deadline");
            }
        }
    }
    m_inFlight.clear();
    OranLm::FinishRun();
}

} // namespace ns3
