/*
 * CMPS 342 project (H. Zoueihed, N. Sakr), Nov 2026.
 * Test Logic Module: one transmit-power command at StepTime.
 */

#include "oran-lm-tx-power-step.h"

#include "cmd-lifecycle.h"
#include "oran-command-lte-tx-power.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/oran-data-repository.h"
#include "ns3/oran-near-rt-ric.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <tuple>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranLmTxPowerStep");

NS_OBJECT_ENSURE_REGISTERED(OranLmTxPowerStep);

TypeId
OranLmTxPowerStep::GetTypeId()
{
    static TypeId tid = TypeId("ns3::OranLmTxPowerStep")
                            .SetParent<OranLm>()
                            .AddConstructor<OranLmTxPowerStep>()
                            .AddAttribute("StepTime",
                                          "Earliest time the power command is issued.",
                                          TimeValue(Seconds(15)),
                                          MakeTimeAccessor(&OranLmTxPowerStep::m_stepTime),
                                          MakeTimeChecker())
                            .AddAttribute("TargetCellId",
                                          "Cell ID whose transmit power is changed.",
                                          UintegerValue(2),
                                          MakeUintegerAccessor(&OranLmTxPowerStep::m_targetCellId),
                                          MakeUintegerChecker<uint16_t>())
                            .AddAttribute("TxPowerDbm",
                                          "Transmit power to set (dBm).",
                                          DoubleValue(24.0),
                                          MakeDoubleAccessor(&OranLmTxPowerStep::m_txPowerDbm),
                                          MakeDoubleChecker<double>());
    return tid;
}

OranLmTxPowerStep::OranLmTxPowerStep()
    : OranLm()
{
    NS_LOG_FUNCTION(this);
    m_name = "OranLmTxPowerStep";
}

OranLmTxPowerStep::~OranLmTxPowerStep()
{
    NS_LOG_FUNCTION(this);
}

std::vector<Ptr<OranCommand>>
OranLmTxPowerStep::Run()
{
    NS_LOG_FUNCTION(this);

    std::vector<Ptr<OranCommand>> commands;
    if (!m_active || m_issued || Simulator::Now() < m_stepTime)
    {
        return commands;
    }

    NS_ABORT_MSG_IF(m_nearRtRic == nullptr, "LM " + m_name + " has no Near-RT RIC");
    Ptr<OranDataRepository> data = m_nearRtRic->Data();

    for (auto enbId : data->GetLteEnbE2NodeIds())
    {
        bool found;
        uint16_t cellId;
        std::tie(found, cellId) = data->GetLteEnbCellInfo(enbId);
        if (found && cellId == m_targetCellId)
        {
            Ptr<OranCommandLteTxPower> cmd = CreateObject<OranCommandLteTxPower>();
            cmd->SetAttribute("TargetE2NodeId", UintegerValue(enbId));
            cmd->SetAttribute("TxPowerDbm", DoubleValue(m_txPowerDbm));
            cmd->SetAttribute("RequestId", UintegerValue(m_nextRequestId++));
            cmd->SetAttribute("CreatedAt", TimeValue(Simulator::Now()));
            if (xapp::cmdlife::IsOpen())
            {
                xapp::cmdlife::Created(cmd, m_name, -1, "txstep");
            }
            data->LogCommandLm(m_name, cmd);
            LogLogicToRepository("Issuing " + cmd->ToString());
            commands.push_back(cmd);
            m_issued = true;
        }
    }
    if (!m_issued)
    {
        LogLogicToRepository("Target cell " + std::to_string(m_targetCellId) +
                             " not registered yet; will retry next cycle");
    }
    return commands;
}

} // namespace ns3
