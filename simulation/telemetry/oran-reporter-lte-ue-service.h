/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S4 preparation: reporter for OranReportLteUeService.
 * On each trigger: one report per UE with open DRBs at this eNB; counters are differences against the
 * previous trigger for the same (RNTI, IMSI). A UE seen for the first time (attach or handover in) is
 * reported over (DRB creation at this eNB, now], so there is no reporting gap and no report spans two cells.
 */
#ifndef ORAN_REPORTER_LTE_UE_SERVICE_H
#define ORAN_REPORTER_LTE_UE_SERVICE_H

#include "ns3/nstime.h"
#include "ns3/oran-reporter.h"

#include "xapp-metrics.h"

#include <map>
#include <utility>

namespace ns3
{

class OranReporterLteUeService : public OranReporter
{
  public:
    static TypeId GetTypeId();
    OranReporterLteUeService();
    ~OranReporterLteUeService() override;

  protected:
    std::vector<Ptr<OranReport>> GenerateReports() override;

  private:
    struct Prev
    {
        Time t;
        xapp::UeCumulative c;
    };
    std::map<std::pair<uint16_t, uint64_t>, Prev> m_prev; //!< (rnti, imsi) -> last trigger
};

} // namespace ns3

#endif
