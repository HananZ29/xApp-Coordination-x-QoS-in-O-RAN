/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * eNB E2 node terminator that also executes OranCommandLteTxPower.
 * Extends ns3-oran's OranE2NodeTerminatorLteEnb (NIST); handover commands still work.
 */

#ifndef ORAN_E2_NODE_TERMINATOR_LTE_ENB_TPC_H
#define ORAN_E2_NODE_TERMINATOR_LTE_ENB_TPC_H

#include "ns3/oran-e2-node-terminator-lte-enb.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

#include <string>

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

    /**
     * TEST ONLY (S3): mark this terminator inactive without calling the NIST Deactivate(), which
     * aborts in ns3-oran 5e60f2d (OranReportTrigger::Deactivate sets the reporter's Trigger
     * attribute to null, which the attribute system rejects). No deregistration is sent.
     */
    void TestSetInactive();

    /** Signature of the TxPowerChange trace: cell ID, old power (dBm), new power (dBm). */
    typedef void (*TxPowerChangeTracedCallback)(uint16_t cellId, double oldDbm, double newDbm);

    /**
     * Signature of the TxPowerCommand trace, fired for EVERY received power command,
     * including commands that are not executed (S1: no silent drops).
     * status: "applied", "applied_clamped" or "dropped_inactive".
     */
    typedef void (*TxPowerCommandTracedCallback)(uint16_t cellId,
                                                 uint64_t requestId,
                                                 Time createdAt,
                                                 double requestedDbm,
                                                 double oldDbm,
                                                 double appliedDbm,
                                                 const std::string& status);

  private:
    double m_minTxPowerDbm; //!< Lowest power a command may set (dBm).
    double m_maxTxPowerDbm; //!< Highest power a command may set (dBm).
    TracedCallback<uint16_t, double, double> m_txPowerChangeTrace;
    TracedCallback<uint16_t, uint64_t, Time, double, double, double, const std::string&>
        m_txPowerCommandTrace;
};

} // namespace ns3

#endif // ORAN_E2_NODE_TERMINATOR_LTE_ENB_TPC_H
