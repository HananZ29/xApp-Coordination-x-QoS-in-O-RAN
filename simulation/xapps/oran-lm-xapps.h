/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S4: the independent TS and TPC xApps as ns3-oran Logic Modules.
 * Each Run() at the RIC query instant t_q builds a Snapshot from the RIC telemetry store (reports that
 * arrived by t_q), calls the pure decision function (xapp-policy.h), logs every decision, and issues
 * commands carrying the intended UE identity (V1(f)) through the S3 command lifecycle.
 */
#ifndef ORAN_LM_XAPPS_H
#define ORAN_LM_XAPPS_H

#include "xapp-policy.h"

#include "ns3/nstime.h"
#include "ns3/oran-lm.h"

namespace ns3
{

class OranLmXappTs : public OranLm
{
  public:
    static TypeId GetTypeId();
    OranLmXappTs();
    ~OranLmXappTs() override;

  protected:
    std::vector<Ptr<OranCommand>> Run() override;
    void FinishRun() override;

  private:
    xapp::policy::TsParams Params() const;
    xapp::policy::TsState m_st;
    bool m_shadowOnly{false}; //!< S5 prep diagnostic: decide and log, never issue commands
    double m_holOnMs;
    double m_prbOn;
    int m_kOn;
    double m_prbTargetMax;
    double m_holTargetMaxMs;
    double m_qinDb;
    double m_nReDbm;
    double m_dwellS;
    double m_returnBlockS;
    double m_cooldownS;
    double m_maxAgeS;
    double m_pendingS;
    int m_maxFailures;
    double m_backoffS;

    std::vector<Ptr<OranCommand>> m_inFlight;
};

class OranLmXappTpc : public OranLm
{
  public:
    static TypeId GetTypeId();
    OranLmXappTpc();
    ~OranLmXappTpc() override;

  protected:
    std::vector<Ptr<OranCommand>> Run() override;
    void FinishRun() override;

  private:
    xapp::policy::TpcParams Params() const;
    xapp::policy::TpcState m_st;
    bool m_shadowOnly{false};
    double m_pNomDbm;
    double m_pMinDbm;
    double m_stepDb;
    double m_prbLow;
    double m_holHeadroomMs;
    int m_kDown;
    double m_dwellS;
    double m_alpha;
    double m_qinDb;
    double m_nReDbm;
    double m_maxAgeS;

    std::vector<Ptr<OranCommand>> m_inFlight;
    uint64_t m_nextRequestId{1};
};

} // namespace ns3

#endif
