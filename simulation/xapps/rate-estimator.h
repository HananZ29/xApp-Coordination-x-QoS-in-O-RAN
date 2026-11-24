/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: telemetry-only per-UE rate estimator for B5 (specification:
 * docs/progress-reports/S5-part1-report.md §6; calibration / held-out validation harness approved 2026-11-23).
 * B5 candidate selection / arbitration is NOT implemented here (not authorised): this module only predicts.
 *
 * Inputs are RIC-side only (Snapshot from the RIC telemetry store): per-UE RSRP of each cell, per-UE mean demand
 * (I3b quantity, last 4 service reports), and the cells' last reported PRB utilisation (initial busy fraction).
 * Static configuration: noise per RE, schedulable RBs, and ns-3's own link adaptation (LteAmc, MiErrorModel
 * "Vienna" model, the default used by the simulated UEs and the PF scheduler), tabulated once (AmcLut).
 *
 * Prediction R_i(a) for a candidate a (move of one UE and/or per-cell power offsets):
 *   1  RSRP_ij(a) = RSRP_ij + dP_j(a)  (dB for dB)
 *   2  data SINR  g_i = S_i / (sum_{j != s_i(a)} rho_j * I_ij + N)   (S, I from RSRP per RE; N = noise per RE)
 *   3  eta_i = kappa * LUT(g_i)  payload bytes per RB per TTI (SINR -> CQI -> MCS -> TB size, ns-3 LteAmc)
 *   4  c_i = nRbSched * eta_i * 1000 * 8 / 1e6 Mbps (whole cell to UE i); RB-time demand r_i = D_i / c_i
 *   5  per cell: L = sum r_i; if L <= 1 tau_i = r_i, else PF water-filling of equal time shares
 *      (tau_i = min(r_i, theta), sum tau_i = 1); rho_j = min(1, L_j)
 *   6  2-5 iterated from rho = reported PRB utilisation until max |d rho| < tol (at most maxIter iterations)
 *   7  R_i = min(D_i, tau_i * c_i)
 * One pre-specified calibration freedom: the scalar kappa (fitted on calibration seeds 301-303 only).
 */
#ifndef XAPP_RATE_ESTIMATOR_H
#define XAPP_RATE_ESTIMATOR_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xapp
{
namespace est
{

/** SINR (dB) -> payload bytes per RB per TTI, as a step function on a uniform grid (floor lookup, so the
 *  table never credits a higher MCS than the SINR supports at its grid point). */
struct AmcLut
{
    double gMinDb{-10.0};
    double stepDb{0.1};
    std::vector<double> bytesPerRb;
    std::vector<int> cqi, mcs;
    double Eta(double gDb) const;
};

/** Tabulates ns-3 LteAmc (default attributes: MiErrorModel) for a flat SINR over nRbCfg RBs, RBG size of the PF
 *  scheduler, TB size for nRbSched RBs, scaled to application payload by payloadFraction. */
AmcLut BuildAmcLut(int nRbCfg = 25, int nRbSched = 24, double payloadFraction = 1024.0 / 1054.0,
                   double gMinDb = -10.0, double gMaxDb = 35.0, double stepDb = 0.1);

struct UeIn
{
    uint64_t imsi{0};
    uint16_t cell{0};                    //!< serving cell now
    double demandMbps{0};                //!< mean demand (I3b quantity)
    std::map<uint16_t, double> rsrpDbm;  //!< reported RSRP per cell (current powers)
};

struct Input
{
    std::vector<UeIn> ues;                //!< verifiable UEs only (the caller logs the others)
    std::map<uint16_t, double> rhoInit;   //!< last reported prb_util_sched per cell
    double nReDbm{-123.24};
    int nRbSched{24};
    double kappa{1.0};
    int maxIter{5};
    double tol{0.01};
};

struct Candidate
{
    std::string name;                 //!< N | T | P | B
    uint64_t moveImsi{0};             //!< 0: no move
    uint16_t moveTo{0};
    std::map<uint16_t, double> deltaDb; //!< per-cell power change
};

struct UeOut
{
    uint64_t imsi{0};
    uint16_t cellAfter{0};
    double gammaDb{0}, etaBytes{0}, capMbps{0}, rbTime{0}, tau{0}, rateMbps{0};
};

struct Output
{
    std::vector<UeOut> ues;
    std::map<uint16_t, double> rho;  //!< predicted busy fraction
    std::map<uint16_t, double> load; //!< predicted RB-time demand sum r_i (C_X of the TS KPI)
    int iterations{0};
    bool converged{false};
};

Output Predict(const Input& in, const Candidate& a, const AmcLut& lut);

} // namespace est
} // namespace xapp

#endif
