/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S4 known-answer tests of the TS / TPC decision logic
 * (S4-specification-v2 §3 TS-K1..K8, §4 TPC-K1..K7). Deterministic, no network simulation.
 * TS-K9 (RNTI change between proposal and execution) is the V1(f) execution path, verified in S3
 * (v1_stale_rnti, v1f_reest) and re-checked in the S4 pilots.
 * Usage: xapp-policy-test [--out=FILE]  -> CSV of case, verdict, observed, expected; exit 1 on failure.
 */
#include "b5-policy.h"
#include "b5sa-policy.h"
#include "coord-policy.h"
#include "rate-estimator.h"
#include "xapp-policy.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <tuple>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace xapp::policy;

namespace
{
struct Row
{
    std::string id, verdict, observed, expected;
};
std::vector<Row> g_rows;

void
Check(const std::string& id, bool ok, const std::string& obs, const std::string& exp)
{
    g_rows.push_back({id, ok ? "PASS" : "FAIL", obs, exp});
    std::cout << (ok ? "PASS " : "FAIL ") << id << ": " << obs << "\n";
}

const double NOW = 20.0;

/** k consecutive 0.5-s load reports ending at t_end (newest first). */
std::vector<LoadRep>
Reps(int k, double prb, double hol, double tEnd = NOW, uint64_t drops = 0, double txp = -1e9)
{
    std::vector<LoadRep> v;
    for (int i = 0; i < k; i++)
    {
        LoadRep r;
        r.t = tEnd - 0.5 * i;
        r.t0 = r.t - 0.5;
        r.prb = prb;
        r.holMs = hol;
        r.dropBytes = drops;
        r.txPowerDbm = txp;
        v.push_back(r);
    }
    return v;
}

UeView
Ue(uint64_t imsi, uint16_t cell, double r1, double r2, double age = 0.1)
{
    UeView u;
    u.imsi = imsi;
    u.servingCell = cell;
    u.rnti = static_cast<uint16_t>(imsi + 10);
    u.servingAge = age;
    u.rsrpDbm = {{1, r1}, {2, r2}};
    u.rsrpAge = {{1, age}, {2, age}};
    SvcRep s;
    s.t = NOW;
    s.t0 = NOW - 0.5;
    s.cell = cell;
    s.rnti = u.rnti;
    s.servedMbps = 1.0;
    s.demandMbps = 1.0;
    u.svc = s;
    return u;
}

Snapshot
TsBase()
{
    Snapshot s;
    s.now = NOW;
    s.load[1] = Reps(2, 1.0, 95); // congested twice
    s.load[2] = Reps(2, 0.30, 2); // capacity
    // imsi 4: target 2 dB weaker than serving (gamma_t ~ -2 dB, passes C2)
    // imsi 5: target 1 dB weaker (gamma_t ~ -1 dB, best)
    // imsi 6: target 6 dB weaker (gamma_t ~ -6 dB, fails C2)
    s.ues = {Ue(4, 1, -90, -92), Ue(5, 1, -90, -91), Ue(6, 1, -90, -96), Ue(9, 2, -100, -80)};
    return s;
}

int
CountTs(const std::vector<TsProposal>& v)
{
    return static_cast<int>(v.size());
}
} // namespace

int
main(int argc, char** argv)
{
    std::string out;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a.rfind("--out=", 0) == 0)
            out = a.substr(6);
    }
    TsParams tp;
    std::vector<LogEntry> log;

    // TS-K1: congested source + eligible UEs -> exactly one proposal, for the highest-gamma UE
    {
        TsState st;
        auto v = TsDecide(TsBase(), tp, st, log);
        bool ok = CountTs(v) == 1 && v[0].imsi == 5 && v[0].source == 1 && v[0].target == 2 && v[0].rnti == 15;
        Check("TS-K1", ok, std::to_string(v.size()) + " proposal(s); imsi " + (v.empty() ? "-" : std::to_string(v[0].imsi)),
              "1 proposal, imsi 5 (highest gamma_target), cell 1->2, rnti from the serving report");
    }
    // TS-K2: only one congested report (the older one is not congested) -> nothing
    {
        Snapshot s = TsBase();
        s.load[1][1].holMs = 50;
        TsState st;
        auto v = TsDecide(s, tp, st, log);
        Check("TS-K2", v.empty(), std::to_string(v.size()) + " proposals", "0 (k_on = 2 consecutive)");
    }
    // TS-K3: target PRB 0.97 -> nothing (capacity)
    {
        Snapshot s = TsBase();
        s.load[2] = Reps(2, 0.97, 2);
        TsState st;
        auto v = TsDecide(s, tp, st, log);
        Check("TS-K3", v.empty(), std::to_string(v.size()) + " proposals", "0 (target prb >= 0.959)");
    }
    // TS-K4: the C2-failing UE is never chosen even if it is the only one
    {
        Snapshot s = TsBase();
        s.ues = {Ue(6, 1, -90, -96)};
        TsState st;
        double g = GammaCtrl(s.ues[0], 2, tp.nReDbm);
        auto v = TsDecide(s, tp, st, log);
        Check("TS-K4", v.empty() && g < -3.9, "gamma_target " + std::to_string(g) + ", " + std::to_string(v.size()) + " proposals",
              "gamma_target < -3.9 dB -> not chosen");
    }
    // TS-K5: stale load report (age 1.2 s) -> stale_input, nothing
    {
        Snapshot s = TsBase();
        s.load[1] = Reps(2, 1.0, 95, NOW - 1.2);
        TsState st;
        std::vector<LogEntry> l;
        auto v = TsDecide(s, tp, st, l);
        bool logged = false;
        for (auto& e : l)
            logged = logged || e.reason == "stale_input";
        Check("TS-K5", v.empty() && logged, std::to_string(v.size()) + " proposals, stale_input logged " + std::to_string(logged),
              "0 proposals, stale_input");
    }
    // TS-K6: equal gamma -> lowest IMSI
    {
        Snapshot s = TsBase();
        s.ues = {Ue(8, 1, -90, -91), Ue(7, 1, -90, -91)};
        TsState st;
        auto v = TsDecide(s, tp, st, log);
        Check("TS-K6", CountTs(v) == 1 && v[0].imsi == 7, "imsi " + (v.empty() ? std::string("-") : std::to_string(v[0].imsi)),
              "imsi 7 (lowest)");
    }
    // TS-K7: dwell, return block and source cooldown suppress re-proposals
    {
        TsState st;
        Snapshot s = TsBase();
        auto v1 = TsDecide(s, tp, st, log); // proposes imsi 5 at t=20
        s.now = NOW + 0.5;
        for (auto& [c, r] : s.load)
            for (auto& x : r)
            {
                x.t += 0.5;
                x.t0 += 0.5;
            }
        auto v2 = TsDecide(s, tp, st, log); // cooldown (0.5 s < 1 s)
        // imsi 5 moved to cell 2 at t=20.5 (serving report); 3 s later cell 2 congested, cell 1 has capacity:
        TsState st2 = st;
        Snapshot s3 = TsBase();
        s3.now = NOW + 3.5;
        s3.load[2] = Reps(2, 1.0, 95, s3.now);
        s3.load[1] = Reps(2, 0.3, 2, s3.now);
        s3.ues = {Ue(5, 2, -91, -90)};
        s3.ues[0].servingAge = 0.1;
        Snapshot sMove = s;
        sMove.ues = {Ue(5, 2, -91, -90)};
        TsDecide(sMove, tp, st2, log); // registers the cell change at t=20.5
        auto v3 = TsDecide(s3, tp, st2, log); // within dwell (3 s < 5 s)
        s3.now = NOW + 6.0;
        for (auto& [c, r] : s3.load)
            for (auto& x : r)
            {
                x.t += 2.5;
                x.t0 += 2.5;
            }
        auto v4 = TsDecide(s3, tp, st2, log); // dwell over (5.5 s) but return block (cell 1 = previous) until 30.5
        s3.now = NOW + 11.0;
        for (auto& [c, r] : s3.load)
            for (auto& x : r)
            {
                x.t += 5;
                x.t0 += 5;
            }
        auto v5 = TsDecide(s3, tp, st2, log); // return block over (10.5 s)
        bool ok = v1.size() == 1 && v2.empty() && v3.empty() && v4.empty() && v5.size() == 1;
        Check("TS-K7", ok,
              "t20 " + std::to_string(v1.size()) + ", cooldown " + std::to_string(v2.size()) + ", dwell " +
                  std::to_string(v3.size()) + ", return-block " + std::to_string(v4.size()) + ", after " +
                  std::to_string(v5.size()),
              "1, 0, 0, 0, 1");
    }
    // TS-K8: pending handover -> no duplicate; unconfirmed after the timeout; back-off after 2 failures
    {
        TsState st;
        Snapshot s = TsBase();
        s.ues = {Ue(5, 1, -90, -91)};
        auto a = TsDecide(s, tp, st, log); // t=20 propose
        s.now = NOW + 1.0;                 // cooldown over, UE still in cell 1, still pending (1.0 s = timeout bound)
        for (auto& [c, r] : s.load)
            for (auto& x : r)
            {
                x.t += 1.0;
                x.t0 += 1.0;
            }
        auto b = TsDecide(s, tp, st, log); // pending -> skipped
        s.now = NOW + 1.5;                 // timeout -> failure 1, re-propose allowed
        for (auto& [c, r] : s.load)
            for (auto& x : r)
            {
                x.t += 0.5;
                x.t0 += 0.5;
            }
        auto c = TsDecide(s, tp, st, log);
        s.now = NOW + 3.0; // second timeout -> failure 2 -> back-off 10 s
        for (auto& [cc, r] : s.load)
            for (auto& x : r)
            {
                x.t += 1.5;
                x.t0 += 1.5;
            }
        auto d = TsDecide(s, tp, st, log);
        bool ok = a.size() == 1 && b.empty() && c.size() == 1 && d.empty() && st.ue[5].backoffUntil > NOW + 12;
        Check("TS-K8", ok,
              std::to_string(a.size()) + "," + std::to_string(b.size()) + "," + std::to_string(c.size()) + "," +
                  std::to_string(d.size()) + " backoff_until " + std::to_string(st.ue[5].backoffUntil),
              "1,0,1,0 and back-off to 33 s");
    }

    // ------------------------------------------------------------------ TPC
    TpcParams pp;
    auto tpcBase = [&]() {
        Snapshot s;
        s.now = NOW;
        s.load[2] = Reps(4, 0.2, 1, NOW, 0, 30.0);
        s.ues = {Ue(7, 2, -110, -80), Ue(8, 2, -105, -85)}; // strong serving links
        return s;
    };
    // TPC-K1: low load, safeguards pass -> one -3 dB command
    {
        TpcState st;
        auto v = TpcDecide(tpcBase(), pp, st, log);
        bool ok = v.size() == 1 && v[0].kind == "reduce" && std::fabs(v[0].targetDbm - 27) < 1e-9;
        Check("TPC-K1", ok, std::to_string(v.size()) + " " + (v.empty() ? "" : v[0].kind + " " + std::to_string(v[0].targetDbm)),
              "1 reduce to 27 dBm");
    }
    // TPC-K2: only 3 quiet reports -> nothing
    {
        Snapshot s = tpcBase();
        s.load[2] = Reps(3, 0.2, 1, NOW, 0, 30.0);
        TpcState st;
        auto v = TpcDecide(s, pp, st, log);
        Check("TPC-K2", v.empty(), std::to_string(v.size()), "0 (k_down = 4)");
    }
    // TPC-K3: C4 fails for one UE -> no reduction (gamma after the step < -3.9 dB)
    {
        Snapshot s = tpcBase();
        s.ues.push_back(Ue(9, 2, -86, -88)); // gamma now ~ -2 dB; after -3 dB ~ -5 dB
        TpcState st;
        std::vector<LogEntry> l;
        auto v = TpcDecide(s, pp, st, l);
        bool blocked = false;
        for (auto& e : l)
            blocked = blocked || e.reason == "c4_fail";
        Check("TPC-K3", v.empty() && blocked, std::to_string(v.size()) + " commands, c4_fail " + std::to_string(blocked),
              "0, reduction_blocked c4_fail");
    }
    // TPC-K4: reduced power + observable per-UE violation -> +3 dB restore
    {
        Snapshot s = tpcBase();
        s.load[2] = Reps(4, 0.6, 3, NOW, 0, 27.0);
        s.ues[0].svc->servedMbps = 0.8; // 0.8 < 0.95 x 1.0
        TpcState st;
        st.cell[2].lastChange = NOW - 3;
        auto v = TpcDecide(s, pp, st, log);
        bool ok = v.size() == 1 && v[0].kind == "restore" && std::fabs(v[0].targetDbm - 30) < 1e-9 && v[0].reason == "ue_qos_flag";
        Check("TPC-K4", ok, std::to_string(v.size()) + " " + (v.empty() ? "" : v[0].kind + " " + v[0].reason),
              "1 restore to 30 dBm, ue_qos_flag");
    }
    // TPC-K5: inside the dwell -> no reduction
    {
        Snapshot s = tpcBase();
        TpcState st;
        st.cell[2].lastChange = NOW - 1.0;
        auto v = TpcDecide(s, pp, st, log);
        Check("TPC-K5", v.empty(), std::to_string(v.size()), "0 (dwell 2 s)");
    }
    // TPC-K6: stale load report -> stale_input at nominal power, fail-safe restore at reduced power
    {
        Snapshot s = tpcBase();
        s.load[2] = Reps(4, 0.2, 1, NOW - 1.5, 0, 30.0);
        TpcState st;
        std::vector<LogEntry> l;
        auto v = TpcDecide(s, pp, st, l);
        bool logged = false;
        for (auto& e : l)
            logged = logged || e.reason == "stale_input";
        Snapshot s2 = tpcBase();
        s2.load[2] = Reps(4, 0.2, 1, NOW - 1.5, 0, 27.0);
        TpcState st2;
        st2.cell[2].lastChange = NOW - 5;
        auto v2 = TpcDecide(s2, pp, st2, log);
        bool ok = v.empty() && logged && v2.size() == 1 && v2[0].reason == "stale_load_report";
        Check("TPC-K6", ok, "nominal: " + std::to_string(v.size()) + " (stale_input " + std::to_string(logged) + "); reduced: " +
                                (v2.empty() ? std::string("none") : v2[0].kind + " " + v2[0].reason),
              "nominal: 0 + stale_input; reduced: restore stale_load_report");
    }
    // TPC-K7: at nominal power with violations -> nothing (never above nominal)
    {
        Snapshot s = tpcBase();
        s.load[2] = Reps(4, 1.0, 120, NOW, 500, 30.0);
        s.ues[0].svc->servedMbps = 0.5;
        TpcState st;
        auto v = TpcDecide(s, pp, st, log);
        Check("TPC-K7", v.empty(), std::to_string(v.size()), "0 (no increase above nominal)");
    }
    // Supplementary (unresolved-risk checks): missing per-UE report blocks a reduction; restore acts once per evidence
    {
        Snapshot s = tpcBase();
        s.ues[1].svc.reset(); // transition-period UE without a service report
        TpcState st;
        std::vector<LogEntry> l;
        auto v = TpcDecide(s, pp, st, l);
        bool why = false;
        for (auto& e : l)
            why = why || e.reason == "ue_info_unavailable";
        Check("TPC-S1", v.empty() && why, std::to_string(v.size()) + " commands, ue_info_unavailable " + std::to_string(why),
              "missing UE report = unavailable, reduction blocked");
        Snapshot r = tpcBase();
        r.load[2] = Reps(4, 0.6, 12, NOW, 0, 24.0); // HOL headroom lost at 24 dBm
        TpcState sr;
        sr.cell[2].lastChange = NOW - 3;
        auto a = TpcDecide(r, pp, sr, log);
        auto b = TpcDecide(r, pp, sr, log); // same report again: no second restore
        Check("TPC-S2", a.size() == 1 && std::fabs(a[0].targetDbm - 27) < 1e-9 && b.empty(),
              std::to_string(a.size()) + " then " + std::to_string(b.size()), "one +3 dB restore per new report, none on the same evidence");
    }

    // TPC-S3 (I3b): one packet left in the queue at the report boundary is not a violation
    {
        SvcRep r;
        r.servedMbps = 0.98;
        r.demandMbps = 1.05;
        r.demandMeanMbps = 1.0125;
        SvcRep q = r;
        q.servedMbps = 0.90;
        Check("TPC-S3", !I3Flag(r, 0.95) && I3Flag(q, 0.95),
              "served 0.98 vs mean demand 1.0125: " + std::to_string(I3Flag(r, 0.95)) + "; served 0.90: " +
                  std::to_string(I3Flag(q, 0.95)),
              "not flagged (single-window demand would flag it); flagged at 0.90");
    }

    // ------------------------------------------------------------------ S5 detector / B3 / B4
    {
        using namespace xapp::coord;
        auto TS = [](uint64_t id, uint64_t imsi, uint16_t s, uint16_t d) {
            Prop p; p.type = Prop::TS; p.id = id; p.imsi = imsi; p.src = s; p.dst = d; return p; };
        auto TP = [](uint64_t id, uint16_t c, double req, double cur) {
            Prop p; p.type = Prop::TPC; p.id = id; p.cell = c; p.requestedDbm = req; p.currentDbm = cur; return p; };
        auto flags = [](const std::vector<Prop>& v) {
            std::string o;
            for (auto& c : Detect(v))
                o += std::to_string(v[c.ts].id) + "x" + std::to_string(v[c.tpc].id) + ":" + c.pattern + (c.flagged ? "!" : "") + " ";
            return o;
        };
        auto ids = [](const std::vector<Prop>& v, const std::vector<size_t>& ix) {
            std::string o;
            for (size_t i : ix)
                o += std::to_string(v[i].id) + " ";
            return o;
        };
        // D1: TS into 2 + TPC reduction of 2 in the same cycle -> P1 flagged
        std::vector<Prop> v1 = {TS(1, 4, 1, 2), TP(2, 2, 27, 30)};
        Check("DET-1", flags(v1) == "1x2:P1! ", flags(v1), "1x2:P1! (potential conflict)");
        // D2: order independence (same pairs and flags for every permutation of the input)
        std::vector<Prop> v2 = {TS(1, 4, 1, 2), TP(2, 2, 27, 30), TS(3, 9, 2, 1), TP(4, 1, 33, 30)};
        std::vector<Prop> v2r = {v2[3], v2[1], v2[2], v2[0]};
        Check("DET-2", flags(v2) == flags(v2r), flags(v2) + "| " + flags(v2r), "identical classification for permuted input");
        // D3: TPC reduction of the TS SOURCE cell -> P2, not flagged
        std::vector<Prop> v3 = {TS(1, 4, 1, 2), TP(2, 1, 27, 30)};
        Check("DET-3", flags(v3) == "1x2:P2 ", flags(v3), "1x2:P2 (logged, not flagged)");
        // D4: TPC increase / no-op on the TS target -> P3, not flagged
        std::vector<Prop> v4 = {TS(1, 4, 1, 2), TP(2, 2, 30, 27), TP(3, 2, 30, 30)};
        Check("DET-4", flags(v4) == "1x2:P3 1x3:P3 ", flags(v4), "P3 for increase and for equal power, not flagged");
        // D5: single-xApp cycles -> no pairs
        std::vector<Prop> v5a = {TS(1, 4, 1, 2)}, v5b = {TP(2, 2, 27, 30)};
        Check("DET-5", flags(v5a).empty() && flags(v5b).empty(), "'" + flags(v5a) + "' '" + flags(v5b) + "'", "no pairs");
        // R1: B3 passes everything; TS by ascending IMSI, then TPC by ascending cell
        std::vector<Prop> v6 = {TP(10, 2, 27, 30), TS(11, 9, 2, 1), TP(12, 1, 27, 30), TS(13, 4, 1, 2)};
        auto r1 = Resolve(v6, Mode::B3, false);
        Check("B3-1", ids(v6, r1.dispatch) == "13 11 12 10 " && r1.rejected.empty(), ids(v6, r1.dispatch),
              "13 11 12 10 (TS imsi 4, 9; TPC cell 1, 2), none rejected (B3 never suppresses)");
        auto r2 = Resolve(v6, Mode::B3, true);
        Check("B3-2", ids(v6, r2.dispatch) == "12 10 13 11 " && r2.rejected.empty(), ids(v6, r2.dispatch),
              "12 10 13 11 (reversed: TPC first)");
        // B4-TS: in v6, TS 13 (1->2) x TPC 10 (cell 2 reduce) and TS 11 (2->1) x TPC 12 (cell 1 reduce) are P1
        auto r3 = Resolve(v6, Mode::B4_TS, false);
        std::string rj3;
        for (auto& [i, w] : r3.rejected)
            rj3 += std::to_string(v6[i].id) + ":" + w + " ";
        Check("B4-TS-1", ids(v6, r3.dispatch) == "13 11 " && rj3 == "10:b4_priority_ts 12:b4_priority_ts ",
              ids(v6, r3.dispatch) + "| " + rj3, "both TS dispatched, both conflicting TPC rejected (b4_priority_ts)");
        auto r4 = Resolve(v6, Mode::B4_TPC, false);
        std::string rj4;
        for (auto& [i, w] : r4.rejected)
            rj4 += std::to_string(v6[i].id) + ":" + w + " ";
        Check("B4-TPC-1", ids(v6, r4.dispatch) == "12 10 " && rj4 == "13:b4_priority_tpc 11:b4_priority_tpc ",
              ids(v6, r4.dispatch) + "| " + rj4, "both TPC dispatched, both conflicting TS rejected (b4_priority_tpc)");
        // B4 only suppresses flagged pairs: TPC on the TS source (P2) and an unrelated TS pass
        std::vector<Prop> v7 = {TS(1, 4, 1, 2), TP(2, 1, 27, 30), TP(3, 2, 27, 30), TS(4, 5, 1, 2)};
        auto r5 = Resolve(v7, Mode::B4_TS, false);
        auto r6 = Resolve(v7, Mode::B4_TPC, false);
        std::string a5, a6;
        for (auto& [i, w] : r5.rejected) a5 += std::to_string(v7[i].id) + " ";
        for (auto& [i, w] : r6.rejected) a6 += std::to_string(v7[i].id) + " ";
        Check("B4-2", ids(v7, r5.dispatch) == "1 4 2 " && a5 == "3 " && ids(v7, r6.dispatch) == "2 3 " && a6 == "1 4 ",
              "TS-first dispatch " + ids(v7, r5.dispatch) + "rejected " + a5 + "| TPC-first dispatch " + ids(v7, r6.dispatch) + "rejected " + a6,
              "TS-first: 1 4 2 / rejected 3 (once, though 2 TS conflict with it); TPC-first: 2 3 / rejected 1 4; P2 TPC (2) never suppressed");
        // No conflict: B4 (both directions) == B3
        std::vector<Prop> v8 = {TS(1, 4, 1, 2), TP(2, 1, 27, 30), TP(3, 2, 33, 30)};
        auto b3 = Resolve(v8, Mode::B3, false), bt = Resolve(v8, Mode::B4_TS, false), bp = Resolve(v8, Mode::B4_TPC, false);
        Check("B4-3", b3.dispatch == bt.dispatch && b3.dispatch == bp.dispatch && bt.rejected.empty() && bp.rejected.empty(),
              ids(v8, b3.dispatch) + "| " + ids(v8, bt.dispatch) + "| " + ids(v8, bp.dispatch), "identical, nothing rejected");
    }

    // ------------------------------------------------------------------ S5 shadow interaction labeller
    {
        using namespace xapp::coord;
        auto TS = [](uint64_t id, uint64_t imsi, uint16_t s, uint16_t d) {
            Prop p; p.type = Prop::TS; p.id = id; p.imsi = imsi; p.src = s; p.dst = d; return p; };
        auto TP = [](uint64_t id, uint16_t c, double req, double cur) {
            Prop p; p.type = Prop::TPC; p.id = id; p.cell = c; p.requestedDbm = req; p.currentDbm = cur; return p; };
        auto HT = [](uint64_t id, double t, uint64_t imsi, uint16_t s, uint16_t d) {
            HistCmd h; h.type = Prop::TS; h.id = id; h.tDisp = t; h.imsi = imsi; h.src = s; h.dst = d; return h; };
        auto HP = [](uint64_t id, double t, uint16_t c, double req, double before) {
            HistCmd h; h.type = Prop::TPC; h.id = id; h.tDisp = t; h.cell = c; h.requestedDbm = req;
            h.believedBeforeDbm = before; return h; };
        // flags of proposal 0 as "def=0/1 ..." plus would-reject markers
        auto lab = [](const LabelInput& in) {
            std::string o;
            for (auto& l : LabelInteractions(in))
                if (l.prop == 0)
                    o += l.def + "=" + std::to_string(l.flagged) + (l.tsFirstWouldReject ? "r" : "") +
                         (l.tpcFirstWouldReject ? "R" : "") + " ";
            return o;
        };
        const double T = 20.0;
        // L1: same-cycle TS into 2 + reduction of 2: every definition flags both
        LabelInput a;
        a.now = T;
        a.props = {TS(1, 4, 1, 2), TP(2, 2, 27, 30)};
        a.evidenceStart = {T - 0.5, T - 2.5};
        auto la = LabelInteractions(a);
        bool all = la.size() == 14;
        for (auto& l : la)
            all = all && l.flagged;
        Check("LAB-1", all && la[0].tpcFirstWouldReject && !la[0].tsFirstWouldReject && la[7].tsFirstWouldReject,
              std::to_string(la.size()) + " labels, all flagged " + std::to_string(all),
              "14 labels (7 definitions x 2 proposals), all flagged; TS-first rejects the TPC, TPC-first the TS");
        // L2: TPC reduction of 2 at T; TS into 2 dispatched 1.5 s earlier and already observed in cell 2;
        //     the reduction's evidence starts at T-2.5 (before the handover)
        LabelInput b;
        b.now = T;
        b.props = {TP(2, 2, 27, 30)};
        b.evidenceStart = {T - 2.5};
        b.history = {HT(9, T - 1.5, 4, 1, 2)};
        b.hoPending = {false};
        std::string eb = lab(b);
        Check("LAB-2", eb == "same_cycle=0 win_1=0 win_2=1r win_5=1r state=0 evidence=1r combined=1r ", eb,
              "window 2/5 and evidence flag (the reduction's load evidence predates the handover); not pending; "
              "TS-first would reject the TPC, TPC-first would not");
        // L3: the handover was 3 s ago, before the evidence window -> only the 5 s window flags it
        b.history = {HT(9, T - 3.0, 4, 1, 2)};
        std::string ec = lab(b);
        Check("LAB-3", ec == "same_cycle=0 win_1=0 win_2=0 win_5=1r state=0 evidence=0 combined=0 ", ec,
              "only win_5: the evidence already reflects the handover");
        // L4: TS into 2 while cell 2 was reduced 10 s ago (confirmed by a report): active state only
        LabelInput d;
        d.now = T;
        d.props = {TS(1, 4, 1, 2)};
        d.evidenceStart = {T - 0.5};
        d.history = {HP(7, T - 10, 2, 27, 30)};
        d.believedDbm = {{2, 27.0}};
        std::string ed = lab(d);
        Check("LAB-4", ed == "same_cycle=0 win_1=0 win_2=0 win_5=0 state=1R evidence=0 combined=1R ", ed,
              "state (believed 27 dBm < nominal) and combined; no window; TPC-first would reject the TS");
        // L5: reduction dispatched 0.3 s ago, no report since: state and evidence and every window
        d.history = {HP(7, T - 0.3, 2, 27, 30)};
        std::string ee = lab(d);
        Check("LAB-5", ee == "same_cycle=0 win_1=1R win_2=1R win_5=1R state=1R evidence=1R combined=1R ", ee,
              "all but same_cycle");
        // L6: TPC reduction while a handover into 2 (0.4 s ago) is pending
        b.history = {HT(9, T - 0.4, 4, 1, 2)};
        b.hoPending = {true};
        std::string ef = lab(b);
        Check("LAB-6", ef == "same_cycle=0 win_1=1r win_2=1r win_5=1r state=1r evidence=1r combined=1r ", ef,
              "pending handover: state flag");
        // L7: restorations, TS into another cell and TPC on the source are not labelled as P1 counterparts
        LabelInput g;
        g.now = T;
        g.props = {TS(1, 4, 2, 1), TP(2, 2, 30, 27), TP(3, 2, 27, 30)};
        g.evidenceStart = {T - 0.5, T - 0.5, T - 2.5};
        std::string eg = lab(g);
        size_t nlab = LabelInteractions(g).size();
        Check("LAB-7", eg == "same_cycle=0 win_1=0 win_2=0 win_5=0 state=0 evidence=0 combined=0 " && nlab == 14, eg + "| " + std::to_string(nlab) + " labels",
              "TS 2->1 vs TPC on 2 (its source) not flagged; the restore (prop 1) gets no labels; 14 labels");
        // L8: believed power and pending helpers
        std::vector<HistCmd> h = {HP(1, 5.0, 2, 27, 30)};
        double b0 = BelievedPower({}, 2, std::nan(""), -1e18, 30), b1 = BelievedPower(h, 2, 30, 4.5, 30),
               b2 = BelievedPower(h, 2, 30, 5.5, 30), b3 = BelievedPower(h, 2, 27, 5.5, 30);
        HistCmd ho = HT(1, 5.0, 4, 1, 2);
        bool p1 = HoPending(ho, 2, 5.5, 5.6, 1.0), p2 = HoPending(ho, 1, 5.5, 5.6, 1.0), p3 = HoPending(ho, 1, 5.5, 6.2, 1.0);
        Check("LAB-8", b0 == 30 && b1 == 27 && b2 == 30 && b3 == 27 && !p1 && p2 && !p3,
              "believed " + std::to_string(b0) + " " + std::to_string(b1) + " " + std::to_string(b2) + " " +
                  std::to_string(b3) + "; pending " + std::to_string(p1) + std::to_string(p2) + std::to_string(p3),
              "30 (nothing known), 27 (command not yet reported), 30 (report after dispatch shows it did not take), "
              "27; pending 0 (observed in target) 1 (not yet) 0 (timeout)");
        // L9: history order does not matter
        LabelInput m = b;
        m.history = {HT(9, T - 0.4, 4, 1, 2), HT(10, T - 3, 5, 1, 2)};
        m.hoPending = {true, false};
        LabelInput mr = m;
        std::reverse(mr.history.begin(), mr.history.end());
        std::reverse(mr.hoPending.begin(), mr.hoPending.end());
        Check("LAB-9", lab(m) == lab(mr), lab(m) + "| " + lab(mr), "identical for permuted history");
    }

    // ------------------------------------------------------------------ S5 rate estimator
    {
        using namespace xapp::est;
        AmcLut real = BuildAmcLut();
        bool mono = true;
        for (size_t i = 1; i < real.bytesPerRb.size(); i++)
            mono = mono && real.bytesPerRb[i] >= real.bytesPerRb[i - 1];
        double lo = real.Eta(-10), hi = real.Eta(34.9);
        int cqiHi = real.cqi.back();
        std::string thr;
        int lastC = -1;
        for (size_t i = 0; i < real.cqi.size(); i++)
            if (real.cqi[i] != lastC)
            {
                lastC = real.cqi[i];
                std::ostringstream q;
                q.precision(1);
                q << std::fixed << real.gMinDb + i * real.stepDb;
                thr += "cqi" + std::to_string(lastC) + "@" + q.str() + " ";
            }
        Check("EST-1", mono && lo == 0 && cqiHi == 15 && hi > 50, "monotone " + std::to_string(mono) + ", eta(-10 dB) " +
              std::to_string(lo) + ", eta(35 dB) " + std::to_string(hi) + " B/RB, thresholds " + thr,
              "ns-3 LteAmc (MiErrorModel) table: non-decreasing, 0 below the CQI-1 threshold, CQI 15 at high SINR");
        // synthetic table: constant 52.0833 payload B/RB -> c = 24 * 52.0833 * 8000 / 1e6 = 10 Mbps
        AmcLut flat;
        flat.gMinDb = -50;
        flat.stepDb = 100;
        flat.bytesPerRb = {10.0 / (24 * 8000 / 1e6)};
        auto U = [](uint64_t imsi, uint16_t c, double d, double r1, double r2) {
            UeIn u; u.imsi = imsi; u.cell = c; u.demandMbps = d; u.rsrpDbm = {{1, r1}, {2, r2}}; return u; };
        auto rate = [](const Output& o, uint64_t imsi) {
            for (auto& u : o.ues) if (u.imsi == imsi) return u.rateMbps;
            return -1.0;
        };
        Input in;
        in.ues = {U(1, 1, 2, -80, -120), U(2, 1, 3, -80, -120)};
        in.rhoInit = {{1, 0.5}, {2, 0.0}};
        Candidate N;
        N.name = "N";
        auto o = Predict(in, N, flat);
        Check("EST-2", std::fabs(rate(o, 1) - 2) < 1e-9 && std::fabs(rate(o, 2) - 3) < 1e-9 && std::fabs(o.rho[1] - 0.5) < 1e-9,
              "R " + std::to_string(rate(o, 1)) + " " + std::to_string(rate(o, 2)) + ", rho1 " + std::to_string(o.rho[1]),
              "underloaded: R = demand (2, 3), rho = 0.5");
        in.ues = {U(1, 1, 2, -80, -120), U(2, 1, 10, -80, -120)};
        o = Predict(in, N, flat);
        Check("EST-3", std::fabs(rate(o, 1) - 2) < 1e-9 && std::fabs(rate(o, 2) - 8) < 1e-9 && std::fabs(o.load[1] - 1.2) < 1e-9 && o.rho[1] == 1.0,
              "R " + std::to_string(rate(o, 1)) + " " + std::to_string(rate(o, 2)) + ", load " + std::to_string(o.load[1]),
              "PF water-filling: shares 0.2 / 0.8 -> 2 and 8 Mbps; load 1.2, rho 1");
        // power step on a noise-limited UE: gamma drops by exactly the step
        in.ues = {U(1, 2, 1, -200, -110)};
        in.rhoInit = {{1, 0.0}, {2, 0.1}};
        Candidate P;
        P.name = "P";
        P.deltaDb[2] = -3;
        double g0 = Predict(in, N, real).ues[0].gammaDb, g1 = Predict(in, P, real).ues[0].gammaDb;
        Check("EST-4", std::fabs((g0 - g1) - 3) < 1e-6, "gamma " + std::to_string(g0) + " -> " + std::to_string(g1),
              "noise-limited: -3 dB");
        // move: UE 2 (5 Mbps) from cell 1 to 2; loads follow
        in.ues = {U(1, 1, 5, -80, -120), U(2, 1, 5, -90, -90), U(3, 2, 2, -120, -80)};
        in.rhoInit = {{1, 1.0}, {2, 0.2}};
        Candidate T;
        T.name = "T";
        T.moveImsi = 2;
        T.moveTo = 2;
        auto on = Predict(in, N, flat), ot = Predict(in, T, flat);
        Check("EST-5", std::fabs(on.load[1] - 1.0) < 1e-9 && std::fabs(ot.load[1] - 0.5) < 1e-9 && std::fabs(ot.load[2] - 0.7) < 1e-9 &&
                           ot.ues[1].cellAfter == 2,
              "load N " + std::to_string(on.load[1]) + "/" + std::to_string(on.load[2]) + ", T " + std::to_string(ot.load[1]) + "/" + std::to_string(ot.load[2]),
              "N 1.0/0.2, T 0.5/0.7 (demand moves with the UE)");
        // interference coupling: cell 2 has no UE -> rho_2 converges to 0 -> gamma = S/N
        in.ues = {U(1, 1, 1, -100, -100)};
        in.rhoInit = {{1, 0.2}, {2, 1.0}};
        auto oc = Predict(in, N, real);
        double sn = -100 - (-123.24);
        Check("EST-6", oc.converged && oc.iterations == 3 && std::fabs(oc.ues[0].gammaDb - sn) < 1e-6 && oc.rho[2] == 0.0,
              "iterations " + std::to_string(oc.iterations) + ", gamma " + std::to_string(oc.ues[0].gammaDb),
              "converged in 3 iterations (it. 1 uses the reported rho_2 = 1; it. 2 rho_2 = 0 changes gamma, hence rho_1; "
              "it. 3 stable); gamma = S/N = 23.24 dB [expectation corrected 2026-11-23: was 2, miscounted]");
    }

    // ------------------------------------------------------------------ S5 B5 pair resolution
    {
        using namespace xapp::b5;
        xapp::est::AmcLut flat; // constant 10 Mbps per UE if alone in the cell
        flat.gMinDb = -50;
        flat.stepDb = 100;
        flat.bytesPerRb = {10.0 / (24 * 8000 / 1e6)};
        auto U = [](uint64_t imsi, uint16_t c, double d, double r1, double r2, bool i3b = false) {
            UeInfo u; u.ue.imsi = imsi; u.ue.cell = c; u.ue.demandMbps = d; u.ue.rsrpDbm = {{1, r1}, {2, r2}}; u.i3b = i3b;
            return u; };
        Params p;
        p.kappa = 1.0;
        Pair pr;
        pr.tsImsi = 2; pr.src = 1; pr.dst = 2; pr.tpcCell = 2; pr.deltaDb = -3;
        std::map<uint16_t, double> rho = {{1, 0.5}, {2, 0.1}};
        auto str = [](const Decision& d) {
            std::string o = d.choice + " (" + d.reason + ") ";
            for (auto& c : d.cands)
                o += c.name + ":f" + std::to_string(c.feasible) + "s" + std::to_string(c.safe) + "v" + std::to_string(c.vhat) +
                     (c.why.empty() ? "" : "[" + c.why + "]") + " ";
            return o;
        };
        // K1: u's C2 at cell 2 holds at the current power (-2.1 dB) but fails after the -3 dB step (-5.1 dB): B
        //     infeasible; P and B tie on J otherwise; P chosen
        std::vector<UeInfo> base = {U(1, 1, 2, -80, -100), U(2, 1, 3, -90, -92), U(3, 2, 1, -110, -80)};
        auto d1 = Decide(base, false, rho, pr, p, flat);
        Check("B5-K1", d1.choice == "P" && !d1.cands[3].feasible && d1.cands[3].why == "c2_fail" && d1.cands[1].feasible, str(d1),
              "P: B infeasible (C2 after the step), T feasible but J(P) < J(T)");
        // K2: u at -90.5 dBm from cell 2: not vulnerable (gamma_now +0.5 dB) and B feasible (gamma at cell 2 after the
        //     step -3.5 dB >= -3.9); J(B) = J(P), equal predicted violations -> B by preference
        //     [input corrected 2026-11-23: -88 dBm made u vulnerable (gamma_now -2.1 dB), which is a different case]
        auto b2 = base;
        b2[1] = U(2, 1, 3, -90, -90.5);
        auto d2 = Decide(b2, false, rho, pr, p, flat);
        Check("B5-K2", d2.choice == "B" && d2.reason == "b5_choice_B", str(d2), "B (tie with P broken by B > T > P > N)");
        // K3: a low-demand UE (0.3 Mbps) anywhere: all-user pessimism (eps_low 0.076 > 5 % margin) counts it violating
        //     under every candidate != N -> N (the documented provisional degeneracy)
        auto b3 = b2;
        b3[2] = U(3, 2, 0.3, -110, -80);
        auto d3 = Decide(b3, false, rho, pr, p, flat);
        Check("B5-K3", d3.choice == "N" && !d3.cands[1].safe && !d3.cands[2].safe && !d3.cands[3].safe && d3.cands[2].vhat == 1,
              str(d3), "N: T/P/B unsafe by S-a'(i) through the pessimistic low-demand estimate (provisional scope)");
        // K4: already-violating UE in cell 2 (9 Mbps) loses 2 Mbps when u (3 Mbps) moves in (PF water-filling) ->
        //     T and B unsafe; P safe -> P
        auto b4 = b2;
        b4[2] = U(3, 2, 9, -110, -80, true);
        auto d4 = Decide(b4, false, rho, pr, p, flat);
        Check("B5-K4", d4.choice == "P" && !d4.cands[1].safe && !d4.cands[3].safe && d4.cands[2].safe, str(d4),
              "P: T and B degrade the already-violating UE beyond eps_av (and add a violation)");
        // K5: an unverifiable UE in an involved cell -> every candidate but N excluded
        auto d5 = Decide(b2, true, rho, pr, p, flat);
        Check("B5-K5", d5.choice == "N" && d5.reason == "b5_unverifiable", str(d5), "N, reason b5_unverifiable");
        // K6: source congestion: cell 1 overloaded unless u leaves (u 6 Mbps, a 6 Mbps: load 1.2); T and B relieve it;
        //     B infeasible (K1 radio) -> T has J = 0 + 500 - 1, P has J = 100 + 0 - 1 -> P? compute: J(P) = 1000*(0.2/2) - 1 = 99,
        //     J(T) = 1000*(0 + 1/2) - 1 = 499 -> P
        std::vector<UeInfo> b6 = {U(1, 1, 6, -80, -100), U(2, 1, 6, -90, -92), U(3, 2, 1, -110, -80)};
        auto d6 = Decide(b6, false, rho, pr, p, flat);
        Check("B5-K6", d6.choice == "P" && std::fabs(d6.cands[2].J - 99) < 1e-9 && std::fabs(d6.cands[1].J - 499) < 1e-9, str(d6),
              "J(P) = 99 < J(T) = 499 under Formulation A (equal weights, zeta 1000): P");
        // K7: determinism / input-only: identical inputs give identical decisions; permuting UEs changes nothing
        auto b7 = b2;
        std::reverse(b7.begin(), b7.end());
        auto d7a = Decide(b2, false, rho, pr, p, flat), d7b = Decide(b7, false, rho, pr, p, flat);
        Check("B5-K7", str(d7a) == str(d7b), str(d7b), "identical decision for permuted UE order");
    }

    // ------------------------------------------------------------------ S5 state-aware B4 (decisions 1-2)
    {
        using namespace xapp::coord;
        auto TS = [](uint64_t id, uint64_t imsi, uint16_t s, uint16_t d) {
            Prop p; p.type = Prop::TS; p.id = id; p.imsi = imsi; p.src = s; p.dst = d; return p; };
        auto TP = [](uint64_t id, uint16_t c, double req, double cur) {
            Prop p; p.type = Prop::TPC; p.id = id; p.cell = c; p.requestedDbm = req; p.currentDbm = cur; return p; };
        auto HT = [](uint64_t id, double t, uint64_t imsi, uint16_t s, uint16_t d) {
            HistCmd h; h.type = Prop::TS; h.id = id; h.tDisp = t; h.imsi = imsi; h.src = s; h.dst = d; return h; };
        auto HP = [](uint64_t id, double t, uint16_t c, double req, double before) {
            HistCmd h; h.type = Prop::TPC; h.id = id; h.tDisp = t; h.cell = c; h.requestedDbm = req;
            h.believedBeforeDbm = before; return h; };
        // "prop:action/reason/note/hist" per labelled proposal
        auto sa = [](const LabelInput& in, const std::string& def, bool tsFirst) {
            std::string o;
            for (auto& d : ResolveStateAwareB4(in, def, tsFirst))
                o += std::to_string(d.prop) + ":" + (d.label.flagged ? "F" : "-") + (d.reject ? "rej/" + d.reason : "pass") +
                     (d.note.empty() ? "" : "/" + d.note) + (d.label.histId >= 0 ? "/h" + std::to_string(d.label.histId) : "") + " ";
            return o;
        };
        const double T = 20.0;
        bool neverHistory = true; // SA-7: only current proposals are ever rejected; the history is never touched
        auto chk = [&](const LabelInput& in) {
            LabelInput c = in;
            for (bool f : {true, false})
                for (const std::string def : {"combined", "win_2", "win_5"})
                    for (auto& d : ResolveStateAwareB4(c, def, f))
                        neverHistory = neverHistory && d.prop < c.props.size();
            neverHistory = neverHistory && c.history.size() == in.history.size();
        };
        // SA-1 same-cycle pair: as the approved B4 in both directions
        LabelInput a;
        a.now = T;
        a.props = {TS(1, 4, 1, 2), TP(2, 2, 27, 30)};
        a.evidenceStart = {T - 0.5, T - 2.5};
        chk(a);
        std::string s1 = sa(a, "combined", true), s1r = sa(a, "combined", false);
        Check("SA-1", s1 == "0:Fpass/counterpart_rejected_same_cycle 1:Frej/b4sa_priority_ts " &&
                          s1r == "0:Frej/b4sa_priority_tpc 1:Fpass/counterpart_rejected_same_cycle ",
              s1 + "| " + s1r, "TS-first rejects the TPC, TPC-first the TS (= same-cycle B4)");
        // SA-2 TS into 2 dispatched 0.4 s ago, still pending; reduction of 2 now
        LabelInput b;
        b.now = T;
        b.props = {TP(2, 2, 27, 30)};
        b.evidenceStart = {T - 2.5};
        b.history = {HT(9, T - 0.4, 4, 1, 2)};
        b.hoPending = {true};
        chk(b);
        std::string s2 = sa(b, "combined", true), s2r = sa(b, "combined", false);
        Check("SA-2", s2 == "0:Frej/b4sa_priority_ts/h9 " && s2r == "0:Fpass/priority_unenforceable/h9 ", s2 + "| " + s2r,
              "TS-first rejects the later reduction; TPC-first passes it (the handover cannot be undone) and logs it");
        // SA-3 cell 2 reduced (dispatched 10 s ago, believed 27); TS into 2 now
        LabelInput d;
        d.now = T;
        d.props = {TS(1, 4, 1, 2)};
        d.evidenceStart = {T - 0.5};
        d.history = {HP(7, T - 10, 2, 27, 30)};
        d.believedDbm = {{2, 27.0}};
        chk(d);
        std::string s3 = sa(d, "combined", false), s3r = sa(d, "combined", true);
        LabelInput d0 = d;
        d0.history.clear(); // reduced state known only from a power report
        std::string s3b = sa(d0, "combined", true);
        Check("SA-3", s3 == "0:Frej/b4sa_priority_tpc/h7 " && s3r == "0:Fpass/priority_unenforceable/h7 " &&
                          s3b == "0:Fpass/priority_unenforceable_state ",
              s3 + "| " + s3r + "| " + s3b,
              "TPC-first rejects the later TS into the reduced cell; TS-first passes it (no restoration) and logs it");
        // SA-4 pending vs completed handover: TS into 2 dispatched 0.8 s ago; the reduction's evidence starts after it
        LabelInput e = b;
        e.evidenceStart = {T - 0.5};
        e.history = {HT(9, T - 0.8, 4, 1, 2)};
        e.hoPending = {true};
        std::string s4p = sa(e, "combined", true);
        e.hoPending = {false};
        std::string s4c = sa(e, "combined", true);
        chk(e);
        Check("SA-4", s4p == "0:Frej/b4sa_priority_ts/h9 " && s4c == "0:-pass ", s4p + "| " + s4c,
              "pending: flagged (state); completed and already in the evidence: not flagged");
        // SA-5 stale observation: reduction of 2 dispatched 0.3 s ago; TS evidence starts after the dispatch
        std::vector<HistCmd> h5 = {HP(7, T - 0.3, 2, 27, 30)};
        LabelInput f;
        f.now = T;
        f.props = {TS(1, 4, 1, 2)};
        f.evidenceStart = {T - 0.2};
        f.history = h5;
        f.believedDbm = {{2, BelievedPower(h5, 2, 30.0, T - 0.5, 30)}}; // report generated before the dispatch
        std::string s5a = sa(f, "combined", false);
        f.believedDbm = {{2, BelievedPower(h5, 2, 30.0, T - 0.1, 30)}}; // report after it: command did not take effect
        std::string s5b = sa(f, "combined", false);
        chk(f);
        Check("SA-5", s5a == "0:Frej/b4sa_priority_tpc/h7 " && s5b == "0:-pass ", s5a + "| " + s5b,
              "an older report (30) does not override the dispatched command (27); a newer report (30) does");
        // SA-6 registered window sensitivities: reduction 3 s ago, believed 27; and the same after a restoration
        LabelInput g;
        g.now = T;
        g.props = {TS(1, 4, 1, 2)};
        g.evidenceStart = {T - 0.5};
        g.history = {HP(7, T - 3, 2, 27, 30)};
        g.windows = {2.0, 5.0};
        g.believedDbm = {{2, 27.0}};
        std::string g1 = sa(g, "combined", false) + sa(g, "win_2", false) + sa(g, "win_5", false);
        g.history.push_back(HP(8, T - 1, 2, 30, 27));
        g.believedDbm = {{2, 30.0}};
        std::string g2 = sa(g, "combined", false) + sa(g, "win_2", false) + sa(g, "win_5", false);
        chk(g);
        Check("SA-6", g1 == "0:Frej/b4sa_priority_tpc/h7 0:-pass 0:Frej/b4sa_priority_tpc/h7 " &&
                          g2 == "0:-pass 0:-pass 0:Frej/b4sa_priority_tpc/h7 ",
              g1 + "| " + g2, "combined follows the believed state; win_2 misses the 3 s gap; win_5 flags even after "
                              "the cell was restored");
        Check("SA-7", neverHistory, std::to_string(neverHistory),
              "every rejection refers to a current proposal; no history entry is ever cancelled or restored");
        // SA-8 order independence
        LabelInput m = b;
        m.history = {HT(9, T - 0.4, 4, 1, 2), HT(10, T - 3, 5, 1, 2)};
        m.hoPending = {true, false};
        LabelInput mr = m;
        std::reverse(mr.history.begin(), mr.history.end());
        std::reverse(mr.hoPending.begin(), mr.hoPending.end());
        Check("SA-8", sa(m, "combined", true) == sa(mr, "combined", true) && sa(m, "combined", false) == sa(mr, "combined", false),
              sa(m, "combined", true) + "| " + sa(mr, "combined", true), "identical for permuted history");
    }

    // ------------------------------------------------------------------ S5 state-aware B5
    {
        using namespace xapp;
        est::AmcLut flat;
        flat.gMinDb = -50;
        flat.stepDb = 100;
        flat.bytesPerRb = {10.0 / (24 * 8000 / 1e6)};
        auto U = [](uint64_t imsi, uint16_t c, double d, double r1, double r2, bool i3b = false) {
            b5::UeInfo u; u.ue.imsi = imsi; u.ue.cell = c; u.ue.demandMbps = d; u.ue.rsrpDbm = {{1, r1}, {2, r2}}; u.i3b = i3b;
            return u; };
        b5::Params p;
        p.kappa = 1.0;
        std::map<uint16_t, double> rho = {{1, 0.5}, {2, 0.1}};
        auto str = [](const std::vector<b5::CandEval>& v, const std::string& ch) {
            std::string o = ch + " ";
            for (auto& c : v)
                o += c.name + ":f" + std::to_string(c.feasible) + "s" + std::to_string(c.safe) + "v" + std::to_string(c.vhat) +
                     "J" + std::to_string(c.J) + (c.why.empty() ? "" : "[" + c.why + "]") + " ";
            return o;
        };
        auto sc = [&](const std::string& n, std::vector<b5::UeInfo> u, double delta) {
            b5sa::Scenario s; s.name = n; s.ues = u; s.rho = rho; s.tpcDeltaDb = delta; return s; };
        b5sa::Unit pairU;
        pairU.kind = "pair"; pairU.tsImsi = 2; pairU.src = 1; pairU.dst = 2; pairU.tpcCell = 2; pairU.cands = {"N", "T", "P", "B"};
        b5::Pair pr;
        pr.tsImsi = 2; pr.src = 1; pr.dst = 2; pr.tpcCell = 2; pr.deltaDb = -3;
        // K1: same-cycle pair, one believed scenario: identical to the original B5 on the B5-K1..K6 inputs
        std::vector<std::vector<b5::UeInfo>> ins = {
            {U(1, 1, 2, -80, -100), U(2, 1, 3, -90, -92), U(3, 2, 1, -110, -80)},
            {U(1, 1, 2, -80, -100), U(2, 1, 3, -90, -90.5), U(3, 2, 1, -110, -80)},
            {U(1, 1, 2, -80, -100), U(2, 1, 3, -90, -90.5), U(3, 2, 0.3, -110, -80)},
            {U(1, 1, 2, -80, -100), U(2, 1, 3, -90, -90.5), U(3, 2, 9, -110, -80, true)},
            {U(1, 1, 6, -80, -100), U(2, 1, 6, -90, -92), U(3, 2, 1, -110, -80)}};
        bool same = true;
        std::string ex;
        for (const auto& u : ins)
        {
            auto o = b5::Decide(u, false, rho, pr, p, flat);
            auto n = b5sa::Decide({sc("believed", u, -3)}, "", pairU, p, flat);
            same = same && o.choice == n.choice && str(o.cands, o.choice) == str(n.cands, n.choice);
            ex += o.choice + "=" + n.choice + " ";
        }
        auto o5 = b5::Decide(ins[1], true, rho, pr, p, flat);
        auto n5 = b5sa::Decide({sc("believed", ins[1], -3)}, "unverifiable", pairU, p, flat);
        same = same && o5.choice == n5.choice && n5.reason == "b5sa_unverifiable";
        Check("B5SA-K1", same, ex + "unverifiable " + n5.choice, "B5sa = B5 on every same-cycle known answer (P B N P P; N)");
        // K2: sequential units evaluate only present-time actions: ts unit {N, T}, tpc unit {N, P}
        b5sa::Unit tsU = pairU;
        tsU.kind = "ts"; tsU.cands = {"N", "T"};
        auto k2 = b5sa::Decide({sc("believed", ins[1], 0)}, "", tsU, p, flat);
        b5sa::Unit tpcU = pairU;
        tpcU.kind = "tpc"; tpcU.cands = {"N", "P"};
        // the earlier TS (UE 2 -> cell 2) in effect; UE 2 sees cell 2 3 dB above cell 1 (not vulnerable there)
        // [input corrected 2026-11-23 before acceptance: ins[1]'s UE 2 is vulnerable in cell 2 (gamma -0.5 dB), so
        //  the provisional all-user pessimism correctly made P unsafe - a different case]
        std::vector<b5::UeInfo> moved = ins[1];
        moved[1] = U(2, 2, 3, -91, -88);
        auto k2b = b5sa::Decide({sc("believed", moved, -3)}, "", tpcU, p, flat);
        Check("B5SA-K2", k2.cands.size() == 2 && k2.choice == "T" && k2b.cands.size() == 2 && k2b.cands[1].name == "P" &&
                             k2b.choice == "P",
              str(k2.cands, k2.choice) + "| " + str(k2b.cands, k2b.choice),
              "ts unit: N/T only, T chosen (safe, tie on J, T > N); tpc unit: N/P only, P chosen (s_TPC)");
        // K3: uncertain state handled conservatively: P safe in the believed scenario (UE 2 in cell 2), but if the
        //     earlier handover has not completed UE 2 is still in cell 1, vulnerable there (gamma -3 dB), and the
        //     pessimistic estimate counts a violation -> P unsafe in that scenario -> excluded, reason tagged
        std::vector<b5::UeInfo> notDone = moved;
        notDone[1].ue.cell = 1;
        auto k3 = b5sa::Decide({sc("believed", moved, -3), sc("ho2_not_done", notDone, -3)}, "", tpcU, p, flat);
        auto k3a = b5sa::Decide({sc("believed", moved, -3)}, "", tpcU, p, flat);
        Check("B5SA-K3", k3.choice == "N" && k3.cands[1].feasible && !k3.cands[1].safe &&
                             k3.cands[1].why == "sa_i_more_violations@ho2_not_done" && k3a.choice == "P",
              str(k3.cands, k3.choice), "N: P must be safe in every plausible state (believed alone: P)");
        // K4: blocked (unverifiable UE / too many uncertain items): only N, reason recorded
        auto k4 = b5sa::Decide({sc("believed", moved, -3)}, "uncertain_overflow", tpcU, p, flat);
        Check("B5SA-K4", k4.choice == "N" && k4.reason == "b5sa_uncertain_overflow" && k4.cands[1].why == "uncertain_overflow",
              k4.choice + " " + k4.reason, "N, reason b5sa_uncertain_overflow");
        // K5: per-scenario, per-candidate predictions are returned for offline error evaluation
        Check("B5SA-K5", k3.preds.size() == 4 && k3.preds[0].ues.size() == 3 && k3.perScenario.size() == 2,
              std::to_string(k3.preds.size()) + " prediction sets", "2 scenarios x 2 candidates, 3 UEs each");
    }

    // --bench: wall-clock cost of one decision (D1 maps measured computation into processing delay).
    // Snapshot sizes as in the study (2 cells, 9 UEs, 8 load reports per cell); median of 21 repetitions.
    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--bench")
        {
            Snapshot s = TsBase();
            for (int k = 0; k < 6; k++)
                s.ues.push_back(Ue(20 + k, k % 2 ? 1 : 2, -95 - k, -97 + k));
            s.load[1] = Reps(8, 1.0, 95);
            s.load[2] = Reps(8, 0.2, 1, NOW, 0, 30.0);
            auto timeIt = [&](auto fn) {
                std::vector<double> us;
                for (int rep = 0; rep < 21; rep++)
                {
                    const int n = 20000;
                    auto t0 = std::chrono::steady_clock::now();
                    for (int j = 0; j < n; j++)
                        fn();
                    auto t1 = std::chrono::steady_clock::now();
                    us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / n);
                }
                std::sort(us.begin(), us.end());
                return std::make_pair(us[10], us[19]);
            };
            auto ts = timeIt([&]() { TsState st; std::vector<LogEntry> l; return TsDecide(s, tp, st, l).size(); });
            auto tpc = timeIt([&]() { TpcState st; std::vector<LogEntry> l; return TpcDecide(s, pp, st, l).size(); });
            std::cout << "BENCH ts_decide_us median " << ts.first << " p95 " << ts.second << "\n";
            std::cout << "BENCH tpc_decide_us median " << tpc.first << " p95 " << tpc.second << "\n";
            // S5 pre-P3 timing verification (2026-11-23): B5 / B5sa decision cost on a study-sized input (9 UEs,
            // 2 cells, real AMC LUT), timed with the same steady_clock as the in-simulation compute_us AND, as an
            // independent clock, process CPU time (std::clock). Measurement only; no policy code is changed.
            {
                xapp::est::AmcLut lut = xapp::est::BuildAmcLut();
                auto B = [](uint64_t imsi, uint16_t c, double d, double r1, double r2, bool i3b = false) {
                    xapp::b5::UeInfo u; u.ue.imsi = imsi; u.ue.cell = c; u.ue.demandMbps = d; u.ue.rsrpDbm = {{1, r1}, {2, r2}};
                    u.i3b = i3b; return u; };
                std::vector<xapp::b5::UeInfo> ues = {B(1, 1, 0.92, -84, -108), B(2, 1, 0.92, -82, -110), B(3, 1, 0.92, -85, -107),
                                                    B(4, 1, 0.92, -97, -101), B(5, 1, 0.92, -98, -103), B(6, 1, 0.92, -97, -102),
                                                    B(7, 2, 0.29, -110, -86), B(8, 2, 0.29, -104, -92), B(9, 2, 0.29, -111, -86)};
                std::map<uint16_t, double> rho{{1, 0.96}, {2, 0.42}};
                xapp::b5::Pair pr; pr.tsImsi = 4; pr.src = 1; pr.dst = 2; pr.tpcCell = 2; pr.deltaDb = -3;
                xapp::b5::Params bp;
                xapp::b5sa::Unit tsU; tsU.kind = "ts"; tsU.tsImsi = 4; tsU.src = 1; tsU.dst = 2; tsU.tpcCell = 2; tsU.cands = {"N", "T"};
                xapp::b5sa::Unit pU = tsU; pU.kind = "pair"; pU.cands = {"N", "T", "P", "B"};
                auto S = [&](const std::string& n, std::vector<xapp::b5::UeInfo> u) {
                    xapp::b5sa::Scenario s; s.name = n; s.ues = u; s.rho = rho; s.tpcDeltaDb = -3; return s; };
                auto notDone = ues; notDone[3].ue.cell = 1;
                std::vector<xapp::b5sa::Scenario> one{S("believed", ues)}, two{S("believed", ues), S("ho_not_done", notDone)};
                volatile size_t sink = 0;
                auto timeBoth = [&](auto fn) {
                    std::vector<double> wall, cpu;
                    for (int rep = 0; rep < 21; rep++)
                    {
                        const int n = 2000;
                        auto t0 = std::chrono::steady_clock::now();
                        std::clock_t c0 = std::clock();
                        for (int j = 0; j < n; j++)
                            sink = sink + fn();
                        std::clock_t c1 = std::clock();
                        auto t1 = std::chrono::steady_clock::now();
                        wall.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / n);
                        cpu.push_back(1e6 * double(c1 - c0) / CLOCKS_PER_SEC / n);
                    }
                    std::sort(wall.begin(), wall.end());
                    std::sort(cpu.begin(), cpu.end());
                    return std::make_tuple(wall[10], wall[19], cpu[10]);
                };
                auto pr1 = [&](const char* name, std::tuple<double, double, double> r) {
                    std::cout << "BENCH " << name << " median " << std::get<0>(r) << " p95 " << std::get<1>(r)
                              << " cpu_median " << std::get<2>(r) << " wall/cpu " << std::get<0>(r) / std::get<2>(r) << "\n";
                };
                pr1("b5_pair_us", timeBoth([&]() { return xapp::b5::Decide(ues, false, rho, pr, bp, lut).cands.size(); }));
                pr1("b5sa_pair_1scen_us", timeBoth([&]() { return xapp::b5sa::Decide(one, "", pU, bp, lut).cands.size(); }));
                pr1("b5sa_ts_1scen_us", timeBoth([&]() { return xapp::b5sa::Decide(one, "", tsU, bp, lut).cands.size(); }));
                pr1("b5sa_ts_2scen_us", timeBoth([&]() { return xapp::b5sa::Decide(two, "", tsU, bp, lut).cands.size(); }));
                // clock sanity against a known interval
                auto s0 = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                std::cout << "BENCH steady_clock_sleep20ms_us "
                          << std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - s0).count() << "\n";
            }
        }
    }

    int fails = 0;
    for (auto& r : g_rows)
        fails += r.verdict == "FAIL";
    if (!out.empty())
    {
        std::ofstream f(out);
        f << "case,verdict,observed,expected\n";
        for (auto& r : g_rows)
            f << r.id << "," << r.verdict << ",\"" << r.observed << "\",\"" << r.expected << "\"\n";
    }
    std::cout << g_rows.size() - fails << " PASS, " << fails << " FAIL\n";
    return fails ? 1 : 0;
}
