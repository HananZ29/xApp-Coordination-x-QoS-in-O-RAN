/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S4: RIC-side telemetry store.
 *
 * Filled ONLY by the RIC report sink (OranQueryTriggerTelemetrySink) when an E2 report arrives at the
 * RIC, so every xApp decision at t_q uses information that had arrived by t_q. Holds the latest eNB load
 * reports per cell (history of 8), the latest per-UE service report per IMSI, and per UE E2 node the
 * latest RSRP per measured cell. UE identity (IMSI) is resolved only from reports: the UE's serving
 * (cell, RNTI) is matched to an eNB per-UE service report carrying (cell, RNTI, IMSI).
 * Also writes the xApp decision log <out>-xappdec.csv.
 */
#ifndef XAPP_RIC_TELEMETRY_STORE_H
#define XAPP_RIC_TELEMETRY_STORE_H

#include "xapp-policy.h"

#include "ns3/oran-report.h"
#include "ns3/ptr.h"

#include <string>
#include <utility>
#include <vector>

namespace xapp
{
namespace ric
{

void OnReport(ns3::Ptr<ns3::OranReport> report);
policy::Snapshot BuildSnapshot(double now);

/** S5 protocol correction C-A (approved 2026-11-23): controller activation.
 *  T_A = traffic start + warm-up. A WINDOWED report (eNB load report, per-UE service report, and every term of
 *  the I3b demand mean) is admissible to the controllers only if its measurement interval starts at or after T_A
 *  (t0 >= T_A). Inadmissible reports are omitted from the Snapshot; RSRP (an L3-filtered level of the
 *  load-independent reference signal, not a windowed load measurement) is not filtered. Report timing, ages and
 *  the store contents are unchanged: the rule only withholds old windows, it never provides fresher data.
 *  Default -inf = no filtering (pre-C-A behaviour, byte-identical). */
void SetAdmissionStart(double tA);
double AdmissionStart();
/** Number of admissible windowed reports in the store at this instant (C-A: a controller is active only if > 0). */
int AdmissibleReports();

/** S5 shadow interaction diagnostic: RIC-side record of the evidence a proposal was computed from
 *  (earliest measurement time among the inputs the issuing xApp used), keyed by lifecycle command id. */
void RegisterEvidence(uint64_t cmdId, double evidenceStartS);
/** NaN if not registered. */
double EvidenceStart(uint64_t cmdId);
/** Latest per-UE service report of this IMSI as known at the RIC: (cell, report time); (0, -inf) if none. */
std::pair<uint16_t, double> LatestServiceObservation(uint64_t imsi);
/** Report time of the newest load report of the cell (-inf if none). */
double LatestLoadReportTime(uint16_t cell);
/** Cell DL power as last reported by its eNB to the RIC (NaN if never reported). */
double LatestTxPowerDbm(uint16_t cell);

void OpenDecisionLog(const std::string& outPrefix);
void LogDecisions(double now, const std::vector<policy::LogEntry>& entries);
void CloseDecisionLog();

} // namespace ric
} // namespace xapp

#endif
