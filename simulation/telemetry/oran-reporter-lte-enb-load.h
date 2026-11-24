/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2: reporter for OranReportLteEnbLoad.
 *
 * Installed on an eNB E2 node terminator. Each time its trigger fires, it reads the eNB's
 * cumulative counters at that instant (xapp::MetricsCellCumulative) and reports the difference
 * since its previous report. The report then follows the normal ns3-oran path: buffered at the
 * E2 node terminator until its next send (SendIntervalRv), then delivered to the RIC after the
 * terminator's TransmissionDelayRv.
 */

#ifndef ORAN_REPORTER_LTE_ENB_LOAD_H
#define ORAN_REPORTER_LTE_ENB_LOAD_H

#include "ns3/nstime.h"
#include "ns3/oran-reporter.h"

#include "xapp-metrics.h"

namespace ns3
{

class OranReporterLteEnbLoad : public OranReporter
{
  public:
    static TypeId GetTypeId();
    OranReporterLteEnbLoad();
    ~OranReporterLteEnbLoad() override;

  protected:
    std::vector<Ptr<OranReport>> GenerateReports() override;

  private:
    bool m_havePrev{false};
    Time m_prevTime;
    xapp::CellCumulative m_prev;
    uint32_t m_nRbSched;
    uint32_t m_nRbPhys;
    bool m_reportTxPower{false};
};

} // namespace ns3

#endif // ORAN_REPORTER_LTE_ENB_LOAD_H
