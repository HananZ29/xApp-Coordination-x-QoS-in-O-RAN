/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * eNB E2 node terminator that also executes OranCommandLteTxPower.
 */

#include "oran-e2-node-terminator-lte-enb-tpc.h"

#include "cmd-lifecycle.h"
#include "oran-command-lte-tx-power.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/lte-enb-net-device.h"
#include "ns3/lte-enb-phy.h"
#include "ns3/oran-command-lte-2-lte-handover.h"
#include "ns3/trace-source-accessor.h"

#include <algorithm>
#include <cmath>

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
                            "ns3::OranE2NodeTerminatorLteEnbTpc::TxPowerChangeTracedCallback")
            .AddTraceSource("TxPowerCommand",
                            "Fired for every received power command, executed or not.",
                            MakeTraceSourceAccessor(
                                &OranE2NodeTerminatorLteEnbTpc::m_txPowerCommandTrace),
                            "ns3::OranE2NodeTerminatorLteEnbTpc::TxPowerCommandTracedCallback");
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
OranE2NodeTerminatorLteEnbTpc::TestSetInactive()
{
    m_active = false;
}

void
OranE2NodeTerminatorLteEnbTpc::ReceiveCommand(Ptr<OranCommand> command)
{
    NS_LOG_FUNCTION(this << command);

    // S3: every command is logged on arrival and ends in a logged outcome (no silent drops).
    const bool life = xapp::cmdlife::IsOpen();
    if (life)
    {
        xapp::cmdlife::Stage(command, "e2_received", "", "",
                             "cell " + std::to_string(GetNetDevice()->GetCellId()));
    }

    if (command->GetInstanceTypeId() == OranCommandLteTxPower::GetTypeId())
    {
        Ptr<OranCommandLteTxPower> txCmd = command->GetObject<OranCommandLteTxPower>();
        Ptr<LteEnbNetDevice> dev = GetNetDevice();
        Ptr<LteEnbPhy> phy = dev->GetPhy();
        double oldDbm = phy->GetTxPower();

        if (!m_active)
        {
            // Not executed, but no longer silent (S1).
            m_txPowerCommandTrace(dev->GetCellId(),
                                  txCmd->GetRequestId(),
                                  txCmd->GetCreatedAt(),
                                  txCmd->GetTxPowerDbm(),
                                  oldDbm,
                                  oldDbm,
                                  "dropped_inactive");
            if (life)
            {
                xapp::cmdlife::Stage(command, "validated", "rejected", "terminator_inactive");
                xapp::cmdlife::Final(command, "rejected", "terminator_inactive");
            }
            return;
        }
        if (!std::isfinite(txCmd->GetTxPowerDbm()))
        {
            // V2: a non-numeric value cannot be clamped (S3; never occurred before S3).
            m_txPowerCommandTrace(dev->GetCellId(),
                                  txCmd->GetRequestId(),
                                  txCmd->GetCreatedAt(),
                                  txCmd->GetTxPowerDbm(),
                                  oldDbm,
                                  oldDbm,
                                  "rejected_invalid_value");
            if (life)
            {
                xapp::cmdlife::Stage(command, "validated", "rejected", "invalid_value");
                xapp::cmdlife::Final(command, "rejected", "invalid_value");
            }
            return;
        }

        double newDbm = std::clamp(txCmd->GetTxPowerDbm(), m_minTxPowerDbm, m_maxTxPowerDbm);
        if (life)
        {
            xapp::cmdlife::Stage(command, "validated", "accepted", "",
                                 newDbm == txCmd->GetTxPowerDbm() ? "" : "value clamped");
        }
        phy->SetTxPower(newDbm);

        NS_LOG_INFO("Cell " << dev->GetCellId() << " TxPower " << oldDbm << " -> " << newDbm
                            << " dBm");
        m_txPowerChangeTrace(dev->GetCellId(), oldDbm, newDbm);
        // Takes effect when the PHY next builds a transmit PSD (next subframe); the scenario
        // observes the radiated change on the spectrum channel (t_a).
        const std::string status =
            newDbm == txCmd->GetTxPowerDbm() ? "applied" : "applied_clamped";
        m_txPowerCommandTrace(dev->GetCellId(),
                              txCmd->GetRequestId(),
                              txCmd->GetCreatedAt(),
                              txCmd->GetTxPowerDbm(),
                              oldDbm,
                              newDbm,
                              status);
        if (life)
        {
            xapp::cmdlife::PowerApplied(command, dev->GetCellId(), txCmd->GetTxPowerDbm(), oldDbm,
                                        newDbm, status);
        }
        return;
    }

    if (command->GetInstanceTypeId() == OranCommandLte2LteHandover::GetTypeId())
    {
        if (!life)
        {
            // No lifecycle log: original NIST behaviour (unchanged for S0-S2 runs).
            OranE2NodeTerminatorLteEnb::ReceiveCommand(command);
            return;
        }
        Ptr<OranCommandLte2LteHandover> ho = command->GetObject<OranCommandLte2LteHandover>();
        if (!m_active)
        {
            // The NIST terminator ignores the command silently in this case.
            xapp::cmdlife::Stage(command, "validated", "rejected", "terminator_inactive");
            xapp::cmdlife::Final(command, "rejected", "terminator_inactive");
            return;
        }
        int64_t imsi = -1;
        const int64_t intended = xapp::cmdlife::IntendedImsi(command);
        std::string reason = xapp::cmdlife::CheckV1(GetNetDevice(), ho->GetTargetRnti(),
                                                    ho->GetTargetCellId(), intended, &imsi);
        if (!reason.empty())
        {
            xapp::cmdlife::Stage(command, "validated", "rejected", reason);
            xapp::cmdlife::Final(command, "rejected", reason);
            return;
        }
        xapp::cmdlife::Stage(command, "validated", "accepted", "",
                             intended >= 0 ? "identity verified imsi " + std::to_string(imsi)
                                           : "identity_unverified (LM recorded no intended UE)");
        xapp::cmdlife::HoRequested(command, imsi, GetNetDevice()->GetCellId(),
                                   ho->GetTargetCellId(), ho->GetTargetRnti());
        OranE2NodeTerminatorLteEnb::ReceiveCommand(command);
        return;
    }

    // Any other command type is ignored by the NIST eNB terminator.
    if (life)
    {
        xapp::cmdlife::Stage(command, "validated", "rejected", "unsupported_command_type");
        xapp::cmdlife::Final(command, "rejected", "unsupported_command_type");
    }
    OranE2NodeTerminatorLteEnb::ReceiveCommand(command);
}

} // namespace ns3
