/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S4: TS and TPC decision logic (see header).
 */
#include "xapp-policy.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace xapp
{
namespace policy
{
namespace
{

std::string
F(double v, int p = 2)
{
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(p);
    s << v;
    return s.str();
}

/** The k newest reports are fresh, consecutive (each starts where the previous ended) and all
 *  start at or after notBefore. */
bool
Consecutive(const std::vector<LoadRep>& r, int k, double now, double maxAge, double notBefore)
{
    if (static_cast<int>(r.size()) < k || now - r[0].t > maxAge + 1e-9)
    {
        return false;
    }
    for (int i = 0; i < k; i++)
    {
        if (r[i].t0 < notBefore - 1e-9)
        {
            return false;
        }
        if (i + 1 < k && std::fabs(r[i].t0 - r[i + 1].t) > 1e-6)
        {
            return false;
        }
    }
    return true;
}

} // namespace

double
GammaCtrl(const UeView& u, uint16_t s, double nReDbm, uint16_t c, double deltaC)
{
    auto it = u.rsrpDbm.find(s);
    if (it == u.rsrpDbm.end())
    {
        return std::nan("");
    }
    double sig = it->second + (s == c ? deltaC : 0.0);
    double den = std::pow(10.0, nReDbm / 10.0);
    for (const auto& [j, r] : u.rsrpDbm)
    {
        if (j != s)
        {
            den += std::pow(10.0, (r + (j == c ? deltaC : 0.0)) / 10.0);
        }
    }
    return sig - 10.0 * std::log10(den);
}

bool
I3Flag(const SvcRep& r, double alpha)
{
    double d = r.demandMeanMbps > 0 ? r.demandMeanMbps : r.demandMbps;
    if (d <= 0)
    {
        return false; // no DL demand: nothing to violate
    }
    return r.servedMbps < alpha * d || r.harqLossShare > 1.0 - alpha;
}

// ---------------------------------------------------------------------------------------- TS
std::vector<TsProposal>
TsDecide(const Snapshot& s, const TsParams& p, TsState& st, std::vector<LogEntry>& log)
{
    const double now = s.now;
    // 1. per-UE state from the UE's own serving-cell reports (causal)
    for (const auto& u : s.ues)
    {
        TsUeState& us = st.ue[u.imsi];
        if (u.servingAge <= p.maxAgeS && u.servingCell != 0)
        {
            if (us.lastCell == 0)
            {
                us.lastCell = u.servingCell;
            }
            else if (u.servingCell != us.lastCell)
            {
                us.prevCell = us.lastCell;
                us.lastCell = u.servingCell;
                us.lastChange = now;
                if (us.pending)
                {
                    bool ok = u.servingCell == us.pendingTarget;
                    log.push_back({"ts", "imsi " + std::to_string(u.imsi), ok ? "ho_confirmed" : "ho_other_cell",
                                   "", "now in cell " + std::to_string(u.servingCell)});
                    us.pending = false;
                    us.failures = ok ? 0 : us.failures + 1;
                }
            }
        }
        if (us.pending && now - us.pendingSince > p.pendingS + 1e-9)
        {
            us.pending = false;
            us.failures++;
            log.push_back({"ts", "imsi " + std::to_string(u.imsi), "ho_unconfirmed", "pending_timeout",
                           "failures " + std::to_string(us.failures)});
        }
        if (us.failures >= p.maxFailures)
        {
            us.backoffUntil = now + p.backoffS;
            us.failures = 0;
            log.push_back({"ts", "imsi " + std::to_string(u.imsi), "backoff", "max_failures", ""});
        }
    }

    std::vector<TsProposal> out;
    for (const auto& [x, rx] : s.load)
    {
        const std::string ent = "cell " + std::to_string(x);
        if (rx.empty() || now - rx[0].t > p.maxAgeS + 1e-9)
        {
            log.push_back({"ts", ent, "no_action", "stale_input", "load report missing or older than max age"});
            continue;
        }
        bool congested = Consecutive(rx, p.kOn, now, p.maxAgeS, -1e9);
        for (int i = 0; congested && i < p.kOn; i++)
        {
            congested = rx[i].holMs >= p.holOnMs && rx[i].prb >= p.prbOn;
        }
        if (!congested)
        {
            continue; // the common case; not logged
        }
        auto lp = st.lastProposal.find(x);
        if (lp != st.lastProposal.end() && now - lp->second < p.cooldownS - 1e-9)
        {
            log.push_back({"ts", ent, "no_action", "source_cooldown", ""});
            continue;
        }
        std::vector<uint16_t> targets;
        for (const auto& [y, ry] : s.load)
        {
            if (y == x)
            {
                continue;
            }
            if (ry.empty() || now - ry[0].t > p.maxAgeS + 1e-9)
            {
                log.push_back({"ts", ent, "target_skipped", "stale_input", "cell " + std::to_string(y)});
                continue;
            }
            if (ry[0].prb < p.prbTargetMax && ry[0].holMs < p.holTargetMaxMs)
            {
                targets.push_back(y);
            }
            else
            {
                log.push_back({"ts", ent, "target_skipped", "no_capacity",
                               "cell " + std::to_string(y) + " prb " + F(ry[0].prb, 3) + " hol " + F(ry[0].holMs, 0)});
            }
        }
        if (targets.empty())
        {
            log.push_back({"ts", ent, "no_action", "no_eligible_target", ""});
            continue;
        }
        const UeView* best = nullptr;
        uint16_t bestY = 0;
        double bestG = -1e9;
        int nServed = 0, nC2 = 0;
        for (const auto& u : s.ues)
        {
            if (u.servingCell != x || u.servingAge > p.maxAgeS)
            {
                continue;
            }
            nServed++;
            const TsUeState& us = st.ue[u.imsi];
            std::string why;
            if (u.imsi == 0)
                why = "identity_unknown"; // no IMSI resolvable from reports: never steered (V1(f) needs it)
            else if (us.pending)
                why = "pending_handover";
            else if (now < us.backoffUntil)
                why = "backoff";
            else if (now - us.lastChange < p.dwellS - 1e-9)
                why = "dwell";
            if (!why.empty())
            {
                log.push_back({"ts", "imsi " + std::to_string(u.imsi), "candidate_skipped", why, ""});
                continue;
            }
            for (uint16_t y : targets)
            {
                if (us.prevCell == y && now - us.lastChange < p.returnBlockS - 1e-9)
                {
                    log.push_back({"ts", "imsi " + std::to_string(u.imsi), "candidate_skipped", "return_block",
                                   "cell " + std::to_string(y)});
                    continue;
                }
                auto ax = u.rsrpAge.find(x), ay = u.rsrpAge.find(y);
                if (ax == u.rsrpAge.end() || ay == u.rsrpAge.end() || ax->second > p.maxAgeS ||
                    ay->second > p.maxAgeS)
                {
                    log.push_back({"ts", "imsi " + std::to_string(u.imsi), "candidate_skipped", "stale_input",
                                   "rsrp of cell " + std::to_string(x) + " or " + std::to_string(y)});
                    continue;
                }
                double g = GammaCtrl(u, y, p.nReDbm);
                if (!(g >= p.qinDb))
                {
                    log.push_back({"ts", "imsi " + std::to_string(u.imsi), "candidate_skipped", "c2_fail",
                                   "gamma_target " + F(g)});
                    continue;
                }
                nC2++;
                if (!best || g > bestG + 1e-9 || (std::fabs(g - bestG) <= 1e-9 && u.imsi < best->imsi))
                {
                    best = &u;
                    bestY = y;
                    bestG = g;
                }
            }
        }
        if (!best)
        {
            log.push_back({"ts", ent, "no_action", "no_eligible_ue",
                           std::to_string(nServed) + " served, " + std::to_string(nC2) + " pass C2"});
            continue;
        }
        out.push_back({best->imsi, best->rnti, x, bestY, bestG});
        TsUeState& us = st.ue[best->imsi];
        us.pending = true;
        us.pendingTarget = bestY;
        us.pendingSince = now;
        st.lastProposal[x] = now;
        log.push_back({"ts", "imsi " + std::to_string(best->imsi), "propose_handover", "congested_source",
                       "cell " + std::to_string(x) + "->" + std::to_string(bestY) + " gamma_target " + F(bestG) +
                           " hol " + F(rx[0].holMs, 0) + " prb " + F(rx[0].prb, 3) + " load_age " +
                           F(now - rx[0].t, 3)});
    }
    return out;
}

// --------------------------------------------------------------------------------------- TPC
std::vector<TpcProposal>
TpcDecide(const Snapshot& s, const TpcParams& p, TpcState& st, std::vector<LogEntry>& log)
{
    const double now = s.now;
    std::vector<TpcProposal> out;
    for (const auto& [c, rc] : s.load)
    {
        const std::string ent = "cell " + std::to_string(c);
        TpcCellState& cs = st.cell[c];
        double power = p.pNomDbm;
        if (!rc.empty() && rc[0].txPowerDbm > -1e8)
        {
            power = rc[0].txPowerDbm; // reported by the eNB
        }
        else if (cs.commandedDbm != 0)
        {
            power = cs.commandedDbm;
        }
        const bool reduced = power < p.pNomDbm - 1e-6;
        const bool fresh = !rc.empty() && now - rc[0].t <= p.maxAgeS + 1e-9;

        // per-UE safety information for the UEs served by c
        int nUe = 0, nUnavail = 0, nQos = 0, nRadioNow = 0, nRadioAfter = 0;
        double minGammaNow = 1e9, minGammaAfter = 1e9;
        for (const auto& u : s.ues)
        {
            if (u.servingCell != c || u.servingAge > p.maxAgeS)
            {
                continue;
            }
            nUe++;
            bool svcOk = u.svc && u.svc->cell == c && now - u.svc->t <= p.maxAgeS + 1e-9;
            auto ac = u.rsrpAge.find(c);
            bool radioOk = ac != u.rsrpAge.end() && ac->second <= p.maxAgeS;
            if (!svcOk || !radioOk)
            {
                nUnavail++;
                continue;
            }
            if (I3Flag(*u.svc, p.alpha))
            {
                nQos++;
            }
            double g0 = GammaCtrl(u, c, p.nReDbm);
            double g1 = GammaCtrl(u, c, p.nReDbm, c, -p.stepDb);
            minGammaNow = std::min(minGammaNow, g0);
            minGammaAfter = std::min(minGammaAfter, g1);
            nRadioNow += g0 < p.qinDb;
            nRadioAfter += g1 < p.qinDb;
        }
        const std::string ues = std::to_string(nUe) + " UEs, unavailable " + std::to_string(nUnavail) +
                                ", qos_flag " + std::to_string(nQos) + ", min_gamma " + F(minGammaNow) +
                                ", min_gamma_after_step " + F(minGammaAfter) + ", power " + F(power, 1);

        // ---- restoration (takes precedence; not subject to the reduction dwell) ----
        if (reduced)
        {
            std::string why;
            if (!fresh)
                why = "stale_load_report";
            else if (nUnavail > 0)
                why = "ue_info_unavailable";
            else if (nQos > 0)
                why = "ue_qos_flag";
            else if (rc[0].holMs >= p.holHeadroomMs)
                why = "hol_headroom_lost";
            else if (rc[0].dropBytes > 0)
                why = "rlc_drops";
            else if (nRadioNow > 0)
                why = "c1_fail_at_current_power";
            if (!why.empty())
            {
                // act at most once per piece of evidence, and only on evidence generated after the
                // last change; a stale report or missing UE information restores immediately (fail-safe)
                bool evidenceNew = !fresh || nUnavail > 0 || (rc[0].t > cs.lastChange + 1e-9 && rc[0].t > cs.lastActedReport + 1e-9);
                if (evidenceNew && now - cs.lastChange > 1e-9)
                {
                    double tgt = std::min(p.pNomDbm, power + p.stepDb);
                    out.push_back({c, tgt, "restore", why});
                    cs.lastChange = now;
                    cs.lastActedReport = fresh ? rc[0].t : now;
                    cs.commandedDbm = tgt;
                    log.push_back({"tpc", ent, "propose_restore", why, ues + " -> " + F(tgt, 1)});
                }
                else
                {
                    log.push_back({"tpc", ent, "no_action", "restore_awaiting_new_evidence", why});
                }
                continue;
            }
        }
        // ---- reduction ----
        if (!fresh)
        {
            log.push_back({"tpc", ent, "no_action", "stale_input", ""});
            continue;
        }
        if (power - p.stepDb < p.pMinDbm - 1e-6)
        {
            continue; // at the floor: nothing to do
        }
        if (now - cs.lastChange < p.dwellS - 1e-9)
        {
            continue; // dwell after the last change
        }
        bool quiet = Consecutive(rc, p.kDown, now, p.maxAgeS, cs.lastChange);
        for (int i = 0; quiet && i < p.kDown; i++)
        {
            quiet = rc[i].prb < p.prbLow && rc[i].holMs < p.holHeadroomMs && rc[i].dropBytes == 0;
        }
        if (!quiet)
        {
            continue; // the common case at load; not logged
        }
        std::string why;
        if (nUnavail > 0)
            why = "ue_info_unavailable";
        else if (nQos > 0)
            why = "ue_qos_flag";
        else if (nRadioAfter > 0)
            why = "c4_fail";
        else if (rc[0].t <= cs.lastActedReport + 1e-9)
            why = "no_new_evidence";
        if (!why.empty())
        {
            log.push_back({"tpc", ent, "reduction_blocked", why, ues});
            continue;
        }
        double tgt = power - p.stepDb;
        out.push_back({c, tgt, "reduce", "low_load_headroom"});
        cs.lastChange = now;
        cs.lastActedReport = rc[0].t;
        cs.commandedDbm = tgt;
        log.push_back({"tpc", ent, "propose_reduce", "low_load_headroom",
                       ues + ", prb " + F(rc[0].prb, 3) + " -> " + F(tgt, 1)});
    }
    return out;
}

} // namespace policy
} // namespace xapp
