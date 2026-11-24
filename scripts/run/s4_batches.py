#!/usr/bin/env python3
"""S4-preparation batches (telemetry validation only; no controller logic).

s4tel: per-UE service telemetry vs delivered application goodput, on HELD-OUT seeds (101-102) never used for
calibration (1-15) or the G2-MX grid (1-5). The 4 operating-scenario cells (0.45/0.55 ISD x rho_B 0.1/0.5) x
rho_A {0.5, 0.9, 1.2} x {no action, scripted handover of UE 3 at 15 s}. 30 s runs, W = T_report = 0.5 s.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from batch import run_batch  # noqa: E402

LAY = {"x45": "L-cal-r1.csv", "x55": "L-cal-r1-e55.csv"}


def s4tel():
    cases = []
    for x, lay in LAY.items():
        for ra in (0.51, 0.92, 1.23):
            for rb in (0.29, 1.43):
                for act in ("none", "ho"):
                    for s in (101, 102):
                        a = ["--assign-streams=0", f"--layout-file={os.path.join(REPO, 'configs/layouts', lay)}",
                             f"--rate-a={ra}", f"--rate-b={rb}", "--sim-time=30s", "--window=500ms",
                             "--lm=noop", "--telemetry=true", "--telemetry-interval=0.5", "--e2-send-interval=0.5",
                             "--ue-service-report=true", "--proc-delay=0", "--max-wait-time=0", f"--RngRun={s}"]
                        if act == "ho":
                            a += ["--ho-time=15", "--ho-ue=3", "--ho-target-cell=2"]
                        cases.append((f"tel_{x}_a{ra}_b{rb}_{act}_s{s}", "S4-telemetry", x, a))
    return cases


XAPP = ["--assign-streams=0", "--sim-time=30s", "--window=500ms", "--telemetry=true", "--telemetry-interval=0.5",
        "--e2-send-interval=0.5", "--ue-service-report=true", "--proc-delay=0", "--max-wait-time=0",
        "--lm-query-interval=500ms"]


def scen(x, ra, rb):
    return [f"--layout-file={os.path.join(REPO, 'configs/layouts', LAY[x])}", f"--rate-a={ra}", f"--rate-b={rb}"]


def s4cal():
    """TPC calibration (power-only, TPC xApp alone) on CALIBRATION seed 1: low-load threshold x step."""
    c = []
    for x in LAY:
        for ra in (0.51, 0.92, 1.23):
            for rb in (0.29, 1.43):
                for pl in (0.3, 0.5, 0.7):
                    for st in (3, 6):
                        a = XAPP + scen(x, ra, rb) + ["--lm=tpc", f"--ns3::OranLmXappTpc::PrbLow={pl}",
                                                      f"--ns3::OranLmXappTpc::StepDb={st}", "--RngRun=1"]
                        c.append((f"cal_{x}_a{ra}_b{rb}_pl{pl}_st{st}", "S4-tpc-calibration", x, a))
    return c


def s4pilot():
    """Paired independent-controller pilots on NEW seeds 201-202: B0 (noop), B1 (TS only), B2 (TPC only)."""
    c = []
    for x in LAY:
        for ra in (0.51, 0.92, 1.23):
            for rb in (0.29, 1.43):
                for ctl, lm in (("b0", "noop"), ("b1", "ts"), ("b2", "tpc")):
                    for s in (201, 202):
                        c.append((f"pil_{x}_a{ra}_b{rb}_{ctl}_s{s}", "S4-pilot", x,
                                  XAPP + scen(x, ra, rb) + [f"--lm={lm}", f"--RngRun={s}"]))
    return c


ATTACH = f"--attach-layout-file={os.path.join(REPO, 'configs/layouts', 'L-cal-r1.csv')}"


def s5attach():
    """Initial-attachment check at 0.55 ISD: legacy procedure vs retained-connection procedure, seeds 1-30, 4 s."""
    c = []
    for proc in ("legacy", "retained"):
        for rb in (0.29, 1.43):
            for s in range(1, 31):
                a = XAPP[:1] + XAPP[2:] + ["--sim-time=4s"] + scen("x55", 0.92, rb) + ["--lm=noop", f"--RngRun={s}"]
                if proc == "retained":
                    a += [ATTACH, "--attach-move-time=1.0"]
                c.append((f"att_{proc}_b{rb}_s{s}", "S5-attach", "x55", a))
    return c


def s5joint():
    """Joint-action opportunity diagnostic (no arbitration): both xApps decide on the same telemetry; dual-exec none
    (both shadow), ts (TS executes, TPC shadows), tpc (TPC executes, TS shadows). Retained-connection procedure for
    every scenario (no move at 0.45 ISD). Pilot seeds 201-202."""
    c = []
    for x in LAY:
        for ra in (0.51, 0.92, 1.23):
            for rb in (0.29, 1.43):
                for ex in ("none", "ts", "tpc"):
                    for s in (201, 202):
                        c.append((f"jnt_{x}_a{ra}_b{rb}_{ex}_s{s}", "S5-joint-diagnostic", x,
                                  XAPP + scen(x, ra, rb) + [ATTACH, "--attach-move-time=1.0", "--lm=dual",
                                                           f"--dual-exec={ex}", f"--RngRun={s}"]))
    return c


def s5pilot():
    """Short paired network pilots for B3 / B4 (approved 2026-11-23): both xApps execute (dual-exec=both) under the
    coordination CMM. Joint class (both-shadow opportunity class 'joint'): x45 rho_B 0.1 L3, x55 rho_B 0.1 L2 and L3,
    modes b3, b3 reversed, b4-ts, b4-tpc. Single-controller checks: x45 rho_B 0.5 L3 (TS only) and x45 rho_B 0.1 L1
    (TPC only), modes b3, b4-ts, b4-tpc. Seeds 201-202 and the retained-connection procedure, as s5joint (which
    provides the paired both-shadow / TS-only / TPC-only runs)."""
    c = []
    joint = [("x45", 1.23, 0.29), ("x55", 0.92, 0.29), ("x55", 1.23, 0.29)]
    single = [("x45", 1.23, 1.43), ("x45", 0.51, 0.29)]
    modes = {"b3": ["--coord=b3"], "b3rev": ["--coord=b3", "--coord-reverse=true"], "b4ts": ["--coord=b4-ts"],
             "b4tpc": ["--coord=b4-tpc"]}
    for cond, ms in ((joint, ("b3", "b3rev", "b4ts", "b4tpc")), (single, ("b3", "b4ts", "b4tpc"))):
        for x, ra, rb in cond:
            for m in ms:
                for s in (201, 202):
                    c.append((f"cp_{x}_a{ra}_b{rb}_{m}_s{s}", "S5-coord-pilot", x,
                              XAPP + scen(x, ra, rb) + [ATTACH, "--attach-move-time=1.0", "--lm=dual",
                                                       "--dual-exec=both", *modes[m], f"--RngRun={s}"]))
    return c


if __name__ == "__main__":
    which = sys.argv[1]
    jobs = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    run_batch(which, {"s4tel": s4tel, "s4cal": s4cal, "s4pilot": s4pilot, "s5attach": s5attach, "s5joint": s5joint,
                       "s5pilot": s5pilot}[which](), jobs=jobs, stage="s4",
              data_root=os.path.expanduser("~/xapp-research-data/s4"))
