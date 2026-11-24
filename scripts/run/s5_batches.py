#!/usr/bin/env python3
"""S5 part 2 batches (authorised 2026-11-23: C-A, shadow interaction diagnostic, estimator harness).

C-A (controller activation) is a protocol correction: every controller run from here on carries
--ctrl-activation=warmup (T_A = traffic start 3 s + warm-up 2 s = 5 s). The binary default stays 'none' so that
every earlier command line reproduces its recorded result.

s5retro    the s5pilot configurations unchanged (C-A off) + the shadow interaction log: (i) proves the diagnostic
           changes no action (all original outputs byte-identical to s5pilot), (ii) labels the existing traces.
s5pilotca  the s5pilot coordination pilots and their single-controller references (both shadow ~ B0, TS only,
           TPC only; = s5joint for the same 5 conditions) re-run under C-A, with the shadow interaction log.
s5diag     prospective shadow diagnostic on INDEPENDENT seeds 211-214: B3 (both xApps execute, no suppression),
           C-A, all 12 scenario conditions.
s5estcal   estimator calibration (kappa only), seeds 301-303: 12 conditions x 4 executed candidates (N, T, P, B)
           by scripted commands at t_a = 10 s (T = UE 3 -> cell 2, P = cell 2 -3 dB), probe at t_a, 20 s runs.
s5estval   held-out validation, seeds 304-311, identical design. Analysed only with kappa frozen from s5estcal.

State-aware B4 / B5 (authorised 2026-11-23, "S5 final coordination authorization"):
s5seqest   estimator in SEQUENTIAL states, independent seeds 341-342, 12 conditions. Per cluster 3 orders x 2
           executed candidates, scripted, --coord=b5sa --b5-shadow=true (B5sa evaluates and logs, never acts):
             q1 Y reduced at 8.5 s, TS into Y at 10 s?   q1_N / q1_T
             q2 TS into Y at 9.5 s (pending at 10 s), reduction of Y at 10 s (ev 8.0)?   q2_N / q2_P
             q3 TS into Y at 8.5 s (completed), reduction of Y at 10 s (ev 8.0)?          q3_N / q3_P
           kappa / epsilon frozen; evaluation only. The snapshot probe at 10 s (as s5estval) gives the UE classes
           and the naive (observed-state, not re-referenced) prediction for comparison.
s5sapilot  independent paired network pilot, seeds 221-222, all 12 conditions, both xApps, C-A, shadow labels:
           b3, b4-ts, b4-tpc, b5, b4sa-ts, b4sa-tpc, b5sa (combined). The win_2 / win_5 sensitivities are read
           from the shadow labels of the b3 runs (they act only where they flag).
s5sademo   NON-INDEPENDENT demonstration replay: the s5pilotca conditions and seeds 201-202 (used when the
           combined definition was proposed) under b4sa-ts, b4sa-tpc, b5sa; paired with the recorded s5pilotca
           B3 / B4 and s5b5pilot B5 runs. Reported separately and labelled; never pooled with s5sapilot.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from batch import run_batch  # noqa: E402
from s4_batches import ATTACH, LAY, XAPP, s5pilot, scen  # noqa: E402

CA = ["--ctrl-activation=warmup"]
CONDS = [(x, ra, rb) for x in LAY for ra in (0.51, 0.92, 1.23) for rb in (0.29, 1.43)]
T_A = 10
CANDS = {"N": "", "T": f"{T_A}:ho:ue=3:cell=2", "P": f"{T_A}:tpc:cell=2:dbm=27",
         "B": f"{T_A}:ho:ue=3:cell=2;{T_A}:tpc:cell=2:dbm=27"}


def s5retro():
    return [(rid.replace("cp_", "rt_", 1), "S5-retro-diagnostic", sc, args + ["--interact-log=true"])
            for rid, _, sc, args in s5pilot()]


def s5pilotca():
    c = [(rid.replace("cp_", "ca_", 1), "S5-coord-pilot-CA", sc, args + CA + ["--interact-log=true"])
         for rid, _, sc, args in s5pilot()]
    conds = [("x45", 1.23, 0.29), ("x55", 0.92, 0.29), ("x55", 1.23, 0.29), ("x45", 1.23, 1.43), ("x45", 0.51, 0.29)]
    for x, ra, rb in conds:
        for ex in ("none", "ts", "tpc"):
            for s in (201, 202):
                c.append((f"caref_{x}_a{ra}_b{rb}_{ex}_s{s}", "S5-coord-pilot-CA-ref", x,
                          XAPP + scen(x, ra, rb) + [ATTACH, "--attach-move-time=1.0", "--lm=dual",
                                                   f"--dual-exec={ex}", f"--RngRun={s}"] + CA))
    return c


def s5b5pilot():
    """B5 short paired network pilot (authorised 2026-11-23): the s5pilotca conditions and seeds (201-202), C-A,
    --coord=b5; paired with the existing s5pilotca B3 / B4 / single-controller runs (not re-run)."""
    return [(rid.replace("ca_", "b5p_", 1).replace("_b3_", "_b5_"), "S5-B5-pilot", sc,
             [a if a != "--coord=b3" else "--coord=b5" for a in args])
            for rid, _, sc, args in s5pilotca() if rid.startswith("ca_") and "_b3_" in rid]


def s5diag():
    return [(f"dg_{x}_a{ra}_b{rb}_b3_s{s}", "S5-diagnostic-prospective", x,
             XAPP + scen(x, ra, rb) + [ATTACH, "--attach-move-time=1.0", "--lm=dual", "--dual-exec=both",
                                      "--coord=b3", f"--RngRun={s}", "--interact-log=true"] + CA)
            for x, ra, rb in CONDS for s in (211, 212, 213, 214)]


def _est(prefix, family, seeds):
    c = []
    for x, ra, rb in CONDS:
        for cand, script in CANDS.items():
            for s in seeds:
                a = [v for v in XAPP if not v.startswith("--sim-time")] + ["--sim-time=20s"] + scen(x, ra, rb) + \
                    [ATTACH, "--attach-move-time=1.0", "--lm=script"] + ([f"--cmd-script={script}"] if script else []) + [
                     f"--est-probe-time={T_A}", "--est-ts-ue=3", "--est-ts-target=2", "--est-tpc-cell=2",
                     "--est-step-db=3", "--est-kappa-grid=true", f"--RngRun={s}"] + CA
                c.append((f"{prefix}_{x}_a{ra}_b{rb}_{cand}_s{s}", family, x, a))
    return c


def s5estcal():
    return _est("ec", "S5-estimator-calibration", (301, 302, 303))


def s5estval():
    return _est("ev", "S5-estimator-validation", tuple(range(304, 312)))


SEQ = {"q1_N": "8.5:tpc:cell=2:dbm=27", "q1_T": "8.5:tpc:cell=2:dbm=27;10:ho:ue=3:cell=2",
       "q2_N": "9.5:ho:ue=3:cell=2", "q2_P": "9.5:ho:ue=3:cell=2;10:tpc:cell=2:dbm=27:ev=8.0",
       "q3_N": "8.5:ho:ue=3:cell=2", "q3_P": "8.5:ho:ue=3:cell=2;10:tpc:cell=2:dbm=27:ev=8.0"}
SA_MODES = ["b3", "b4-ts", "b4-tpc", "b5", "b4sa-ts", "b4sa-tpc", "b5sa"]


def s5seqest():
    c = []
    for x, ra, rb in CONDS:
        for cand, script in SEQ.items():
            for s in (341, 342):
                a = [v for v in XAPP if not v.startswith("--sim-time")] + ["--sim-time=20s"] + scen(x, ra, rb) + \
                    [ATTACH, "--attach-move-time=1.0", "--lm=script", f"--cmd-script={script}", "--cmd-lifecycle=true",
                     "--coord=b5sa", "--b5-shadow=true", f"--est-probe-time={T_A}", "--est-ts-ue=3", "--est-ts-target=2",
                     "--est-tpc-cell=2", "--est-step-db=3", "--est-kappa=0.85", f"--RngRun={s}"] + CA
                c.append((f"sq_{x}_a{ra}_b{rb}_{cand}_s{s}", "S5-SA-estimator-sequential", x, a))
    return c


def _pilot_args(x, ra, rb, mode, s):
    return XAPP + scen(x, ra, rb) + [ATTACH, "--attach-move-time=1.0", "--lm=dual", "--dual-exec=both",
                                     f"--coord={mode}", f"--RngRun={s}", "--interact-log=true"] + CA


def s5sapilot():
    return [(f"sp_{x}_a{ra}_b{rb}_{m.replace('-', '')}_s{s}", "S5-SA-pilot", x, _pilot_args(x, ra, rb, m, s))
            for x, ra, rb in CONDS for m in SA_MODES for s in (221, 222)]


def s5sademo():
    return [(rid.replace("ca_", "sd_", 1).replace("_b3_", f"_{m.replace('-', '')}_"), "S5-SA-demo-nonindependent", sc,
             [a if a != "--coord=b3" else f"--coord={m}" for a in args])
            for rid, _, sc, args in s5pilotca() if rid.startswith("ca_") and "_b3_" in rid
            for m in ("b4sa-ts", "b4sa-tpc", "b5sa")]


# ---------------------------------------------------------------------------------------------------------------
# P3 timing pilot and P4 statistical pilot (authorised 2026-11-23 after G5; Option A: B5 / B5sa frozen as evaluated).
# Registered in docs/progress-reports/P3-P4-prelaunch.md BEFORE any P3/P4 output existed. Policies, thresholds,
# traffic, positions, powers and controller start times are those of s5sapilot (unchanged).
#
# P3 (P-1.0 §7.1 item 4; S5-11; D-TIM-6; control-delay-configurations.md): seeds 231 (stage 1) and 232 (stage 2),
#   12 conditions, modes b3 / b4sa-ts / b4sa-tpc / b5sa. Timing levels, one factor at a time from D0:
#     D0      reference: all modelled delays 0, query at the report instant (input age 0.5 s)
#     PH1     query phase +1 ms (RIC started at 1.001 s): reports of the same instant are seen (age ~ 0)
#     E1      E2 transport 1 ms up and 1 ms down (indicative lower published value)
#     E7      E2 transport 7 ms up and 7 ms down (indicative upper published value)
#     D1c     mapped computation, k = 1: LM processing 12 us (TS p95 + TPC p95, bench), coordination hold 131 us
#             (in-simulation B5sa p95; B4sa uses the same, conservative) for coordinated arms
#     K100    mapped computation, k = 100 (1.2 ms LM, 13.1 ms hold)
#     DL50 / DL125 / DL250   coordination hold Delta = 0.1 / 0.25 / 0.5 x T_control (coordinated arms only)
#   Delta >= T_control (1x, 2x) is NOT run: the U5 overlap rule is undecided.
#   B3 is not run at hold-only levels (D1c/K100 hold, DL*): its paired reference is the same level's LM/E2 part,
#   which for those levels equals D0 (B3 has no coordination step). B3 at D1c/K100 runs with the LM part only.
# P4 (P-1.0 §7.1 item 5; D-STA-1): seeds 241-245 (stage 1), 246-250 (stage 2), 12 conditions, D0, modes
#   B0 (dual-exec none) / B1 (ts) / B2 (tpc) / b3 / b4-ts / b4-tpc / b5 / b4sa-ts / b4sa-tpc / b5sa.
# ---------------------------------------------------------------------------------------------------------------
P3_MODES = ["b3", "b4sa-ts", "b4sa-tpc", "b5sa"]
E2UP = "--ns3::OranE2NodeTerminator::TransmissionDelayRv=ns3::ConstantRandomVariable[Constant={}]"
P3_LEVELS = {
    "D0": ([], []),
    "PH1": (["--ric-start=1.001"], []),
    "E1": ([E2UP.format(0.001), "--e2-cmd-delay=0.001"], []),
    "E7": ([E2UP.format(0.007), "--e2-cmd-delay=0.007"], []),
    "D1c": (["--proc-delay=0.000012"], ["--coord-delay=0.000131"]),
    "K100": (["--proc-delay=0.0012"], ["--coord-delay=0.0131"]),
    "DL50": ([], ["--coord-delay=0.05"]),
    "DL125": ([], ["--coord-delay=0.125"]),
    "DL250": ([], ["--coord-delay=0.25"]),
}


def _p3(seeds):
    c = []
    for x, ra, rb in CONDS:
        for lv, (base, hold) in P3_LEVELS.items():
            for m in P3_MODES:
                if m == "b3" and lv.startswith("DL"):
                    continue
                extra = base + ([] if m == "b3" else hold)
                args = [a for a in _pilot_args(x, ra, rb, m, 0) if not a.startswith("--RngRun")]
                if any(a.startswith("--proc-delay") for a in extra):
                    args = [a for a in args if not a.startswith("--proc-delay")]
                for s in seeds:
                    c.append((f"p3_{lv}_{x}_a{ra}_b{rb}_{m.replace('-', '')}_s{s}", f"P3-timing-{lv}", x,
                              args + [f"--RngRun={s}"] + extra + ["--cmd-lifecycle=true"]))
    return c


def p3s1():
    return _p3([231])


def p3s2():
    return _p3([232])


P4_MODES = ["none", "ts", "tpc", "b3", "b4-ts", "b4-tpc", "b5", "b4sa-ts", "b4sa-tpc", "b5sa"]


def _p4(seeds):
    c = []
    for x, ra, rb in CONDS:
        for m in P4_MODES:
            for s in seeds:
                if m in ("none", "ts", "tpc"):
                    args = XAPP + scen(x, ra, rb) + [ATTACH, "--attach-move-time=1.0", "--lm=dual", f"--dual-exec={m}",
                                                     f"--RngRun={s}"] + CA  # no --interact-log: it requires --coord (B0/B1/B2 have none)
                    name = {"none": "B0", "ts": "B1", "tpc": "B2"}[m]
                else:
                    args = _pilot_args(x, ra, rb, m, s)
                    name = m.replace("-", "")
                c.append((f"p4_{x}_a{ra}_b{rb}_{name}_s{s}", "P4-statistical", x, args))
    return c


def p4s1():
    return _p4([241, 242, 243, 244, 245])


def p4s2():
    return _p4([246, 247, 248, 249, 250])


if __name__ == "__main__":
    which = sys.argv[1]
    jobs = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    run_batch(which, {"s5retro": s5retro, "s5pilotca": s5pilotca, "s5diag": s5diag, "s5estcal": s5estcal,
                       "s5estval": s5estval, "s5b5pilot": s5b5pilot, "s5seqest": s5seqest,
                       "s5sapilot": s5sapilot, "s5sademo": s5sademo,
                       "p3s1": p3s1, "p3s2": p3s2, "p4s1": p4s1, "p4s2": p4s2}[which](), jobs=jobs, stage="s5",
              data_root=os.path.expanduser("~/xapp-research-data/s5"))
