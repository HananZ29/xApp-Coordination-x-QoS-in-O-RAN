/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S3: per-command lifecycle log.
 *
 * Every O-RAN command gets a unique id and one row per lifecycle stage in <out>-cmdlife.csv:
 *
 *   time_ns,time_s,cmd_id,stage,lm,cycle_ns,cmd_type,target_e2_node,cell,target_cell,rnti,imsi,
 *   value_dbm,old_dbm,new_dbm,outcome,reason,detail
 *
 * Stages (in order; a command stops at its first terminal "final" row):
 *   created          LM Run() at the query instant t_q (cycle_ns = t_q)
 *   lm_output        LM FinishRun() = t_q + processing delay
 *   lm_late          output after the RIC deadline (outcome dropped_late = final, or saved_late)
 *   lm_cancelled     the RIC cancelled the LM run before it finished (final)
 *   cmm_in           received by the CMM (all LM outputs of the cycle)
 *   dispatched       handed to the RIC E2 terminator (= start of E2 transport)
 *   e2_received      arrival at the E2 node terminator (actuator)
 *   validated        outcome accepted | rejected (reason = V1/V2 code; rejected is final)
 *   applied          power: PHY attribute set (outcome applied | applied_clamped | noop_same_value);
 *                    handover: SendHandoverRequest() called (outcome ho_requested)
 *   phy_effect       power: first radiated frame at the new power
 *   ho_start_enb / ho_start_ue / ho_completed_ue / ho_completed_enb   handover procedure
 *   superseded       power: a later command for the same cell was applied before this one radiated
 *   final            exactly one per command: outcome = effect_observed | completed | rejected |
 *                    noop_same_value | superseded | dropped_late | lm_cancelled | ho_failed |
 *                    not_dispatched_by_end | not_received_by_end | not_effective_by_end |
 *                    not_completed_by_end
 *
 * Commands are tracked by object identity; the log keeps a reference so ids are never reused.
 */
#ifndef XAPP_CMD_LIFECYCLE_H
#define XAPP_CMD_LIFECYCLE_H

#include "ns3/lte-enb-net-device.h"
#include "ns3/net-device-container.h"
#include "ns3/nstime.h"
#include "ns3/oran-command.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <string>

namespace xapp
{
namespace cmdlife
{

void Open(const std::string& outPrefix);
bool IsOpen();
/** Writes a terminal row for every command that has none (closure), then closes the file. */
void Close();

/** Test-fixture registries (ground truth; used by the scripted test LM and by V1). */
void RegisterDevices(const ns3::NetDeviceContainer& ues, const ns3::NetDeviceContainer& enbs);
void RegisterX2(uint16_t cellA, uint16_t cellB);
ns3::Ptr<ns3::NetDevice> UeDevice(uint32_t ueIndex);
uint32_t NumUes();
ns3::Ptr<ns3::LteEnbNetDevice> EnbByCell(uint16_t cellId);

/** LM side. */
uint64_t Created(ns3::Ptr<ns3::OranCommand> c, const std::string& lm, int64_t imsi,
                 const std::string& detail);
void Stage(ns3::Ptr<ns3::OranCommand> c, const std::string& stage, const std::string& outcome = "",
           const std::string& reason = "", const std::string& detail = "");
/** Terminal row (ignored if the command already has one). */
void Final(ns3::Ptr<ns3::OranCommand> c, const std::string& outcome, const std::string& reason = "",
           const std::string& detail = "");
bool HasStage(ns3::Ptr<ns3::OranCommand> c, const std::string& stage);
bool IsFinal(ns3::Ptr<ns3::OranCommand> c);
/** IMSI of the UE the proposal was generated for (-1 if the issuing LM did not record it). */
int64_t IntendedImsi(ns3::Ptr<ns3::OranCommand> c);
/** Lifecycle command id. */
uint64_t IdOf(ns3::Ptr<ns3::OranCommand> c);

/** Actuator side. */
void PowerApplied(ns3::Ptr<ns3::OranCommand> c, uint16_t cell, double requestedDbm, double oldDbm,
                  double newDbm, const std::string& status);
void PhyPowerObserved(uint16_t cell, double oldDbm, double newDbm);
void HoRequested(ns3::Ptr<ns3::OranCommand> c, int64_t imsi, uint16_t sourceCell,
                 uint16_t targetCell, uint16_t rnti);
void AttachHandoverTraces();

/**
 * Validity rule V1 (docs/methodology/command-semantics.md section 2). Returns "" if the
 * handover may be executed, otherwise the reason code. @p imsi receives the UE's IMSI if known.
 */
std::string CheckV1(ns3::Ptr<ns3::LteEnbNetDevice> source, uint16_t rnti, uint16_t targetCell,
                    int64_t intendedImsi, int64_t* imsi);

} // namespace cmdlife
} // namespace xapp

#endif // XAPP_CMD_LIFECYCLE_H
