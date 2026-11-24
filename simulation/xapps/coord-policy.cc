/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: conflict detector and B3 / B4 resolution (see header).
 */
#include "coord-policy.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace xapp
{
namespace coord
{
namespace
{
std::vector<size_t>
Sorted(const std::vector<Prop>& p, Prop::Type t)
{
    std::vector<size_t> v;
    for (size_t i = 0; i < p.size(); i++)
    {
        if (p[i].type == t)
        {
            v.push_back(i);
        }
    }
    std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {
        if (t == Prop::TS)
        {
            return p[a].imsi != p[b].imsi ? p[a].imsi < p[b].imsi : p[a].src < p[b].src;
        }
        return p[a].cell != p[b].cell ? p[a].cell < p[b].cell : p[a].requestedDbm < p[b].requestedDbm;
    });
    return v;
}
} // namespace

std::vector<PairClass>
Detect(const std::vector<Prop>& props)
{
    std::vector<PairClass> out;
    for (size_t i : Sorted(props, Prop::TS))
    {
        for (size_t j : Sorted(props, Prop::TPC))
        {
            PairClass c;
            c.ts = i;
            c.tpc = j;
            const Prop& ts = props[i];
            const Prop& tp = props[j];
            if (tp.cell == ts.dst && tp.requestedDbm < tp.currentDbm - 1e-9)
            {
                c.pattern = "P1";
                c.flagged = true;
            }
            else if (tp.cell == ts.src)
            {
                c.pattern = "P2";
            }
            else if (tp.cell == ts.dst)
            {
                c.pattern = "P3";
            }
            else
            {
                c.pattern = "none";
            }
            out.push_back(c);
        }
    }
    return out;
}

Resolution
Resolve(const std::vector<Prop>& props, Mode mode, bool reverseOrder)
{
    Resolution r;
    r.pairs = Detect(props);
    std::set<size_t> drop;
    for (const auto& c : r.pairs)
    {
        if (!c.flagged)
        {
            continue;
        }
        if (mode == Mode::B4_TS && !drop.count(c.tpc))
        {
            drop.insert(c.tpc);
            r.rejected.emplace_back(c.tpc, "b4_priority_ts");
        }
        else if (mode == Mode::B4_TPC && !drop.count(c.ts))
        {
            drop.insert(c.ts);
            r.rejected.emplace_back(c.ts, "b4_priority_tpc");
        }
    }
    std::vector<size_t> ts = Sorted(props, Prop::TS), tpc = Sorted(props, Prop::TPC), other = Sorted(props, Prop::OTHER);
    std::vector<size_t> order;
    if (!reverseOrder)
    {
        order = ts;
        order.insert(order.end(), tpc.begin(), tpc.end());
    }
    else
    {
        order = tpc;
        order.insert(order.end(), ts.begin(), ts.end());
    }
    order.insert(order.end(), other.begin(), other.end());
    for (size_t i : order)
    {
        if (!drop.count(i))
        {
            r.dispatch.push_back(i);
        }
    }
    return r;
}

double
BelievedPower(const std::vector<HistCmd>& h, uint16_t cell, double reportedDbm, double reportT, double nominalDbm)
{
    const HistCmd* last = nullptr;
    for (const auto& c : h)
    {
        if (c.type == Prop::TPC && c.cell == cell && (!last || c.tDisp >= last->tDisp))
        {
            last = &c;
        }
    }
    bool haveReport = !std::isnan(reportedDbm);
    if (last && !(haveReport && reportT > last->tDisp + 1e-9))
    {
        return last->requestedDbm;
    }
    return haveReport ? reportedDbm : nominalDbm;
}

bool
HoPending(const HistCmd& h, uint16_t obsCell, double obsT, double now, double pendingS)
{
    if (h.type != Prop::TS)
    {
        return false;
    }
    bool observed = obsCell == h.dst && obsT > h.tDisp + 1e-9;
    return !observed && now - h.tDisp <= pendingS + 1e-9;
}

std::vector<Label>
LabelInteractions(const LabelInput& in)
{
    std::vector<Label> out;
    auto believed = [&](uint16_t c) {
        for (const auto& [cell, v] : in.believedDbm)
        {
            if (cell == c)
            {
                return v;
            }
        }
        return in.nominalDbm;
    };
    struct Cp
    {
        int64_t id;
        double t;
        bool sameCycle;
        bool pending; // TS counterpart: handover pending at the RIC
    };
    for (size_t i = 0; i < in.props.size(); i++)
    {
        const Prop& p = in.props[i];
        const bool isTs = p.type == Prop::TS;
        const bool isRed = p.type == Prop::TPC && p.requestedDbm < p.currentDbm - 1e-9;
        if (!isTs && !isRed)
        {
            continue;
        }
        const uint16_t y = isTs ? p.dst : p.cell;
        double ev = i < in.evidenceStart.size() && !std::isnan(in.evidenceStart[i]) ? in.evidenceStart[i] : in.now;
        // counterparts
        std::vector<Cp> cps;
        for (size_t j = 0; j < in.props.size(); j++)
        {
            const Prop& q = in.props[j];
            if (isTs && q.type == Prop::TPC && q.cell == y && q.requestedDbm < q.currentDbm - 1e-9)
                cps.push_back({static_cast<int64_t>(q.id), in.now, true, false});
            if (isRed && q.type == Prop::TS && q.dst == y)
                cps.push_back({static_cast<int64_t>(q.id), in.now, true, true});
        }
        for (size_t k = 0; k < in.history.size(); k++)
        {
            const HistCmd& h = in.history[k];
            if (isTs && h.type == Prop::TPC && h.cell == y && h.requestedDbm < h.believedBeforeDbm - 1e-9)
                cps.push_back({static_cast<int64_t>(h.id), h.tDisp, false, false});
            if (isRed && h.type == Prop::TS && h.dst == y)
                cps.push_back({static_cast<int64_t>(h.id), h.tDisp, false, k < in.hoPending.size() && in.hoPending[k]});
        }
        // most recent first (ties: same-cycle first, then lower id) -> deterministic
        std::sort(cps.begin(), cps.end(), [](const Cp& a, const Cp& b) {
            if (a.t != b.t)
                return a.t > b.t;
            if (a.sameCycle != b.sameCycle)
                return a.sameCycle;
            return a.id < b.id;
        });
        auto first = [&](auto pred) -> const Cp* {
            for (const auto& c : cps)
                if (pred(c))
                    return &c;
            return nullptr;
        };
        auto hist = [&](auto pred) -> const Cp* {
            for (const auto& c : cps)
                if (!c.sameCycle && pred(c))
                    return &c;
            return nullptr;
        };
        const bool anySc = first([](const Cp& c) { return c.sameCycle; }) != nullptr;
        // history counterpart per definition (most recent first); every definition counts a same-cycle counterpart
        auto withHist = [&](const Cp* h) {
            out.back().sameCycleCp = out.back().flagged && anySc;
            if (h)
            {
                out.back().histId = h->id;
                out.back().histT = h->t;
            }
        };
        auto emit = [&](const std::string& def, const Cp* c, bool flagged, const std::string& basis) {
            Label l;
            l.prop = i;
            l.def = def;
            l.flagged = flagged;
            if (c)
            {
                l.relatedId = c->id;
                l.relatedT = c->t;
            }
            l.basis = basis;
            l.tsFirstWouldReject = flagged && !isTs;
            l.tpcFirstWouldReject = flagged && isTs;
            out.push_back(l);
        };
        const Cp* sc = first([](const Cp& c) { return c.sameCycle; });
        emit("same_cycle", sc, sc != nullptr, "");
        withHist(nullptr);
        for (double w : in.windows)
        {
            const Cp* c = first([&](const Cp& c) { return in.now - c.t <= w + 1e-9; });
            std::ostringstream n;
            n << "win_" << w;
            emit(n.str(), c, c != nullptr, "");
            withHist(hist([&](const Cp& c) { return in.now - c.t <= w + 1e-9; }));
        }
        bool tsReduced = false; // TS p: Y believed reduced (state)
        const Cp* st = nullptr;
        std::string stBasis;
        if (isTs)
        {
            double b = believed(y);
            if (sc)
            {
                st = sc;
                stBasis = "same-cycle reduction";
            }
            else if (b < in.nominalDbm - 1e-6)
            {
                st = first([](const Cp&) { return true; }); // the reduction that set the state (may be absent
                                                              // if the state is known only from a report)
                stBasis = "believed power " + std::to_string(b);
            }
            bool flag = sc || b < in.nominalDbm - 1e-6;
            tsReduced = b < in.nominalDbm - 1e-6;
            emit("state", st, flag, stBasis);
            withHist(tsReduced ? hist([](const Cp&) { return true; }) : nullptr);
        }
        else
        {
            st = first([](const Cp& c) { return c.pending; });
            emit("state", st, st != nullptr, st ? (st->sameCycle ? "same-cycle TS" : "handover pending") : "");
            withHist(hist([](const Cp& c) { return c.pending; }));
        }
        auto stPred = [&](const Cp& c) { return isTs ? tsReduced : c.pending; };
        const Cp* evc = first([&](const Cp& c) { return c.t > ev + 1e-9 || c.sameCycle; });
        std::ostringstream eb;
        eb << "evidence start " << ev;
        emit("evidence", evc, evc != nullptr, eb.str());
        withHist(hist([&](const Cp& c) { return c.t > ev + 1e-9; }));
        bool comb = out[out.size() - 2].flagged || evc != nullptr;
        emit("combined", evc ? evc : st, comb, "");
        withHist(hist([&](const Cp& c) { return stPred(c) || c.t > ev + 1e-9; }));
    }
    return out;
}

std::vector<SaDecision>
ResolveStateAwareB4(const LabelInput& in, const std::string& def, bool tsFirst)
{
    std::vector<SaDecision> out;
    for (const auto& l : LabelInteractions(in))
    {
        if (l.def != def)
        {
            continue;
        }
        SaDecision d;
        d.prop = l.prop;
        d.label = l;
        const bool isTs = in.props[l.prop].type == Prop::TS;
        if (l.flagged)
        {
            if (tsFirst && !isTs)
            {
                d.reject = true;
                d.reason = "b4sa_priority_ts";
            }
            else if (!tsFirst && isTs)
            {
                d.reject = true;
                d.reason = "b4sa_priority_tpc";
            }
            else if (l.histId >= 0)
            {
                d.note = "priority_unenforceable";
            }
            else if (!l.sameCycleCp)
            {
                d.note = "priority_unenforceable_state"; // TS p: Y believed reduced, no dispatched record
            }
            else
            {
                d.note = "counterpart_rejected_same_cycle";
            }
        }
        out.push_back(d);
    }
    return out;
}

} // namespace coord
} // namespace xapp
