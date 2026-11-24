/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: STATE-AWARE B5 (B5sa), a separate policy variant (authorised
 * 2026-11-23, "S5 final coordination authorization" §3). The same-cycle B5 (b5-policy.h, b5::Decide) is unchanged.
 *
 * Trigger: every proposal flagged by the combined (state OR evidence) detector, coord-policy.h.
 * Decision unit and candidate set (only genuinely feasible present-time actions; earlier actions are state):
 *   pair  a same-cycle P1 pair (TS into Y + reduction of Y): the original four candidates N, T, P, B
 *   ts    a TS into Y flagged only by earlier / believed reductions of Y: {N (reject the TS), T (accept)}
 *   tpc   a reduction of Y flagged only by earlier TS into Y: {N (reject the reduction), P (accept)}
 * Never: cancellation, restoration, rollback, deferral, cooldown, persistent suppression or new commands.
 * Network state: the caller builds one or more Scenarios from RIC information only (B5 snapshot rules, RSRP
 *   re-referenced to the RIC-believed power of every cell, history of dispatched commands). Scenario 0 is the
 *   believed state (every dispatched command in effect). Uncertain items - a dispatched handover the RIC has not
 *   yet observed (pending), or a dispatched power command not yet confirmed by a power report - add scenarios in
 *   which that command has NOT taken effect.
 * Evaluation per scenario: exactly the B5 rules (estimator at frozen kappa 0.85, feasibility C2/C4 at Qin,
 *   provisional all-user pessimism with calibration epsilon, S-a' (i)/(ii), J = zeta(d_TS/2 + d_TPC/2) -
 *   (s_TS + s_TPC)^2 with C_X of the TS source X (for a tpc unit: the source of the earlier TS)). s_TPC = 1[a
 *   executes the unit's TPC proposal]; for a ts unit (no TPC proposal; the earlier reduction stays in every
 *   candidate) s_TPC = 1 for all candidates, which does not change the ranking.
 * Conservative handling of uncertain state: a candidate is feasible / safe only if it is feasible / safe in EVERY
 *   scenario; v_hat is the worst case; J is taken from the believed scenario. N is always feasible and safe.
 *   Unverifiable UE in an involved cell, or more than MaxUncertain uncertain items: only N (reason recorded).
 * Choice: argmin J over feasible & safe candidates of the unit; ties: fewer predicted violations, then B, T, P, N.
 */
#ifndef XAPP_B5SA_POLICY_H
#define XAPP_B5SA_POLICY_H

#include "b5-policy.h"

#include <string>
#include <vector>

namespace xapp
{
namespace b5sa
{

struct Scenario
{
    std::string name;                     //!< "believed" | e.g. "ho12_not_done" | "pwr2_not_applied"
    std::vector<b5::UeInfo> ues;          //!< verifiable UEs, in this scenario's cells / RSRP
    std::map<uint16_t, double> rho;       //!< initial busy fraction per cell
    double tpcDeltaDb{0};                 //!< unit's TPC proposal: requested - this scenario's current power
};

struct Unit
{
    std::string kind;                     //!< pair | ts | tpc
    uint64_t tsImsi{0};                   //!< TS of the unit (pair, ts) or the earlier TS (tpc)
    uint16_t src{0}, dst{0};              //!< its source and target
    uint16_t tpcCell{0};                  //!< TPC proposal's cell (pair, tpc); for ts: the reduced cell Y
    std::vector<std::string> cands;       //!< evaluated candidates (N first)
};

struct ScenarioPred
{
    std::string scenario, cand;
    std::vector<est::UeOut> ues;
};

struct Decision
{
    std::string choice;                   //!< one of unit.cands
    std::string reason;                   //!< b5sa_choice_<X> | b5sa_unverifiable | b5sa_uncertain_overflow
    std::vector<b5::CandEval> cands;      //!< aggregated over scenarios, in unit.cands order
    std::vector<std::vector<b5::CandEval>> perScenario;
    std::vector<ScenarioPred> preds;      //!< per scenario x candidate predicted rates (for offline error checks)
};

/** blockReason non-empty: only N is allowed (unverifiable UE involved / too many uncertain items). */
Decision Decide(const std::vector<Scenario>& scen, const std::string& blockReason, const Unit& unit,
                const b5::Params& p, const est::AmcLut& lut);

/** One scenario's evaluation of the unit's candidates (exposed for tests). */
std::vector<b5::CandEval> Evaluate(const Scenario& s, const Unit& unit, const b5::Params& p, const est::AmcLut& lut,
                                   std::vector<est::Output>* outs = nullptr);

} // namespace b5sa
} // namespace xapp

#endif
