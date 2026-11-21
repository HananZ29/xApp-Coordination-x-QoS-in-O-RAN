/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * eNB E2 node terminator that also executes OranCommandLteTxPower.
 */

#include "oran-e2-node-terminator-lte-enb-tpc.h"

#include "oran-command-lte-tx-power.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/lte-enb-net-device.h"
#include "ns3/lte-enb-phy.h"
#include "ns3/trace-source-accessor.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranE2NodeTerminatorLteEnbTpc");

NS_OBJECT_ENSURE_REGISTERED(OranE2NodeTerminatorLteEnbTpc);

TypeId
OranE2NodeTerminatorLteEnbTpc::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranE2NodeTerminatorLteEnbTpc")
            .SetParent<OranE2NodeTerminatorLteEnb>()
            .AddConstructor<OranE2NodeTerminatorLteEnbTpc>()
            .AddAttribute("MinTxPowerDbm",
                          "Lowest transmit power a command may set (dBm).",
                          DoubleValue(10.0),
                          MakeDoubleAccessor(&OranE2NodeTerminatorLteEnbTpc::m_minTxPowerDbm),
                          MakeDoubleChecker<double>())
            .AddAttribute("MaxTxPowerDbm",
                          "Highest transmit power a command may set (dBm).",
                          DoubleValue(40.0),
                          MakeDoubleAccessor(&OranE2NodeTerminatorLteEnbTpc::m_maxTxPowerDbm),
                          MakeDoubleChecker<double>())
            .AddTraceSource("TxPowerChange",
                            "Fired when a command changes the eNB transmit power.",
                            MakeTraceSourceAccessor(
                                &OranE2NodeTerminatorLteEnbTpc::m_txPowerChangeTrace),
                            "ns3::OranE2NodeTerminatorLteEnbTpc::TxPowerChangeTracedCallback");
    return tid;
}

OranE2NodeTerminatorLteEnbTpc::OranE2NodeTerminatorLteEnbTpc()
    : OranE2NodeTerminatorLteEnb()
{
    NS_LOG_FUNCTION(this);
}

OranE2NodeTerminatorLteEnbTpc::~OranE2NodeTerminatorLteEnbTpc()
{
    NS_LOG_FUNCTION(this);
}

void
OranE2NodeTerminatorLteEnbTpc::ReceiveCommand(Ptr<OranCommand> command)
{
    NS_LOG_FUNCTION(this << command);

    if (command->GetInstanceTypeId() == OranCommandLteTxPower::GetTypeId())
    {
        if (!m_active)
        {
            return;
        }
        Ptr<OranCommandLteTxPower> txCmd = command->GetObject<OranCommandLteTxPower>();
        Ptr<LteEnbNetDevice> dev = GetNetDevice();
        Ptr<LteEnbPhy> phy = dev->GetPhy();

        double oldDbm = phy->GetTxPower();
        double newDbm = std::clamp(txCmd->GetTxPowerDbm(), m_minTxPowerDbm, m_maxTxPowerDbm);
        phy->SetTxPower(newDbm);

        NS_LOG_INFO("Cell " << dev->GetCellId() << " TxPower " << oldDbm << " -> " << newDbm
                            << " dBm");
        m_txPowerChangeTrace(dev->GetCellId(), oldDbm, newDbm);
        return;
    }

    // Handover and any other command type: original behaviour.
    OranE2NodeTerminatorLteEnb::ReceiveCommand(command);
}

} // namespace ns3
