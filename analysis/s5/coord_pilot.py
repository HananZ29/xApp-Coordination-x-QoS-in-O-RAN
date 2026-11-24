#!/usr/bin/env python3
"""S5 part 1: B3 / B4 short paired network pilots (s5pilot), paired with s5joint (same seeds, conditions, attachment):
none = both shadow (no control), ts = TS only executes, tpc = TPC only executes.
Outputs: results/s5/analysis/coord_pilot_runs.csv, coord_pilot_paired.csv, coord_pilot_consistency.csv
"""
import glob
import json
import os
import sys

import numpy as np
import pandas as pd

REPO = os.path.expanduser("~/ns-3.42/scratch/xapp-coord")
sys.path.insert(0, os.path.join(REPO, "analysis", "s4"))
import s4_pilot as P  # noqa: E402

OUT = os.path.join(REPO, "results", "s5", "analysis")


def batch(name):
    r = sorted(glob.glob(os.path.join(REPO, "results", "s4", name, "*")))[-1]
    return json.load(open(os.path.join(r, "BATCH.json")))["external_data"], [json.loads(l) for l in open(os.path.join(r, "manifests.jsonl"))]


def parse(rid):
    p = rid.split("_")
    return dict(pos=p[1], rate_a=float(p[2][1:]), rate_b=float(p[3][1:]), mode=p[4], seed=int(p[5][1:]))


def row(d, rid, k):
    P.parse = lambda r: k  # run_row only uses parse() for its key fields
    r = P.run_row(d, rid)
    L = pd.read_csv(os.path.join(d, f"{rid}-cmdlife.csv"), keep_default_na=False)
    det = L[L.stage == "detector"]
    r["pairs_flagged"] = int((det.outcome == "potential_conflict").sum() / 2)
    r["pairs_unflagged"] = int((det.outcome == "not_flagged").sum() / 2)
    F = L[L.stage == "final"]
    for o in ("b4_priority_ts", "b4_priority_tpc", "ue_identity_changed", "ue_context_absent"):
        r[f"rej_{o}"] = int((F.reason == o).sum())
    r["rej_enb_state"] = int(F.reason.str.startswith("enb_state").sum())
    r["ts_executed"] = int(((F.cmd_type == "handover") & (F.outcome == "completed")).sum())
    r["tpc_effective"] = int(((F.cmd_type == "tx_power") & (F.outcome == "effect_observed")).sum())
    # first flagged cycle and physical-effect order in it
    fl = L[(L.stage == "detector") & (L.outcome == "potential_conflict")]
    r["first_conflict_s"] = float(fl.time_s.min()) if len(fl) else np.nan
    return r


def main():
    os.makedirs(OUT, exist_ok=True)
    rows = []
    d, mans = batch("s5pilot")
    for m in mans:
        k = parse(m["run_id"])
        rows.append(row(d, m["run_id"], k))
    dj, mj = batch("s5joint")
    dr, mr = batch("s5joint_rerun")
    rr = {m["run_id"] for m in mr}
    for m in mj:
        p = m["run_id"].split("_")
        k = dict(pos=p[1], rate_a=float(p[2][1:]), rate_b=float(p[3][1:]), mode={"none": "B0shadow", "ts": "B1", "tpc": "B2"}[p[4]], seed=int(p[5][1:]))
        rows.append(row(dr if m["run_id"] in rr else dj, m["run_id"], k))
    R = pd.DataFrame(rows)
    R["etx"] = R.etx_c1_Ws + R.etx_c2_Ws
    R.to_csv(os.path.join(OUT, "coord_pilot_runs.csv"), index=False)
    cond = ["pos", "rate_a", "rate_b"]
    keep = R[R.set_index(cond).index.isin(R[R["mode"] == "b3"].set_index(cond).index)]
    g = keep.groupby(cond + ["mode"]).agg(
        seeds=("seed", "count"), flagged=("pairs_flagged", "sum"), ts_exec=("ts_executed", "sum"), tpc_eff=("tpc_effective", "sum"),
        rej_b4ts=("rej_b4_priority_ts", "sum"), rej_b4tpc=("rej_b4_priority_tpc", "sum"), rej_state=("rej_enb_state", "sum"),
        rlf=("rlf", "sum"), ho_rev=("ho_reversals", "sum"), tpc_restore=("tpc_restore", "sum"),
        goodput=("mean_goodput", "mean"), min_frac=("min_user_frac", "mean"), viol=("viol", "mean"), viol_edge=("viol_edge", "mean"),
        etx_c2=("etx_c2_Ws", "mean")).round(3)
    g.to_csv(os.path.join(OUT, "coord_pilot_paired.csv"))
    # consistency: before the first flagged cycle, the xApps' decision logs are identical across modes (same telemetry)
    cs = []
    for key, x in R[R["mode"].isin(["b3", "b3rev", "b4ts", "b4tpc"])].groupby(cond + ["seed"]):
        t0 = x.first_conflict_s.min()
        logs = {}
        for r in x.itertuples():
            dd = d
            X = pd.read_csv(os.path.join(dd, f"{r.run}-xappdec.csv"), keep_default_na=False)
            lim = t0 if not np.isnan(t0) else 1e9
            logs[r.mode] = X[X.time_s <= lim + 1e-9].to_csv(index=False)
        same = len(set(logs.values())) == 1
        cs.append(dict(zip(cond + ["seed"], key), first_conflict_s=t0, modes=len(logs), identical_up_to_first_conflict=same))
    CS = pd.DataFrame(cs)
    CS.to_csv(os.path.join(OUT, "coord_pilot_consistency.csv"), index=False)
    pd.set_option("display.width", 250)
    print(g.to_string())
    print(CS.to_string(index=False))


if __name__ == "__main__":
    main()
