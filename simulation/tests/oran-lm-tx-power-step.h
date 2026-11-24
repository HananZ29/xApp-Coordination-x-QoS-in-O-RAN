/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * Test Logic Module (xApp): issues ONE OranCommandLteTxPower to a chosen cell
 * at the first RIC cycle at or after StepTime. Used only to verify the
 * RIC -> E2 terminator -> eNB power path (Phase 2b); replaced by the TPC xApp in Phase 3.
 */

#ifndef ORAN_LM_TX_POWER_STEP_H
#define ORAN_LM_TX_POWER_STEP_H

#include "ns3/nstime.h"
#include "ns3/oran-lm.h"

namespace ns3
{

class OranLmTxPowerStep : public OranLm
{
  public:
    static TypeId GetTypeId();
    OranLmTxPowerStep();
    ~OranLmTxPowerStep() override;

  protected:
    std::vector<Ptr<OranCommand>> Run() override;

  private:
    Time m_stepTime;         //!< Earliest time the command may be issued.
    uint16_t m_targetCellId; //!< Cell whose power is changed.
    double m_txPowerDbm;     //!< Power to set (dBm).
    bool m_issued{false};    //!< True once the command has been issued.
    uint64_t m_nextRequestId{1}; //!< Next request identifier (S1).
};

} // namespace ns3

#endif // ORAN_LM_TX_POWER_STEP_H
