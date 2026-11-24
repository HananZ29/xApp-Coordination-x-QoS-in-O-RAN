/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2: RIC-side telemetry sink.
 *
 * ns3-oran's Near-RT RIC E2 terminator stores only its built-in report types in the data
 * repository, but passes EVERY received report to each registered query trigger
 * (OranNearRtRicE2Terminator::ReceiveReport -> OranNearRtRic::NotifyReportReceived). This
 * trigger records every report at its RIC arrival time (generation time, age, payload) and
 * never requests an LM query (always returns false), so it does not alter RIC behaviour.
 */

#ifndef ORAN_QUERY_TRIGGER_TELEMETRY_SINK_H
#define ORAN_QUERY_TRIGGER_TELEMETRY_SINK_H

#include "ns3/oran-query-trigger.h"

namespace ns3
{

class OranQueryTriggerTelemetrySink : public OranQueryTrigger
{
  public:
    static TypeId GetTypeId();
    OranQueryTriggerTelemetrySink();
    ~OranQueryTriggerTelemetrySink() override;
    bool QueryLms(Ptr<OranReport> report) override;
};

} // namespace ns3

#endif // ORAN_QUERY_TRIGGER_TELEMETRY_SINK_H
