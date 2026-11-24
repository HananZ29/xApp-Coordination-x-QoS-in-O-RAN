/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: B5 QoS-aware resolution of one flagged TS x TPC pair
 * (authorised 2026-11-23; S5-part2-report §7 decision sheet, approved items: Formulation A, all-user pessimism
 * as a PROVISIONAL conservative safeguard, epsilon from calibration seeds, kappa_eta = 0.85 frozen).
 *
 * Pure function; inputs are RIC-side only (verifiable UEs from the RIC store snapshot, the two proposals, static
 * configuration). Identical definitions to the offline evaluation (analysis/s5/estimator_eval.py, primary):
 *   candidates N (neither), T (TS only), P (TPC only), B (both); R_hat_i(a) from xapp::est::Predict.
 *   Feasibility A_f (gamma_ctrl at the candidate's powers, Qin): T: u at Y (current power); P: every UE now served
 *     by the TPC cell, after the step; B: u at Y after the step and every UE of Y after the step incl. u; N always.
 *   Predicted violation v_i(a) = 1[R' < alpha * D_i], R' = R_hat - eps_i for low-confidence UEs (low demand or
 *     vulnerable) when a != N (all such UEs: provisional conservative scope), else R_hat.
 *   Safe (S-a'): (i) sum v(a) <= sum v(N); (ii) every already-violating UE: R_hat(N) - R_hat(a) <= eps_av.
 *   Unverifiable UE in the TS source, TS target or TPC cell: every candidate other than N is excluded.
 *   J(a) = zeta (d_TS/2 + d_TPC/2) - (s_TS + s_TPC)^2, s_TS = 1[C_X(a) < 1], d_TS = max(0, C_X(a) - 1) with C_X the
 *     predicted RB-time demand of the TS source; s_TPC = 1[a in {P, B}], d_TPC = 1 - s_TPC.
 *   Choice: argmin J over A_f & safe; ties: fewer predicted violations, then B, T, P, N. N is always feasible and
 *     safe, so the set is never empty (the b5_no_safe_candidate rule is kept for completeness).
 * KNOWN LIMITATION (S5 part 2, held-out): with this pessimism scope the choice is N in 64/96 validation decisions
 * and agrees with the realised reference in 0.33 of them. Not an effective safeguard; provisional.
 */
#ifndef XAPP_B5_POLICY_H
#define XAPP_B5_POLICY_H

#include "rate-estimator.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xapp
{
namespace b5
{

struct Params
{
    double kappa{0.85};
    double alpha{0.95};
    double zeta{1000.0};
    double qinDb{-3.9};
    double nReDbm{-123.24};
    double lowDemandMbps{0.6};
    double vulnerableDb{0.0};
    double epsVulnerable{0.26606}, epsLowDemand{0.07604}, epsAlreadyViolating{0.31076}, epsOther{0.00683};
};

struct UeInfo
{
    est::UeIn ue;      //!< verifiable UE (serving cell, mean demand, RSRP per cell)
    bool i3b{false};   //!< already violating (I3b flag now)
};

struct Pair
{
    uint64_t tsImsi{0};
    uint16_t src{0}, dst{0};
    uint16_t tpcCell{0};
    double deltaDb{0}; //!< requested - current power of the TPC cell (< 0 for a reduction)
};

struct CandEval
{
    std::string name;
    bool feasible{false}, safe{false};
    int vhat{0};
    double cSrc{0}, J{0};
    std::string why; //!< infeasibility / unsafety reason
};

struct Decision
{
    std::string choice; //!< N | T | P | B
    std::vector<CandEval> cands;
    std::string reason; //!< b5_choice_<X> | b5_unverifiable
};

Decision Decide(const std::vector<UeInfo>& ues, bool unverifiableInvolved, const std::map<uint16_t, double>& rhoInit,
                const Pair& pair, const Params& p, const est::AmcLut& lut);

} // namespace b5
} // namespace xapp

#endif
