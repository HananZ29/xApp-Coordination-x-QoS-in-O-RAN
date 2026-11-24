/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2: RIC-side telemetry sink.
 */

#include "oran-query-trigger-telemetry-sink.h"

#include "oran-report-lte-enb-load.h"
#include "oran-report-lte-ue-service.h"
#include "ric-telemetry-store.h"
#include "xapp-metrics.h"

#include "ns3/log.h"
#include "ns3/oran-report-lte-ue-cell-info.h"
#include "ns3/oran-report-lte-ue-rsrp-rsrq.h"

#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranQueryTriggerTelemetrySink");
NS_OBJECT_ENSURE_REGISTERED(OranQueryTriggerTelemetrySink);

TypeId
OranQueryTriggerTelemetrySink::GetTypeId()
{
    static TypeId tid = TypeId("ns3::OranQueryTriggerTelemetrySink")
                            .SetParent<OranQueryTrigger>()
                            .AddConstructor<OranQueryTriggerTelemetrySink>();
    return tid;
}

OranQueryTriggerTelemetrySink::OranQueryTriggerTelemetrySink()
    : OranQueryTrigger()
{
}

OranQueryTriggerTelemetrySink::~OranQueryTriggerTelemetrySink() = default;

bool
OranQueryTriggerTelemetrySink::QueryLms(Ptr<OranReport> report)
{
    std::string type = report->GetInstanceTypeId().GetName();
    std::ostringstream p;
    if (auto r = DynamicCast<OranReportLteEnbLoad>(report))
    {
        p << r->ToString();
    }
    else if (auto r = DynamicCast<OranReportLteUeService>(report))
    {
        p << r->ToString();
    }
    else if (auto r = DynamicCast<OranReportLteUeRsrpRsrq>(report))
    {
        p << "rnti=" << r->GetRnti() << ";cell=" << r->GetCellId() << ";rsrp=" << r->GetRsrp()
          << ";rsrq=" << r->GetRsrq() << ";serving=" << r->GetIsServingCell();
    }
    else if (auto r = DynamicCast<OranReportLteUeCellInfo>(report))
    {
        p << "rnti=" << r->GetRnti() << ";cell=" << r->GetCellId();
    }
    xapp::MetricsTelemetryRow(type, report->GetReporterE2NodeId(), report->GetTime(), p.str());
    xapp::ric::OnReport(report); // S4: the only path into the RIC-side telemetry store
    return false; // never triggers an LM query: observation only
}

} // namespace ns3
