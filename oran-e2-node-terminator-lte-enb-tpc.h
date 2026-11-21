/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * eNB E2 node terminator that also executes OranCommandLteTxPower.
 * Extends ns3-oran's OranE2NodeTerminatorLteEnb (NIST); handover commands still work.
 */

#ifndef ORAN_E2_NODE_TERMINATOR_LTE_ENB_TPC_H
#define ORAN_E2_NODE_TERMINATOR_LTE_ENB_TPC_H

#include "ns3/oran-e2-node-terminator-lte-enb.h"
#include "ns3/traced-callback.h"

namespace ns3
{

class OranE2NodeTerminatorLteEnbTpc : public OranE2NodeTerminatorLteEnb
{
  public:
    static TypeId GetTypeId();
    OranE2NodeTerminatorLteEnbTpc();
    ~OranE2NodeTerminatorLteEnbTpc() override;

    /**
     * Executes OranCommandLteTxPower (clamped to [MinTxPowerDbm, MaxTxPowerDbm]);
     * passes every other command to OranE2NodeTerminatorLteEnb.
     */
    void ReceiveCommand(Ptr<OranCommand> command) override;

    /** Signature of the TxPowerChange trace: cell ID, old power (dBm), new power (dBm). */
    typedef void (*TxPowerChangeTracedCallback)(uint16_t cellId, double oldDbm, double newDbm);

  private:
    double m_minTxPowerDbm; //!< Lowest power a command may set (dBm).
    double m_maxTxPowerDbm; //!< Highest power a command may set (dBm).
    TracedCallback<uint16_t, double, double> m_txPowerChangeTrace;
};

} // namespace ns3

#endif // ORAN_E2_NODE_TERMINATOR_LTE_ENB_TPC_H
