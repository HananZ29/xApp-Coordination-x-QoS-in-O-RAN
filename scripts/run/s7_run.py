#!/usr/bin/env python3
"""S7 confirmatory matrix executor (G6 approved 2026-11-24; design frozen in docs/methodology/G6-freeze-proposal.md
and docs/methodology/S7-registry.md).

Design (12 conditions x seeds 1001-1013; common random numbers = the same --RngRun across arms):
  D0     B0, B1, B2, B3, b4ts, b4tpc, b5, b4sats, b4satpc, b5sa          (all timing components 0)
  D1     B0, B1, B2, B3, B3h, 6 coordinated arms                           (E2 2 ms each way, proc 12 us,
                                                                            coordination hold 131 us; B3 hold 0)
  D1dk   B3h, 6 coordinated arms, hold 131 us + Delta_k, Delta = 50/125/250 ms (k = 1..3)
  C3     D1 + cell-A load step 0.51 -> 1.23 Mbps at t = 15 s; 4 scenarios (x45/x55 x rate_b 0.29/1.43);
         B3, B3h, b4sats, b4satpc, b5sa
  S8a    D0, B3 with reversed command order (--coord-reverse), 12 conditions
  B3h = B3 (uncoordinated) carrying the same coordination hold as the coordinated arms (equal-delay control).

Execution rules (approved; exact identity of the 16 output files in IDF):
  Block 1 (seed 1001) simulates EVERY run and verifies every rule per (configuration, detector family):
    LZ  (U-LZ at D0, U-LZh at D1/D1dk/C3): an arm equals its reference (B3 at D0, B3h elsewhere) whenever the
        reference shows no flag under the arm's detector (same_cycle: b4ts/b4tpc/b5; combined: sa arms).
    EH  (fallback if LZ has a counter-example): unflagged arms of one family equal the family leader
        (b4ts / b4sats) at equal hold.
    FULL (fallback if EH also fails): every arm simulated.
    U-B0: B0@D1 equals B0@D0 on the U0 files (verified on the U0 set at E7 + K100 before).
  Seeds 1002-1013: phase A = always-simulated runs (+ family leaders under EH, + all arms under FULL);
  phase B = conditional arms, simulated only if their reference / leader flags (or the reference failed);
  otherwise a DERIVED record (results/s7/derived.jsonl) naming the source run and the sha256 of its files.
  No output is copied. Waves pipeline phase B of seed s with phase A of seed s+1.
Failures: every failed run is retried once in a separate batch (the failed outputs are kept); still-failed
references make all their conditional arms simulated.

Usage: s7_run.py plan | run [--jobs 7]
"""
import datetime as dt
import filecmp
import hashlib
import json
import os
import subprocess
import sys

import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import s5_batches as S  # noqa: E402
from batch import run_batch  # noqa: E402

SEEDS = list(range(1001, 1014))
E2_S, PROC_S, HOLD_S = 0.002, 0.000012, 0.000131
DELTAS = {"D1d1": 0.05, "D1d2": 0.125, "D1d3": 0.25}
STEP = ["--rate-step-time=15", "--rate-a-after=1.23"]
C3_CONDS = [(x, 0.51, rb) for x in ("x45", "x55") for rb in (0.29, 1.43)]
COORD = {"b4ts": "b4-ts", "b4tpc": "b4-tpc", "b5": "b5", "b4sats": "b4sa-ts", "b4satpc": "b4sa-tpc", "b5sa": "b5sa"}
DET = {"b4ts": "same_cycle", "b4tpc": "same_cycle", "b5": "same_cycle",
       "b4sats": "combined", "b4satpc": "combined", "b5sa": "combined"}
LEADER = {"same_cycle": "b4ts", "combined": "b4sats"}
FAM = {"same_cycle": ["b4ts", "b4tpc", "b5"], "combined": ["b4sats", "b4satpc", "b5sa"]}
IDF = ["backlog", "ctrl", "flows", "flowsum", "ho", "interact", "meas", "phypower", "power", "prb", "radio",
       "telemetry", "tput", "ues", "ulsinr", "xappdec"]
U0 = ["tput", "ho", "power", "phypower", "flowsum", "prb", "radio", "flows", "ues", "xappdec"]
OUT = os.path.join(REPO, "results", "s7")
DATA = os.path.expanduser("~/xapp-research-data/s7")


def hold(cfg):
    return {"D0": 0.0, "S8a": 0.0, "D1": HOLD_S, "C3": HOLD_S}.get(cfg, HOLD_S + DELTAS.get(cfg, 0.0))


def args(cfg, x, ra, rb, arm, s):
    a = list(S.XAPP) + S.scen(x, ra, rb) + [S.ATTACH, "--attach-move-time=1.0", "--lm=dual"]
    if arm in ("B0", "B1", "B2"):
        a += [f"--dual-exec={ {'B0': 'none', 'B1': 'ts', 'B2': 'tpc'}[arm] }", f"--RngRun={s}"] + S.CA
    else:
        mode = "b3" if arm in ("B3", "B3h", "B3rev") else COORD[arm]
        a += ["--dual-exec=both", f"--coord={mode}", f"--RngRun={s}", "--interact-log=true"] + S.CA
        if arm == "B3rev":
            a.append("--coord-reverse=true")
    if cfg not in ("D0", "S8a"):  # D1 timing: E2 2 ms up (send-phase anchored) / 2 ms down, LM processing 12 us
        a = [f"--proc-delay={PROC_S:.6f}" if v.startswith("--proc-delay") else v for v in a]
        a += [S.E2UP.format(E2_S), f"--e2-cmd-delay={E2_S}"]
    if arm not in ("B0", "B1", "B2", "B3", "B3rev") and hold(cfg) > 0:
        a.append(f"--coord-delay={hold(cfg):.6f}")
    if cfg == "C3":
        a += STEP
    return a


def rid(cfg, x, ra, rb, arm, s):
    return f"s7_{cfg}_{x}_a{ra}_b{rb}_{arm}_s{s}"


def cells():
    """Every (cfg, x, ra, rb, arm) cell of the design, with its rule role."""
    out = []
    for x, ra, rb in S.CONDS:
        for arm in ["B0", "B1", "B2", "B3"] + list(COORD):
            out.append(("D0", x, ra, rb, arm))
        for arm in ["B0", "B1", "B2", "B3", "B3h"] + list(COORD):
            out.append(("D1", x, ra, rb, arm))
        for cfg in DELTAS:
            for arm in ["B3h"] + list(COORD):
                out.append((cfg, x, ra, rb, arm))
        out.append(("S8a", x, ra, rb, "B3rev"))
    for x, ra, rb in C3_CONDS:
        for arm in ["B3", "B3h", "b4sats", "b4satpc", "b5sa"]:
            out.append(("C3", x, ra, rb, arm))
    return out


def ref_arm(cfg):
    return "B3" if cfg == "D0" else "B3h"


def meta(c, s, kind):
    cfg, x, ra, rb, arm = c
    return dict(run_id=rid(cfg, x, ra, rb, arm, s), cfg=cfg, pos=x, rate_a=ra, rate_b=rb, arm=arm, seed=s, kind=kind)


def phase_a(s, rules, ub0):
    A = []
    for c in cells():
        cfg, x, ra, rb, arm = c
        if arm in COORD:
            r = rules[f"{cfg}:{DET[arm]}"]
            if r == "FULL" or (r == "EH" and arm == LEADER[DET[arm]]):
                A.append(meta(c, s, f"A:{r}"))
        elif arm == "B0" and cfg == "D1":
            if not ub0:
                A.append(meta(c, s, "A:U-B0 failed"))
        else:
            A.append(meta(c, s, "A"))
    return A


# ---------------------------------------------------------------- output inspection
def flagged(prefix, det):
    f = prefix + "-interact.csv"
    if not os.path.exists(f):
        return None
    I = pd.read_csv(f)
    return bool(len(I) and ((I.definition == det) & (I.flagged.astype(int) == 1)).any())


def identical(p1, p2, files=IDF):
    for k in files:
        a, b = f"{p1}-{k}.csv", f"{p2}-{k}.csv"
        if os.path.exists(a) != os.path.exists(b):
            return False
        if os.path.exists(a) and not filecmp.cmp(a, b, shallow=False):
            return False
    return True


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


# ---------------------------------------------------------------- state
def load_state():
    p = os.path.join(OUT, "state.json")
    return json.load(open(p)) if os.path.exists(p) else {"index": {}, "waves": [], "rules": None, "ub0": None}


def save_state(st):
    os.makedirs(OUT, exist_ok=True)
    tmp = os.path.join(OUT, "state.json.tmp")
    json.dump(st, open(tmp, "w"), indent=1)
    os.replace(tmp, os.path.join(OUT, "state.json"))


def prefix(st, r):
    e = st["index"].get(r)
    return os.path.join(e["data"], r) if e and e["status"] == "completed" else None


def log(msg):
    line = f"{dt.datetime.now().strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(os.path.join(OUT, "s7_run.log"), "a") as f:
        f.write(line + "\n")


def run_wave(st, name, metas, jobs):
    """Run one batch; retry failures once in '<name>r'; record every run in the index."""
    if not metas:
        return
    with open(os.path.join(OUT, "plan.jsonl"), "a") as f:
        for m in metas:
            f.write(json.dumps(dict(m, wave=name)) + "\n")
    M = {m["run_id"]: m for m in metas}
    cases = [(m["run_id"], "S7-confirmatory", m["cfg"], args(m["cfg"], m["pos"], m["rate_a"], m["rate_b"], m["arm"], m["seed"]))
             for m in metas]
    t0 = dt.datetime.now()
    d, repo, mans = run_batch(name, cases, jobs=jobs, stage="s7", data_root=DATA)
    for r, man in mans.items():
        st["index"][r] = dict(M[r], data=d, status=man["status"], batch=name)
    failed = [r for r, man in mans.items() if man["status"] != "completed"]
    log(f"wave {name}: {len(cases)} runs, {len(failed)} failed, {(dt.datetime.now() - t0).total_seconds() / 60:.1f} min")
    if failed:
        with open(os.path.join(OUT, "retries.jsonl"), "a") as f:
            for r in failed:
                f.write(json.dumps(dict(run_id=r, first_batch=name, first_data=d, status=mans[r]["status"])) + "\n")
        d2, _, mans2 = run_batch(name + "r", [c for c in cases if c[0] in failed], jobs=jobs, stage="s7", data_root=DATA)
        for r, man in mans2.items():
            st["index"][r] = dict(M[r], data=d2, status=man["status"], batch=name + "r", retried=True)
        log(f"retry {name}r: {sum(m['status'] == 'completed' for m in mans2.values())}/{len(failed)} completed")
    st["waves"].append(name)
    save_state(st)


# ---------------------------------------------------------------- block 1 verification
def check_block1(st, s=1001):
    rows, rules = [], {}
    for cfg in ["D0", "D1", *DELTAS, "C3"]:
        conds = C3_CONDS if cfg == "C3" else S.CONDS
        for det, fam in FAM.items():
            if cfg == "C3" and det == "same_cycle":
                continue
            lz_n = lz_bad = eh_n = eh_bad = missing = 0
            for x, ra, rb in conds:
                ref = prefix(st, rid(cfg, x, ra, rb, ref_arm(cfg), s))
                lead = prefix(st, rid(cfg, x, ra, rb, LEADER[det], s))
                for arm in fam:
                    p = prefix(st, rid(cfg, x, ra, rb, arm, s))
                    if p is None or ref is None or lead is None:
                        missing += 1
                        continue
                    if flagged(ref, det) is False:
                        lz_n += 1
                        ok = identical(p, ref)
                        lz_bad += not ok
                        rows.append(dict(cfg=cfg, det=det, rule="LZ", cond=f"{x}_{ra}_{rb}", arm=arm, identical=ok))
                    if arm != LEADER[det] and flagged(lead, det) is False:
                        eh_n += 1
                        ok = identical(p, lead)
                        eh_bad += not ok
                        rows.append(dict(cfg=cfg, det=det, rule="EH", cond=f"{x}_{ra}_{rb}", arm=arm, identical=ok))
            rule = "LZ" if lz_bad == 0 and missing == 0 else ("EH" if eh_bad == 0 and missing == 0 else "FULL")
            rules[f"{cfg}:{det}"] = rule
            log(f"block1 {cfg}:{det}: LZ {lz_n - lz_bad}/{lz_n} identical, EH {eh_n - eh_bad}/{eh_n}, missing {missing} -> {rule}")
    ub_n = ub_bad = 0
    for x, ra, rb in S.CONDS:
        a, b = prefix(st, rid("D0", x, ra, rb, "B0", s)), prefix(st, rid("D1", x, ra, rb, "B0", s))
        if a is None or b is None:
            ub_bad += 1
            continue
        ub_n += 1
        ok = identical(a, b, U0)
        ub_bad += not ok
        rows.append(dict(cfg="D1", det="-", rule="U-B0", cond=f"{x}_{ra}_{rb}", arm="B0", identical=ok))
    ub0 = ub_bad == 0
    log(f"block1 U-B0: {ub_n - ub_bad}/{ub_n} identical -> {'derive B0@D1' if ub0 else 'simulate B0@D1'}")
    pd.DataFrame(rows).to_csv(os.path.join(OUT, "block1_rule_checks.csv"), index=False)
    json.dump(dict(rules=rules, ub0=ub0), open(os.path.join(OUT, "block1_rules.json"), "w"), indent=1)
    return rules, ub0


# ---------------------------------------------------------------- phase B
def phase_b(st, s):
    """Conditional arms of seed s (needs its phase A outputs). Returns metas to simulate; writes derived records."""
    sim, der = [], []
    for c in cells():
        cfg, x, ra, rb, arm = c
        m = meta(c, s, "B")
        if arm == "B0" and cfg == "D1" and st["ub0"]:
            src = rid("D0", x, ra, rb, "B0", s)
            if prefix(st, src) is None:
                sim.append(dict(m, kind="B:U-B0 source failed"))
            else:
                der.append((m, "U-B0", src, U0))
            continue
        if arm not in COORD:
            continue
        r = st["rules"][f"{cfg}:{DET[arm]}"]
        if r == "FULL" or (r == "EH" and arm == LEADER[DET[arm]]):
            continue  # simulated in phase A
        src = rid(cfg, x, ra, rb, ref_arm(cfg) if r == "LZ" else LEADER[DET[arm]], s)
        p = prefix(st, src)
        f = flagged(p, DET[arm]) if p else None
        if f is False:
            der.append((m, "U-LZ" if (r == "LZ" and cfg == "D0") else ("U-LZh" if r == "LZ" else "U-EH"), src, IDF))
        else:
            sim.append(dict(m, kind=f"B:{r}:{'flagged' if f else 'source missing'}"))
    with open(os.path.join(OUT, "derived.jsonl"), "a") as fh:
        for m, rule, src, files in der:
            e = st["index"][src]
            rec = dict(m, status="derived", rule=rule, source_run_id=src, source_data=e["data"], source_batch=e["batch"],
                       files=files, source_sha256={k: sha(os.path.join(e["data"], f"{src}-{k}.csv"))
                                                   for k in files if os.path.exists(os.path.join(e["data"], f"{src}-{k}.csv"))})
            fh.write(json.dumps(rec) + "\n")
            st["index"][m["run_id"]] = dict(m, data=e["data"], status="derived", source_run_id=src, rule=rule)
    save_state(st)
    log(f"seed {s} phase B: {len(sim)} conditional runs to simulate, {len(der)} derived")
    return sim


def analyse_async():
    """Registered analysis on the completed seed blocks (interim outputs labelled); never blocks execution."""
    a = os.path.join(REPO, "analysis", "s7", "s7_analysis.py")
    if os.path.exists(a):
        subprocess.Popen(["nice", "-n", "10", sys.executable, a], cwd=REPO,
                         stdout=open(os.path.join(OUT, "analysis_last.log"), "w"), stderr=subprocess.STDOUT)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "plan"
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 7
    if cmd == "plan":
        n1 = len(cells())
        print(f"block 1: {n1} runs; later seeds, phase A always (all rules verified): "
              f"{len(phase_a(1002, {f'{c}:{d}': 'LZ' for c in ['D0', 'D1', *DELTAS, 'C3'] for d in FAM}, True))}")
        return
    os.makedirs(OUT, exist_ok=True)
    st = load_state()
    if "s7b1001" not in st["waves"]:
        log("S7 start: block 1 (seed 1001), every run simulated")
        run_wave(st, "s7b1001", [meta(c, 1001, "block1") for c in cells()], jobs)
    if st["rules"] is None:
        st["rules"], st["ub0"] = check_block1(st)
        save_state(st)
        analyse_async()
    prev = None
    for s in SEEDS[1:]:
        name = f"s7w{s}"
        if name in st["waves"]:
            prev = s
            continue
        metas = phase_a(s, st["rules"], st["ub0"]) + (phase_b(st, prev) if prev else [])
        run_wave(st, name, metas, jobs)
        analyse_async()
        prev = s
    if "s7wfinal" not in st["waves"]:
        run_wave(st, "s7wfinal", phase_b(st, SEEDS[-1]), jobs)
        st["waves"].append("s7wfinal")
        save_state(st)
    log("S7 ALL DONE")
    analyse_async()


if __name__ == "__main__":
    main()
