/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2: modelled eNB load telemetry.
 *
 * E2-like report from an eNB covering the interval (IntervalStart, Time]:
 * DL data RBs radiated, subframes elapsed, RLC DRB backlog (time-mean over the interval and
 * value at report time), max head-of-line delay at report time, and RLC transmit-buffer drops.
 * All values are computed at the eNB from measurements up to the report time (no lookahead).
 * This is a simulated O-RAN-like report; it does not claim E2SM-KPM conformance.
 */

#ifndef ORAN_REPORT_LTE_ENB_LOAD_H
#define ORAN_REPORT_LTE_ENB_LOAD_H

#include "ns3/nstime.h"
#include "ns3/oran-report.h"

namespace ns3
{

class OranReportLteEnbLoad : public OranReport
{
  public:
    static TypeId GetTypeId();
    OranReportLteEnbLoad();
    ~OranReportLteEnbLoad() override;
    std::string ToString() const override;

    uint16_t GetCellId() const;
    Time GetIntervalStart() const;
    uint64_t GetDataRbSum() const;
    uint64_t GetSubframes() const;
    double GetPrbUtilSched() const; //!< data RBs / (schedulable RBs x subframes)
    double GetPrbUtilPhys() const;  //!< data RBs / (physical RBs x subframes)
    double GetBacklogMeanBytes() const;
    uint64_t GetBacklogNowBytes() const;
    uint32_t GetMaxHolMs() const;
    uint64_t GetRlcDropBytes() const;
    double GetTxPowerDbm() const; //!< S4: cell DL transmit power at report time (NaN = not reported)

  private:
    uint16_t m_cellId;
    Time m_intervalStart;
    uint64_t m_dataRbSum;
    uint64_t m_subframes;
    uint32_t m_nRbSched;
    uint32_t m_nRbPhys;
    double m_backlogMeanBytes;
    uint64_t m_backlogNowBytes;
    uint32_t m_maxHolMs;
    uint64_t m_rlcDropBytes;
    double m_txPowerDbm;
};

} // namespace ns3

#endif // ORAN_REPORT_LTE_ENB_LOAD_H
