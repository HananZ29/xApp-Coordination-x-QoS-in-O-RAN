/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S4: decision logic of the independent TS and TPC xApps.
 *
 * Pure functions over a Snapshot of telemetry that had ARRIVED AT THE RIC by the decision time t_q
 * (built by ric-telemetry-store from E2 reports only). No simulator state, no receiver outcomes, no
 * future information. Specification: docs/progress-reports/S4-specification-v2.md (approved for
 * implementation 2026-11-22); parameters are configurable and recorded in the run configuration.
 *
 * Radio quantity (ns-3.42, verified): control-channel SINR from RSRP of every reported cell plus noise
 *   gamma_ctrl(u, s) = RSRP_s - 10 log10( sum_{j != s} 10^(RSRP_j/10) + 10^(N_RE/10) )   [dB]
 * Qin = -3.9 dB is ns-3's in-sync threshold (LteUePhy::Qin). It is NOT a guarantee of application QoS
 * nor of immunity from radio-link failure.
 */
#ifndef XAPP_POLICY_H
#define XAPP_POLICY_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xapp
{
namespace policy
{

/** One eNB load report as seen by the RIC. */
struct LoadRep
{
    double t{0};   //!< report (generation) time, s
    double t0{0};  //!< interval start, s
    double prb{0}; //!< prb_util_sched over (t0, t]
    double holMs{0};       //!< max DRB head-of-line delay at t
    uint64_t dropBytes{0}; //!< RLC drops in the interval
    double txPowerDbm{-1e9}; //!< cell transmit power reported by the eNB (< -1e8: not reported)
};

/** Per-UE DL service report (eNB-side). */
struct SvcRep
{
    double t{0}, t0{0};
    uint16_t cell{0}, rnti{0};
    double servedMbps{0}, demandMbps{0}; //!< payload-scaled rates
    double demandMeanMbps{0};            //!< mean demand over the UE's last 4 reports in this cell (I3b)
    double harqLossShare{0};             //!< harqlost / (acked + harqlost)
    uint32_t holMs{0};
};

/** One UE as visible at the RIC. */
struct UeView
{
    uint64_t imsi{0};
    uint16_t servingCell{0}; //!< from the UE's own serving-cell RSRP entry
    uint16_t rnti{0};
    double servingAge{1e9};  //!< age (s) of the serving-cell information
    std::map<uint16_t, double> rsrpDbm; //!< cell -> latest RSRP (dBm)
    std::map<uint16_t, double> rsrpAge; //!< cell -> age (s)
    std::optional<SvcRep> svc;          //!< latest per-UE service report (any cell)
};

struct Snapshot
{
    double now{0};
    std::map<uint16_t, std::vector<LoadRep>> load; //!< cell -> reports, newest first
    std::vector<UeView> ues;
};

/** gamma_ctrl of u towards cell s, with cell c's RSRP shifted by deltaC dB (power change). */
double GammaCtrl(const UeView& u, uint16_t s, double nReDbm, uint16_t c = 0, double deltaC = 0);

/** Decision-log entry (written by the LM wrapper). */
struct LogEntry
{
    std::string xapp, entity, decision, reason, detail;
};

// ------------------------------------------------------------------ TS
struct TsParams
{
    double holOnMs{78}, prbOn{0.95}; //!< congestion: HOL_end >= holOn AND prb >= prbOn
    int kOn{2};                      //!< consecutive congested reports
    double prbTargetMax{0.959}, holTargetMaxMs{9.6}; //!< target capacity eligibility (strict <)
    double qinDb{-3.9}, nReDbm{-123.24};             //!< C2 threshold and noise per RE
    double dwellS{5}, returnBlockS{10}, cooldownS{1};
    double maxAgeS{1.0};     //!< inputs older than this are not used
    double pendingS{1.0};    //!< pending-handover confirmation timeout (2 x T_control)
    int maxFailures{2};      //!< failed / unconfirmed proposals before back-off
    double backoffS{10};
};

struct TsUeState
{
    uint16_t lastCell{0};
    double lastChange{-1e9};
    uint16_t prevCell{0};
    bool pending{false};
    uint16_t pendingTarget{0};
    double pendingSince{0};
    int failures{0};
    double backoffUntil{-1e9};
};

struct TsState
{
    std::map<uint64_t, TsUeState> ue;
    std::map<uint16_t, double> lastProposal; //!< source cell -> time
};

struct TsProposal
{
    uint64_t imsi{0};
    uint16_t rnti{0}, source{0}, target{0};
    double gammaTargetDb{0};
};

std::vector<TsProposal> TsDecide(const Snapshot& s, const TsParams& p, TsState& st,
                                 std::vector<LogEntry>& log);

// ------------------------------------------------------------------ TPC
struct TpcParams
{
    double pNomDbm{30}, pMinDbm{24}, stepDb{3};
    double prbLow{0.5};             //!< low-load condition for a reduction (calibration item)
    double holHeadroomMs{9.6};      //!< headroom: HOL < this and no RLC drops
    int kDown{4};                   //!< consecutive quiet reports (after the last change)
    double dwellS{2};               //!< minimum time between a change and an ordinary reduction
    double alpha{0.95};             //!< per-UE QoS target for the I3 indicator
    double qinDb{-3.9}, nReDbm{-123.24};
    double maxAgeS{1.0};
};

struct TpcCellState
{
    double lastChange{-1e9};
    double lastActedReport{-1e9}; //!< report time that justified the last command
    double commandedDbm{0};       //!< 0 = unknown (use the reported or nominal power)
};

struct TpcState
{
    std::map<uint16_t, TpcCellState> cell;
};

struct TpcProposal
{
    uint16_t cell{0};
    double targetDbm{0};
    std::string kind; //!< "reduce" | "restore"
    std::string reason;
};

/** Observable per-UE QoS violation at alpha (indicator I3b, validated on held-out seeds):
 *  served(k) < alpha * mean demand (last 4 reports, same cell)  OR  HARQ-loss share > 1 - alpha.
 *  Falls back to the window demand if no mean is available. */
bool I3Flag(const SvcRep& r, double alpha);

std::vector<TpcProposal> TpcDecide(const Snapshot& s, const TpcParams& p, TpcState& st,
                                   std::vector<LogEntry>& log);

} // namespace policy
} // namespace xapp

#endif
