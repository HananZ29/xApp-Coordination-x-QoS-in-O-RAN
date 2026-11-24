/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: conflict detector and B3 / B4 resolution (pure functions).
 *
 * Input: the proposals the CMM received in ONE RIC cycle (all logic modules), each described by information
 * available at the RIC at t_c: command type, intended IMSI (TS), source/target cell, requested power (TPC) and the
 * cell's power as last reported by its eNB (RIC telemetry store; nominal if never reported).
 *
 * Detector (approved 2026-11-23): a POTENTIAL conflict exists when a TS proposal moves a UE into cell Y and a TPC
 * proposal REDUCES cell Y's power in the same coordination cycle (pattern P1). Every TS x TPC pair of the cycle
 * is classified and logged; only P1 is flagged. Other classes (P2: TPC on the TS source cell; P3: TPC increase on
 * the TS target) are logged as undetected possible interactions. Cross-cycle and indirect interactions (e.g. a
 * TPC change in a later cycle, interference through neighbour activity) are outside the detector's scope.
 * A flag means "potential conflict", not observed negative interaction and not QoS harm.
 *
 * Resolution modes:
 *   B3        pass through every proposal; list order = all TS by ascending intended IMSI, then all TPC by
 *             ascending cell (deterministic). reverse=true gives all TPC first (pre-registered sensitivity).
 *   B4_TS     as B3, but for every flagged pair the TPC proposal is rejected (reason b4_priority_ts)
 *   B4_TPC    as B3, but for every flagged pair the TS proposal is rejected (reason b4_priority_tpc)
 * Rejected proposals are not deferred or rescheduled. Validity (V1/V2) is evaluated later, at execution.
 */
#ifndef XAPP_COORD_POLICY_H
#define XAPP_COORD_POLICY_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xapp
{
namespace coord
{

struct Prop
{
    enum Type
    {
        TS,
        TPC,
        OTHER
    } type{OTHER};
    uint64_t id{0};         //!< command id (S3 lifecycle)
    uint64_t imsi{0};       //!< TS: intended UE
    uint16_t src{0}, dst{0}; //!< TS: source and target cell
    uint16_t cell{0};       //!< TPC: cell
    double requestedDbm{0}; //!< TPC
    double currentDbm{0};   //!< TPC: last reported power of the cell (nominal if unknown)
};

struct PairClass
{
    size_t ts{0}, tpc{0}; //!< indices into the input vector
    std::string pattern;  //!< P1 | P2 | P3 | none
    bool flagged{false};  //!< P1 only
};

/** All TS x TPC pairs of the cycle, classified; output sorted (TS imsi, TPC cell) so it does not depend on input order. */
std::vector<PairClass> Detect(const std::vector<Prop>& props);

enum class Mode
{
    B3,
    B4_TS,
    B4_TPC
};

struct Resolution
{
    std::vector<size_t> dispatch;                          //!< indices, in dispatch order
    std::vector<std::pair<size_t, std::string>> rejected;  //!< index, reason
    std::vector<PairClass> pairs;
};

Resolution Resolve(const std::vector<Prop>& props, Mode mode, bool reverseOrder);

// ------------------------------------------------------------------------------------------------------------
// S5 SHADOW interaction diagnostic (authorised 2026-11-23): labels only. Its output is logged and never used by
// Resolve(); B3 / B4 behaviour is unchanged. All inputs are RIC-side: the RIC's own record of the commands it
// dispatched (H), eNB power reports, per-UE service reports, and each proposal's evidence start (the earliest
// measurement time among the inputs the issuing xApp used, registered by the xApp).
//
// Candidate definitions, for a proposal p at decision time t (only TS proposals into Y and TPC REDUCTIONS of Y
// are labelled; the counterpart q is a TPC reduction of Y (for TS p) or a TS into Y (for TPC p), taken from the
// same cycle (t_q = t) or from H (t_q = dispatch time)):
//   same_cycle   q in the same cycle (= the approved detector, P1)
//   win_W        t - t_q <= W  (fixed temporal window; W in Params.windows)
//   state        TS p: the RIC-believed power of Y is below nominal (a dispatched reduction not yet superseded,
//                      or a same-cycle reduction)
//                TPC p: a handover into Y is pending (dispatched, not yet observed by the RIC as served by Y,
//                      and younger than the pending timeout), or a same-cycle TS into Y
//   evidence     t_q > E_start(p): q was dispatched after the earliest measurement p was computed from, so p's
//                evidence does not reflect q (includes same-cycle q, since E_start <= t)
//   combined     state OR evidence (proposed definition)
// ------------------------------------------------------------------------------------------------------------
struct HistCmd
{
    Prop::Type type{Prop::OTHER};
    uint64_t id{0};
    double tDisp{0};
    uint64_t imsi{0};
    uint16_t src{0}, dst{0}, cell{0};
    double requestedDbm{0};
    double believedBeforeDbm{0}; //!< TPC: RIC-believed power of the cell just before this command
};

/** RIC belief of a cell's power: the last dispatched command's value until a power report generated after
 *  that dispatch arrives (then the report, which also reveals a rejected/clamped command); nominal if unknown. */
double BelievedPower(const std::vector<HistCmd>& h, uint16_t cell, double reportedDbm, double reportT,
                     double nominalDbm);

/** RIC view of a dispatched handover: pending until the RIC receives a service report of the UE from the
 *  target generated after the dispatch, or until the pending timeout expires (then: unconfirmed, not pending). */
bool HoPending(const HistCmd& h, uint16_t obsCell, double obsT, double now, double pendingS);

struct LabelInput
{
    double now{0};
    std::vector<Prop> props;
    std::vector<double> evidenceStart;       //!< per prop (NaN: unknown -> treated as now)
    std::vector<HistCmd> history;            //!< dispatched before now
    std::vector<bool> hoPending;             //!< per history entry (TS only)
    std::vector<std::pair<uint16_t, double>> believedDbm; //!< (cell, RIC-believed power before this cycle)
    double nominalDbm{30};
    std::vector<double> windows{1.0, 2.0, 5.0};
};

struct Label
{
    size_t prop{0};
    std::string def;
    bool flagged{false};
    int64_t relatedId{-1};    //!< most recent counterpart that satisfies the definition (-1: none)
    double relatedT{0};
    std::string basis;
    bool tsFirstWouldReject{false};  //!< a B4-TS-first policy with this definition would reject p
    bool tpcFirstWouldReject{false}; //!< a B4-TPC-first policy with this definition would reject p
    // S5 state-aware policies (authorised 2026-11-23): the counterparts behind the flag, split by origin
    bool sameCycleCp{false};         //!< a same-cycle counterpart satisfies the definition
    int64_t histId{-1};              //!< most recent DISPATCHED counterpart (in H) that satisfies it (-1: none)
    double histT{0};                 //!< its dispatch time
};

/** Labels for every TS proposal and every TPC reduction of the cycle, one per definition; ordered by proposal
 *  index then definition. Pure; independent of the order of history entries with distinct dispatch times. */
std::vector<Label> LabelInteractions(const LabelInput& in);

// ------------------------------------------------------------------------------------------------------------
// S5 STATE-AWARE B4 (Hanan, 2026-11-23: decisions 1 and 2 approved). A separate policy variant; Resolve() and
// the same-cycle B3 / B4 are unchanged.
//   Detector: the labeller above with one definition: "combined" (state OR evidence; primary) or "win_2" /
//   "win_5" (registered sensitivity comparators). Callers pass TPC proposals with currentDbm = the RIC-believed
//   power (BelievedPower), so a reduction is judged against the believed state, not a stale report.
//   Feasible set: subsets of the CURRENT proposals. Dispatched / pending commands are state: never cancelled,
//   restored, rolled back or deferred. Hence only the later (current) proposal can be rejected:
//     TS-first  rejects every flagged TPC reduction (same-cycle pair as B4-TS; cross-cycle: a reduction of Y
//               after a TS into Y). A flagged TS passes; if its counterpart is a reduction already dispatched
//               (or Y is believed reduced) priority cannot protect it -> note priority_unenforceable.
//     TPC-first rejects every flagged TS (same-cycle pair as B4-TPC; cross-cycle: a TS into a reduced Y). A
//               flagged reduction passes; if its counterpart TS was already dispatched -> priority_unenforceable.
// ------------------------------------------------------------------------------------------------------------
struct SaDecision
{
    size_t prop{0};
    Label label;          //!< the label under the policy's definition
    bool reject{false};
    std::string reason;   //!< b4sa_priority_ts | b4sa_priority_tpc (reject only)
    std::string note;     //!< priority_unenforceable | priority_unenforceable_state | counterpart_rejected_same_cycle
};

/** One entry per labelled proposal (TS, TPC reduction), in proposal order. def: combined | win_2 | win_5 | ... */
std::vector<SaDecision> ResolveStateAwareB4(const LabelInput& in, const std::string& def, bool tsFirst);

} // namespace coord
} // namespace xapp

#endif
