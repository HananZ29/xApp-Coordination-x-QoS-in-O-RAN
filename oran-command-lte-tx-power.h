/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * O-RAN command that sets the downlink transmit power of an LTE eNB.
 * Modelled on ns3-oran's OranCommandLte2LteHandover (NIST).
 */

#ifndef ORAN_COMMAND_LTE_TX_POWER_H
#define ORAN_COMMAND_LTE_TX_POWER_H

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

  private:
    double m_txPowerDbm; //!< Requested transmit power (dBm).
};

} // namespace ns3

#endif // ORAN_COMMAND_LTE_TX_POWER_H
