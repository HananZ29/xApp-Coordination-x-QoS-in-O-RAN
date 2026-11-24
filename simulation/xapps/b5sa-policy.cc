/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: state-aware B5 (see header). The per-scenario evaluation mirrors
 * b5::Decide line by line (KAT B5SA-K1 checks equality with b5::Decide on the same-cycle case).
 */
#include "b5sa-policy.h"

#include <algorithm>
#include <cmath>

namespace xapp
{
namespace b5sa
{
namespace
{
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

bool
Has(const std::string& a, char c)
{
    return a.find(c) != std::string::npos;
}
} // namespace

std::vector<b5::CandEval>
Evaluate(const Scenario& s, const Unit& unit, const b5::Params& p, const est::AmcLut& lut, std::vector<est::Output>* outs)
{
    est::Input in;
    in.kappa = p.kappa;
    in.nReDbm = p.nReDbm;
    in.rhoInit = s.rho;
    for (const auto& u : s.ues)
    {
        in.ues.push_back(u.ue);
    }
    // a candidate moves the TS UE iff it executes a TS PROPOSAL of the unit (T, B); it applies the step iff it
    // executes a TPC PROPOSAL of the unit (P, B). Earlier actions are already in the scenario's state.
    std::vector<est::Candidate> cands;
    for (const auto& n : unit.cands)
    {
        est::Candidate c;
        c.name = n;
        if (n == "T" || n == "B")
        {
            c.moveImsi = unit.tsImsi;
            c.moveTo = unit.dst;
        }
        if (n == "P" || n == "B")
        {
            c.deltaDb[unit.tpcCell] = s.tpcDeltaDb;
        }
        cands.push_back(c);
    }
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
    auto epsOf = [&](const b5::UeInfo& u, bool low, bool vul) {
        double e = -1;
        if (vul)
            e = std::max(e, p.epsVulnerable);
        if (low)
            e = std::max(e, p.epsLowDemand);
        if (u.i3b)
            e = std::max(e, p.epsAlreadyViolating);
        return e < 0 ? p.epsOther : e;
    };
    std::vector<b5::CandEval> ev;
    for (size_t k = 0; k < cands.size(); k++)
    {
        const auto& a = cands[k];
        b5::CandEval c;
        c.name = a.name;
        bool feas = true;
        std::string why;
        for (const auto& u : s.ues)
        {
            const bool isU = u.ue.imsi == unit.tsImsi;
            const uint16_t after = (a.moveImsi != 0 && isU) ? a.moveTo : u.ue.cell;
            const double g = Gamma(u.ue.rsrpDbm, after, a.deltaDb, p.nReDbm);
            if (Has("TB", a.name[0]) && isU && g < p.qinDb)
            {
                feas = false;
                why = "c2_fail";
            }
            if (Has("PB", a.name[0]) && after == unit.tpcCell && g < p.qinDb)
            {
                feas = false;
                why = why.empty() ? "c4_fail" : why;
            }
        }
        c.feasible = a.name == "N" || feas;
        int v = 0;
        for (const auto& u : s.ues)
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
        auto lc = out[k].load.find(unit.src);
        c.cSrc = lc == out[k].load.end() ? 0.0 : lc->second;
        bool sTs = c.cSrc < 1.0;
        double dTs = std::max(0.0, c.cSrc - 1.0);
        bool sTpc = unit.kind == "ts" ? true : Has("PB", a.name[0]);
        c.J = p.zeta * (dTs / 2 + (sTpc ? 0.0 : 1.0) / 2) - std::pow(int(sTs) + int(sTpc), 2);
        if (!c.feasible)
            c.why = why;
        ev.push_back(c);
    }
    for (size_t k = 0; k < ev.size(); k++)
    {
        auto& c = ev[k];
        if (c.name == "N")
        {
            c.safe = true;
            continue;
        }
        bool safe = c.vhat <= ev[0].vhat;
        if (!safe && c.why.empty())
            c.why = "sa_i_more_violations";
        for (const auto& u : s.ues)
        {
            if (u.i3b && rate(0, u.ue.imsi) - rate(k, u.ue.imsi) > p.epsAlreadyViolating)
            {
                if (safe && c.why.empty())
                    c.why = "sa_ii_already_violating";
                safe = false;
            }
        }
        c.safe = safe;
    }
    if (outs)
    {
        *outs = out;
    }
    return ev;
}

Decision
Decide(const std::vector<Scenario>& scen, const std::string& blockReason, const Unit& unit, const b5::Params& p,
       const est::AmcLut& lut)
{
    Decision d;
    for (const auto& s : scen)
    {
        std::vector<est::Output> outs;
        d.perScenario.push_back(Evaluate(s, unit, p, lut, &outs));
        for (size_t k = 0; k < unit.cands.size(); k++)
        {
            d.preds.push_back({s.name, unit.cands[k], outs[k].ues});
        }
    }
    for (size_t k = 0; k < unit.cands.size(); k++)
    {
        b5::CandEval c = d.perScenario[0][k]; // J, C_X from the believed scenario
        for (size_t j = 1; j < scen.size(); j++)
        {
            const auto& o = d.perScenario[j][k];
            if (!o.feasible && c.feasible)
                c.why = o.why + "@" + scen[j].name;
            if (o.feasible && !o.safe && c.safe && c.feasible)
                c.why = o.why + "@" + scen[j].name;
            c.feasible = c.feasible && o.feasible;
            c.safe = c.safe && o.safe;
            c.vhat = std::max(c.vhat, o.vhat);
        }
        if (c.name != "N" && !blockReason.empty())
        {
            c.safe = false;
            c.why = blockReason;
        }
        d.cands.push_back(c);
    }
    static const std::map<std::string, int> pref{{"B", 0}, {"T", 1}, {"P", 2}, {"N", 3}};
    const b5::CandEval* best = nullptr;
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
    d.reason = !blockReason.empty() ? "b5sa_" + blockReason : "b5sa_choice_" + d.choice;
    return d;
}

} // namespace b5sa
} // namespace xapp
