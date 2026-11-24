/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S4 preparation: per-UE DL service telemetry.
 *
 * E2-like report from an eNB about ONE connected UE, covering (IntervalStart, Time]:
 *   InBytes      PDCP PDU bytes that arrived for the UE's DRBs (DL demand seen by the eNB)
 *   OutBytes     RLC payload bytes handed to MAC for the UE (DL volume served, first transmission)
 *   DropBytes    RLC transmit-buffer drops
 *   AckedBytes   RLC payload in DL transport blocks positively HARQ-acknowledged by the UE (delivered)
 *   HarqLostBytes RLC payload in DL transport blocks abandoned without ACK
 *   BacklogNow / HolNowMs   RLC queue and head-of-line delay at report time.
 * Network-side measurements only: no application-receiver goodput, no lookahead. Bytes include
 * PDCP/RLC-level IP packet sizes (UDP/IP headers), not application payload.
 * Analogous in spirit to O-RAN E2SM-KPM per-UE DRB measurements; no conformance is claimed.
 */
#ifndef ORAN_REPORT_LTE_UE_SERVICE_H
#define ORAN_REPORT_LTE_UE_SERVICE_H

#include "ns3/nstime.h"
#include "ns3/oran-report.h"

namespace ns3
{

class OranReportLteUeService : public OranReport
{
  public:
    static TypeId GetTypeId();
    OranReportLteUeService();
    ~OranReportLteUeService() override;
    std::string ToString() const override;

    uint16_t GetCellId() const;
    uint16_t GetRnti() const;
    uint64_t GetImsi() const;
    Time GetIntervalStart() const;
    uint64_t GetInBytes() const;
    uint64_t GetOutBytes() const;
    uint64_t GetDropBytes() const;
    uint64_t GetAckedBytes() const;
    uint64_t GetHarqLostBytes() const;
    uint64_t GetBacklogNowBytes() const;
    uint32_t GetHolNowMs() const;

  private:
    uint16_t m_cellId;
    uint16_t m_rnti;
    uint64_t m_imsi;
    Time m_intervalStart;
    uint64_t m_inBytes;
    uint64_t m_outBytes;
    uint64_t m_dropBytes;
    uint64_t m_ackedBytes;
    uint64_t m_harqLostBytes;
    uint64_t m_backlogNowBytes;
    uint32_t m_holNowMs;
};

} // namespace ns3

#endif
