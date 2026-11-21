/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * O-RAN command that sets the downlink transmit power of an LTE eNB.
 */

#include "oran-command-lte-tx-power.h"

#include "ns3/double.h"
#include "ns3/log.h"

#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranCommandLteTxPower");

NS_OBJECT_ENSURE_REGISTERED(OranCommandLteTxPower);

TypeId
OranCommandLteTxPower::GetTypeId()
{
    static TypeId tid = TypeId("ns3::OranCommandLteTxPower")
                            .SetParent<OranCommand>()
                            .AddConstructor<OranCommandLteTxPower>()
                            .AddAttribute("TxPowerDbm",
                                          "The transmit power to set on the target eNB (dBm).",
                                          DoubleValue(30.0),
                                          MakeDoubleAccessor(&OranCommandLteTxPower::m_txPowerDbm),
                                          MakeDoubleChecker<double>());
    return tid;
}

OranCommandLteTxPower::OranCommandLteTxPower()
    : OranCommand()
{
    NS_LOG_FUNCTION(this);
}

OranCommandLteTxPower::~OranCommandLteTxPower()
{
    NS_LOG_FUNCTION(this);
}

std::string
OranCommandLteTxPower::ToString() const
{
    std::stringstream ss;
    ss << "OranCommandLteTxPower(TargetE2NodeId = " << GetTargetE2NodeId()
       << "; TxPowerDbm = " << m_txPowerDbm << ")";
    return ss.str();
}

double
OranCommandLteTxPower::GetTxPowerDbm() const
{
    return m_txPowerDbm;
}

} // namespace ns3
