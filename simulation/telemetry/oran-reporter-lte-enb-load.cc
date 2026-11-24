/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2: reporter for OranReportLteEnbLoad.
 */

#include "oran-reporter-lte-enb-load.h"

#include "oran-report-lte-enb-load.h"

#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/lte-enb-phy.h"
#include "ns3/log.h"
#include "ns3/lte-enb-net-device.h"
#include "ns3/node.h"
#include "ns3/oran-e2-node-terminator.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranReporterLteEnbLoad");
NS_OBJECT_ENSURE_REGISTERED(OranReporterLteEnbLoad);

TypeId
OranReporterLteEnbLoad::GetTypeId()
{
    static TypeId tid = TypeId("ns3::OranReporterLteEnbLoad")
                            .SetParent<OranReporter>()
                            .AddConstructor<OranReporterLteEnbLoad>()
                            .AddAttribute("SchedulableRbs",
                                          "Schedulable RBs per subframe (PF Type-0: 24 for 25 RB).",
                                          UintegerValue(24),
                                          MakeUintegerAccessor(&OranReporterLteEnbLoad::m_nRbSched),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("PhysicalRbs",
                                          "Physical RBs per subframe.",
                                          UintegerValue(25),
                                          MakeUintegerAccessor(&OranReporterLteEnbLoad::m_nRbPhys),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("ReportTxPower",
                                          "S4: include the cell's current DL transmit power.",
                                          BooleanValue(false),
                                          MakeBooleanAccessor(&OranReporterLteEnbLoad::m_reportTxPower),
                                          MakeBooleanChecker());
    return tid;
}

OranReporterLteEnbLoad::OranReporterLteEnbLoad()
    : OranReporter()
{
}

OranReporterLteEnbLoad::~OranReporterLteEnbLoad() = default;

std::vector<Ptr<OranReport>>
OranReporterLteEnbLoad::GenerateReports()
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
    NS_ABORT_MSG_IF(!dev, "OranReporterLteEnbLoad installed on a node without an LTE eNB");
    uint16_t cellId = dev->GetCellId();

    Time now = Simulator::Now();
    xapp::CellCumulative cur = xapp::MetricsCellCumulative(cellId);
    if (!m_havePrev)
    {
        // first trigger: establish the baseline, report nothing
        m_prev = cur;
        m_prevTime = now;
        m_havePrev = true;
        return reports;
    }
    double dt = (now - m_prevTime).GetSeconds();
    Ptr<OranReportLteEnbLoad> r = CreateObject<OranReportLteEnbLoad>();
    r->SetAttribute("ReporterE2NodeId", UintegerValue(m_terminator->GetE2NodeId()));
    r->SetAttribute("Time", TimeValue(now));
    r->SetAttribute("CellId", UintegerValue(cellId));
    r->SetAttribute("IntervalStart", TimeValue(m_prevTime));
    r->SetAttribute("DataRbSum", UintegerValue(cur.dataRbSum - m_prev.dataRbSum));
    r->SetAttribute("Subframes", UintegerValue((now - m_prevTime).GetMilliSeconds()));
    r->SetAttribute("SchedulableRbs", UintegerValue(m_nRbSched));
    r->SetAttribute("PhysicalRbs", UintegerValue(m_nRbPhys));
    r->SetAttribute("BacklogMeanBytes",
                    DoubleValue(dt > 0 ? (cur.backlogByteSeconds - m_prev.backlogByteSeconds) / dt : 0));
    r->SetAttribute("BacklogNowBytes", UintegerValue(cur.backlogBytesNow));
    r->SetAttribute("MaxHolMs", UintegerValue(cur.maxHolMsNow));
    r->SetAttribute("RlcDropBytes", UintegerValue(cur.rlcDropBytes - m_prev.rlcDropBytes));
    if (m_reportTxPower)
    {
        r->SetAttribute("TxPowerDbm", DoubleValue(dev->GetPhy()->GetTxPower()));
    }
    reports.push_back(r);
    m_prev = cur;
    m_prevTime = now;
    return reports;
}

} // namespace ns3
