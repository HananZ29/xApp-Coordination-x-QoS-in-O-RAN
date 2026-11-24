#!/usr/bin/env python3
"""S8 executor (registry docs/methodology/S8-registry.md; approved by Hanan 2026-11-24).

E1  event-enriched conditional study at D1 (S7 timing), conditions x45 1.23/0.29 and x55 0.92/0.29 (selected from S7
    joint-activity evidence; disclosed). Seeds 2001-2430 in blocks of 20. Per (condition, seed): B3@D1 and B3h@D1 are
    ALWAYS simulated and kept. EVENT = the B3@D1 run's interact.csv has >= 1 'combined' flagged row (reference
    trajectory only). Arms b4sats, b4satpc, b5sa are simulated when B3 or B3h is combined-flagged (B3 == B3h flags in
    156/156 S7 clusters) and for every cluster of the verification block (seeds 2001-2020); otherwise a derived record
    (LZh: arm == B3h, verified in S7 and re-verified on the verification block) is written. Stopping: no new block is
    launched once the event count over completed blocks is >= 150, or after seed 2430. Outcome-blind.
E2  TPC sensitivity at D1: variants V0 (24 dBm / 3 dB), V1 (MinDbm 27), V2 (StepDb 6) via ns-3 attribute defaults;
    6 conditions rate_b 0.29; seeds 3001-3026 shared across variants; B0, B2, B3, B3h always simulated; arms when B3h is
    combined-flagged or seed 3001 (verification); otherwise derived (LZh). Engineering check first (seed 3999, B2,
    x45 0.51/0.29, all variants; excluded from analysis): power.csv must show the variant's floor and step.
Every wave goes through batch.run_batch (manifests, SHA256SUMS, dirty-tree refusal); one retry per failed run.
Usage: s8_run.py e1|e2 [--jobs 7]
"""
import datetime as dt
import hashlib
import json
import os
import sys

import pandas as pd

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import s7_run as R  # noqa: E402  (frozen S7 argument builder, flag reader, identity check)
from batch import run_batch  # noqa: E402

OUT = os.path.join(REPO, "results", "s8")
DATA = os.path.expanduser("~/xapp-research-data/s8")
ARMS = ["b4sats", "b4satpc", "b5sa"]
E1_CONDS = [("x45", 1.23, 0.29), ("x55", 0.92, 0.29)]
E1_SEEDS = list(range(2001, 2431))
E1_BLOCK = 20
E1_TARGET = 150
E1_VERIFY = set(range(2001, 2021))
E2_CONDS = [(x, ra, 0.29) for x in ("x45", "x55") for ra in (0.51, 0.92, 1.23)]
E2_SEEDS = list(range(3001, 3027))
E2_VERIFY = {3001}
E2_GROUP = 4
VARIANTS = {"V0": [], "V1": ["--ns3::OranLmXappTpc::MinDbm=27"], "V2": ["--ns3::OranLmXappTpc::StepDb=6"]}
E2_REFS = ["B0", "B2", "B3", "B3h"]


def rid(exp, var, x, ra, rb, arm, s):
    return f"s8{exp}_{var}_{x}_a{ra}_b{rb}_{arm}_s{s}"


def meta(exp, var, x, ra, rb, arm, s, kind):
    return dict(run_id=rid(exp, var, x, ra, rb, arm, s), exp=exp, variant=var, cfg="D1", pos=x, rate_a=ra, rate_b=rb,
                arm=arm, seed=s, kind=kind)


def argv(m):
    return R.args("D1", m["pos"], m["rate_a"], m["rate_b"], m["arm"], m["seed"]) + VARIANTS.get(m["variant"], [])


class Exp:
    def __init__(self, exp):
        self.exp = exp
        self.dir = os.path.join(OUT, exp)
        os.makedirs(self.dir, exist_ok=True)
        p = os.path.join(self.dir, "state.json")
        self.st = json.load(open(p)) if os.path.exists(p) else {"index": {}, "waves": [], "full": {}, "events": 0,
                                                                "blocks_done": [], "pending": []}

    def save(self):
        tmp = os.path.join(self.dir, "state.json.tmp")
        json.dump(self.st, open(tmp, "w"), indent=1)
        os.replace(tmp, os.path.join(self.dir, "state.json"))

    def log(self, msg):
        line = f"{dt.datetime.now().strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        with open(os.path.join(self.dir, "run.log"), "a") as f:
            f.write(line + "\n")

    def prefix(self, r):
        e = self.st["index"].get(r)
        return os.path.join(e["data"], r) if e and e["status"] == "completed" else None

    def wave(self, name, metas, jobs):
        if not metas:
            return
        with open(os.path.join(self.dir, "plan.jsonl"), "a") as f:
            for m in metas:
                f.write(json.dumps(dict(m, wave=name)) + "\n")
        M = {m["run_id"]: m for m in metas}
        cases = [(m["run_id"], f"S8-{self.exp}", m["variant"], argv(m)) for m in metas]
        t0 = dt.datetime.now()
        d, _, mans = run_batch(name, cases, jobs=jobs, stage="s8", data_root=DATA)
        for r, man in mans.items():
            self.st["index"][r] = dict(M[r], data=d, status=man["status"], batch=name)
        failed = [r for r, man in mans.items() if man["status"] != "completed"]
        self.log(f"wave {name}: {len(cases)} runs, {len(failed)} failed, {(dt.datetime.now() - t0).total_seconds() / 60:.1f} min")
        if failed:
            with open(os.path.join(self.dir, "retries.jsonl"), "a") as f:
                for r in failed:
                    f.write(json.dumps(dict(run_id=r, first_batch=name, first_data=d, status=mans[r]["status"])) + "\n")
            d2, _, m2 = run_batch(name + "r", [c for c in cases if c[0] in failed], jobs=jobs, stage="s8", data_root=DATA)
            for r, man in m2.items():
                self.st["index"][r] = dict(M[r], data=d2, status=man["status"], batch=name + "r", retried=True)
            self.log(f"retry {name}r: {sum(m['status'] == 'completed' for m in m2.values())}/{len(failed)} completed")
        self.st["waves"].append(name)
        self.save()

    def derive(self, m, src):
        e = self.st["index"][src]
        rec = dict(m, status="derived", rule="U-LZh", source_run_id=src, source_data=e["data"], source_batch=e["batch"],
                   files=R.IDF, source_sha256={k: R.sha(os.path.join(e["data"], f"{src}-{k}.csv")) for k in R.IDF
                                                if os.path.exists(os.path.join(e["data"], f"{src}-{k}.csv"))})
        with open(os.path.join(self.dir, "derived.jsonl"), "a") as fh:
            fh.write(json.dumps(rec) + "\n")
        self.st["index"][m["run_id"]] = dict(m, data=e["data"], status="derived", source_run_id=src, rule="U-LZh")

    def verify(self, var, seeds, conds):
        """LZh check on fully simulated clusters: unflagged B3h => arm identical to B3h (16-file set)."""
        n = bad = 0
        rows = []
        for s in seeds:
            for x, ra, rb in conds:
                ref = self.prefix(rid(self.exp, var, x, ra, rb, "B3h", s))
                if ref is None or R.flagged(ref, "combined") is not False:
                    continue
                for a in ARMS:
                    p = self.prefix(rid(self.exp, var, x, ra, rb, a, s))
                    if p is None:
                        continue
                    ok = R.identical(p, ref)
                    n += 1
                    bad += not ok
                    rows.append(dict(variant=var, seed=s, pos=x, rate_a=ra, rate_b=rb, arm=a, identical=ok))
        pd.DataFrame(rows).to_csv(os.path.join(self.dir, f"verify_{var}.csv"), index=False)
        self.st["full"][var] = bad > 0 or n == 0
        self.log(f"verification {var}: LZh {n - bad}/{n} identical -> {'FULL (simulate all arms)' if self.st['full'][var] else 'derive unflagged arms'}")
        self.save()


# ---------------------------------------------------------------- E1
def e1(jobs):
    E = Exp("e1")
    st = E.st
    blocks = [E1_SEEDS[i:i + E1_BLOCK] for i in range(0, len(E1_SEEDS), E1_BLOCK)]
    for bi, block in enumerate(blocks):
        name = f"s8e1b{bi + 1:02d}"
        if name in st["waves"]:
            continue
        if st["events"] >= E1_TARGET:
            break
        refs = [meta("e1", "D1", x, ra, rb, a, s, "reference") for s in block for (x, ra, rb) in E1_CONDS for a in ("B3", "B3h")]
        pend = [m for m in st["pending"] if st["index"].get(m["run_id"]) is None]
        E.wave(name, refs + pend, jobs)
        st["pending"] = []
        if bi == 1 and "D1" not in st["full"]:  # block 1's arms ran in this wave
            E.verify("D1", sorted(E1_VERIFY), E1_CONDS)
        new_pend, ev_rows = [], []
        for s in block:
            for x, ra, rb in E1_CONDS:
                p3, ph = E.prefix(rid("e1", "D1", x, ra, rb, "B3", s)), E.prefix(rid("e1", "D1", x, ra, rb, "B3h", s))
                f3 = R.flagged(p3, "combined") if p3 else None
                fh = R.flagged(ph, "combined") if ph else None
                ev_rows.append(dict(block=bi + 1, seed=s, pos=x, rate_a=ra, rate_b=rb, b3_flag=f3, b3h_flag=fh,
                                    event=bool(f3), ref_failed=p3 is None or ph is None))
                if p3 is None or ph is None:
                    continue  # no event determinable; failure retained and reported
                need = s in E1_VERIFY or f3 or fh or st["full"].get("D1", False)
                for a in ARMS:
                    m = meta("e1", "D1", x, ra, rb, a, s, "arm")
                    if need:
                        new_pend.append(m)
                    else:
                        E.derive(m, rid("e1", "D1", x, ra, rb, "B3h", s))
        with open(os.path.join(E.dir, "events.jsonl"), "a") as f:
            for r in ev_rows:
                f.write(json.dumps(r) + "\n")
        st["events"] += sum(r["event"] for r in ev_rows)
        st["blocks_done"].append(bi + 1)
        st["pending"] = new_pend
        E.save()
        E.log(f"block {bi + 1} (seeds {block[0]}-{block[-1]}): {sum(r['event'] for r in ev_rows)} events; total {st['events']}; "
              f"{len(new_pend)} arm runs queued")
    if st["pending"]:
        E.wave("s8e1final", st["pending"], jobs)
        if "D1" not in st["full"]:
            E.verify("D1", sorted(E1_VERIFY), E1_CONDS)
        st["pending"] = []
        E.save()
    E.log(f"E1 DONE: events {st['events']} (target {E1_TARGET}); seeds used {len(st['blocks_done']) * E1_BLOCK}; "
          f"{'TARGET MET' if st['events'] >= E1_TARGET else 'SEED CAP REACHED BEFORE TARGET'}")


# ---------------------------------------------------------------- E2
def e2_check(E, jobs):
    if "s8e2chk" in E.st["waves"]:
        return E.st.get("check_ok", False)
    metas = [meta("e2", v, "x45", 0.51, 0.29, "B2", 3999, "engineering_check") for v in VARIANTS]
    E.wave("s8e2chk", metas, jobs)
    ok, notes = True, []
    for v, floor, step in (("V0", 24, 3), ("V1", 27, 3), ("V2", 24, 6)):
        p = E.prefix(rid("e2", v, "x45", 0.51, 0.29, "B2", 3999))
        if p is None:
            ok = False
            notes.append(f"{v}: run failed")
            continue
        pw = pd.read_csv(p + "-power.csv")
        ch = pw[pw.source != "initial"]
        lo = ch.new_dbm.min() if len(ch) else None
        steps = sorted(set((ch.old_dbm - ch.new_dbm).abs().round(3))) if len(ch) else []
        good = len(ch) > 0 and lo >= floor - 1e-6 and lo <= floor + 1e-6 and all(abs(s - step) < 1e-6 for s in steps)
        ok &= good
        notes.append(f"{v}: min {lo} dBm, steps {steps} -> {'OK' if good else 'FAIL'}")
    E.st["check_ok"] = ok
    E.save()
    E.log("E2 engineering check: " + "; ".join(notes))
    return ok


def e2(jobs):
    E = Exp("e2")
    st = E.st
    if not e2_check(E, jobs):
        E.log("E2 BLOCKED: variant configuration not verified; nothing further run")
        return
    groups = [E2_SEEDS[i:i + E2_GROUP] for i in range(0, len(E2_SEEDS), E2_GROUP)]
    for gi, grp in enumerate(groups):
        name = f"s8e2g{gi + 1:02d}"
        if name in st["waves"]:
            continue
        refs = [meta("e2", v, x, ra, rb, a, s, "reference") for v in VARIANTS for s in grp for (x, ra, rb) in E2_CONDS
                for a in E2_REFS]
        pend = [m for m in st["pending"] if st["index"].get(m["run_id"]) is None]
        E.wave(name, refs + pend, jobs)
        st["pending"] = []
        new = []
        if gi == 1:
            for v in VARIANTS:
                if v not in st["full"]:
                    E.verify(v, sorted(E2_VERIFY), E2_CONDS)
            for m in st.get("deferred", []):  # group-1 decisions taken only after the per-variant verification
                if st["full"][m["variant"]]:
                    new.append(m)
                else:
                    E.derive(m, rid("e2", m["variant"], m["pos"], m["rate_a"], m["rate_b"], "B3h", m["seed"]))
            st["deferred"] = []
        for v in VARIANTS:
            for s in grp:
                for x, ra, rb in E2_CONDS:
                    ph = E.prefix(rid("e2", v, x, ra, rb, "B3h", s))
                    if ph is None:
                        continue
                    fh = R.flagged(ph, "combined")
                    need = s in E2_VERIFY or fh or st["full"].get(v, False)
                    for a in ARMS:
                        m = meta("e2", v, x, ra, rb, a, s, "arm")
                        if need:
                            new.append(m)
                        elif v not in st["full"]:
                            st.setdefault("deferred", []).append(m)  # rule not yet verified for this variant
                        else:
                            E.derive(m, rid("e2", v, x, ra, rb, "B3h", s))
        st["pending"] = new
        E.save()
        E.log(f"group {gi + 1} (seeds {grp[0]}-{grp[-1]}): {len(new)} arm runs queued")
    if st["pending"]:
        E.wave("s8e2final", st["pending"], jobs)
        st["pending"] = []
        E.save()
    E.log("E2 DONE")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 7
    {"e1": e1, "e2": e2}[cmd](jobs)


if __name__ == "__main__":
    main()
