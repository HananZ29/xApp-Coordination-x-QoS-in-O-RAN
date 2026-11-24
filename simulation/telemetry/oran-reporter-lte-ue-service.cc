/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S4 preparation (see header).
 */
#include "oran-reporter-lte-ue-service.h"

#include "oran-report-lte-ue-service.h"

#include "ns3/log.h"
#include "ns3/lte-enb-net-device.h"
#include "ns3/node.h"
#include "ns3/oran-e2-node-terminator.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranReporterLteUeService");
NS_OBJECT_ENSURE_REGISTERED(OranReporterLteUeService);

TypeId
OranReporterLteUeService::GetTypeId()
{
    static TypeId tid = TypeId("ns3::OranReporterLteUeService")
                            .SetParent<OranReporter>()
                            .AddConstructor<OranReporterLteUeService>();
    return tid;
}

OranReporterLteUeService::OranReporterLteUeService() = default;
OranReporterLteUeService::~OranReporterLteUeService() = default;

std::vector<Ptr<OranReport>>
OranReporterLteUeService::GenerateReports()
{
    std::vector<Ptr<OranReport>> reports;
    if (!m_active)
    {
        return reports;
    }
    NS_ABORT_MSG_IF(m_terminator == nullptr, "reporter without E2 terminator");
    Ptr<Node> node = m_terminator->GetNode();
    Ptr<LteEnbNetDevice> dev;
    for (uint32_t i = 0; i < node->GetNDevices() && !dev; i++)
    {
        dev = DynamicCast<LteEnbNetDevice>(node->GetDevice(i));
    }
    NS_ABORT_MSG_IF(!dev, "OranReporterLteUeService installed on a node without an LTE eNB");
    uint16_t cellId = dev->GetCellId();
    Time now = Simulator::Now();
    std::map<std::pair<uint16_t, uint64_t>, Prev> next;
    for (const auto& u : xapp::MetricsUeCumulative(cellId))
    {
        auto key = std::make_pair(u.rnti, u.imsi);
        auto it = m_prev.find(key);
        Prev p;
        if (it == m_prev.end())
        {
            // first observation of this UE context here (attach or handover in): the counters started at
            // zero when its DRB was created, so the first report covers (DRB creation, now]
            p.t = NanoSeconds(u.createdNs);
        }
        else
        {
            p = it->second;
        }
        Ptr<OranReportLteUeService> r = CreateObject<OranReportLteUeService>();
        r->SetAttribute("ReporterE2NodeId", UintegerValue(m_terminator->GetE2NodeId()));
        r->SetAttribute("Time", TimeValue(now));
        r->SetAttribute("CellId", UintegerValue(cellId));
        r->SetAttribute("Rnti", UintegerValue(u.rnti));
        r->SetAttribute("Imsi", UintegerValue(u.imsi));
        r->SetAttribute("IntervalStart", TimeValue(p.t));
        r->SetAttribute("InBytes", UintegerValue(u.inBytes - p.c.inBytes));
        r->SetAttribute("OutBytes", UintegerValue(u.outBytes - p.c.outBytes));
        r->SetAttribute("DropBytes", UintegerValue(u.dropBytes - p.c.dropBytes));
        r->SetAttribute("AckedBytes", UintegerValue(u.ackedBytes - p.c.ackedBytes));
        r->SetAttribute("HarqLostBytes", UintegerValue(u.harqLostBytes - p.c.harqLostBytes));
        r->SetAttribute("BacklogNowBytes", UintegerValue(u.backlogBytesNow));
        r->SetAttribute("HolNowMs", UintegerValue(u.maxHolMsNow));
        reports.push_back(r);
        next[key] = Prev{now, u};
    }
    m_prev = next; // contexts that disappeared (handover out, release) are forgotten
    return reports;
}

} // namespace ns3
