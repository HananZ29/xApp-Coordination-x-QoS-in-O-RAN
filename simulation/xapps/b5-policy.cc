/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: B5 QoS-aware pair resolution (see header).
 */
#include "b5-policy.h"

#include <algorithm>
#include <cmath>

namespace xapp
{
namespace b5
{
namespace
{
/** gamma_ctrl of a UE towards cell s with per-cell power offsets (same formula as policy::GammaCtrl). */
double
Gamma(const std::map<uint16_t, double>& rsrp, uint16_t s, const std::map<uint16_t, double>& d, double nReDbm)
{
    auto off = [&](uint16_t c) {
        auto it = d.find(c);
        return it == d.end() ? 0.0 : it->second;
    };
    auto it = rsrp.find(s);
    if (it == rsrp.end())
    {
        return -1e9;
    }
    double den = std::pow(10.0, nReDbm / 10.0);
    for (const auto& [j, r] : rsrp)
    {
        if (j != s)
        {
            den += std::pow(10.0, (r + off(j)) / 10.0);
        }
    }
    return it->second + off(s) - 10.0 * std::log10(den);
}
} // namespace

Decision
Decide(const std::vector<UeInfo>& ues, bool unverifiableInvolved, const std::map<uint16_t, double>& rhoInit,
       const Pair& pair, const Params& p, const est::AmcLut& lut)
{
    Decision d;
    est::Input in;
    in.kappa = p.kappa;
    in.nReDbm = p.nReDbm;
    in.rhoInit = rhoInit;
    for (const auto& u : ues)
    {
        in.ues.push_back(u.ue);
    }
    std::vector<est::Candidate> cands(4);
    cands[0].name = "N";
    cands[1].name = "T";
    cands[1].moveImsi = pair.tsImsi;
    cands[1].moveTo = pair.dst;
    cands[2].name = "P";
    cands[2].deltaDb[pair.tpcCell] = pair.deltaDb;
    cands[3] = cands[1];
    cands[3].name = "B";
    cands[3].deltaDb[pair.tpcCell] = pair.deltaDb;

    auto epsOf = [&](const UeInfo& u, bool low, bool vul) {
        double e = -1;
        if (vul)
            e = std::max(e, p.epsVulnerable);
        if (low)
            e = std::max(e, p.epsLowDemand);
        if (u.i3b)
            e = std::max(e, p.epsAlreadyViolating);
        return e < 0 ? p.epsOther : e;
    };
    std::vector<est::Output> out;
    for (const auto& a : cands)
    {
        out.push_back(est::Predict(in, a, lut));
    }
    auto rate = [&](size_t k, uint64_t imsi) {
        for (const auto& r : out[k].ues)
            if (r.imsi == imsi)
                return r.rateMbps;
        return 0.0;
    };
    for (size_t k = 0; k < cands.size(); k++)
    {
        const auto& a = cands[k];
        CandEval c;
        c.name = a.name;
        // feasibility
        bool feas = true;
        std::string why;
        for (const auto& u : ues)
        {
            const bool isU = u.ue.imsi == pair.tsImsi;
            const uint16_t after = (a.moveImsi != 0 && isU) ? a.moveTo : u.ue.cell;
            const double g = Gamma(u.ue.rsrpDbm, after, a.deltaDb, p.nReDbm);
            if ((a.name == "T" || a.name == "B") && isU && g < p.qinDb)
            {
                feas = false;
                why = "c2_fail";
            }
            if ((a.name == "P" || a.name == "B") && after == pair.tpcCell && g < p.qinDb)
            {
                feas = false;
                why = why.empty() ? "c4_fail" : why;
            }
        }
        c.feasible = a.name == "N" || feas;
        // predicted violations, with the provisional all-user pessimism for a != N
        int v = 0;
        for (const auto& u : ues)
        {
            double r = rate(k, u.ue.imsi);
            bool low = u.ue.demandMbps < p.lowDemandMbps;
            bool vul = Gamma(u.ue.rsrpDbm, u.ue.cell, {}, p.nReDbm) < p.vulnerableDb;
            if (a.name != "N" && (low || vul))
            {
                r -= epsOf(u, low, vul);
            }
            v += r < p.alpha * u.ue.demandMbps;
        }
        c.vhat = v;
        auto lc = out[k].load.find(pair.src);
        c.cSrc = lc == out[k].load.end() ? 0.0 : lc->second;
        bool sTs = c.cSrc < 1.0;
        double dTs = std::max(0.0, c.cSrc - 1.0);
        bool sTpc = a.name == "P" || a.name == "B";
        c.J = p.zeta * (dTs / 2 + (sTpc ? 0.0 : 1.0) / 2) - std::pow(int(sTs) + int(sTpc), 2);
        if (!c.feasible)
            c.why = why;
        d.cands.push_back(c);
    }
    // safety (needs N's evaluation)
    for (size_t k = 0; k < d.cands.size(); k++)
    {
        auto& c = d.cands[k];
        if (c.name == "N")
        {
            c.safe = true;
            continue;
        }
        bool safe = c.vhat <= d.cands[0].vhat;
        if (!safe && c.why.empty())
            c.why = "sa_i_more_violations";
        for (const auto& u : ues)
        {
            if (u.i3b && rate(0, u.ue.imsi) - rate(k, u.ue.imsi) > p.epsAlreadyViolating)
            {
                if (safe && c.why.empty())
                    c.why = "sa_ii_already_violating";
                safe = false;
            }
        }
        if (unverifiableInvolved)
        {
            safe = false;
            c.why = "unverifiable_ue";
        }
        c.safe = safe;
    }
    static const std::map<std::string, int> pref{{"B", 0}, {"T", 1}, {"P", 2}, {"N", 3}};
    const CandEval* best = nullptr;
    for (const auto& c : d.cands)
    {
        if (!(c.feasible && c.safe))
            continue;
        if (!best || c.J < best->J - 1e-12 ||
            (std::fabs(c.J - best->J) <= 1e-12 &&
             (c.vhat < best->vhat || (c.vhat == best->vhat && pref.at(c.name) < pref.at(best->name)))))
        {
            best = &c;
        }
    }
    d.choice = best ? best->name : "N";
    d.reason = !best ? "b5_no_safe_candidate" : unverifiableInvolved ? "b5_unverifiable" : "b5_choice_" + d.choice;
    return d;
}

} // namespace b5
} // namespace xapp
