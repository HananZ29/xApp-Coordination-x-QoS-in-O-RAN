/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * O-RAN command that sets the downlink transmit power of an LTE eNB.
 * Modelled on ns3-oran's OranCommandLte2LteHandover (NIST).
 */

#ifndef ORAN_COMMAND_LTE_TX_POWER_H
#define ORAN_COMMAND_LTE_TX_POWER_H

#include "ns3/nstime.h"
#include "ns3/oran-command.h"

namespace ns3
{

/**
 * Command to an LTE eNB E2 node: set the eNB transmit power to TxPowerDbm.
 * TargetE2NodeId (inherited) must be the E2 node ID of the eNB.
 */
class OranCommandLteTxPower : public OranCommand
{
  public:
    static TypeId GetTypeId();
    OranCommandLteTxPower();
    ~OranCommandLteTxPower() override;

    std::string ToString() const override;

    /** @return The requested transmit power (dBm). */
    double GetTxPowerDbm() const;

    /** @return Identifier assigned by the issuing LM (0 = not set). */
    uint64_t GetRequestId() const;

    /** @return Simulation time at which the issuing LM created the command (t_q). */
    Time GetCreatedAt() const;

  private:
    double m_txPowerDbm;  //!< Requested transmit power (dBm).
    uint64_t m_requestId; //!< Request identifier (S1: event correlation).
    Time m_createdAt;     //!< Creation time in simulation time (S1: event correlation).
};

} // namespace ns3

#endif // ORAN_COMMAND_LTE_TX_POWER_H
