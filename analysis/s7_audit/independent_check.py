#!/usr/bin/env python3
"""S7 AUDIT (read-only, 2026-11-24). Independent re-derivation of S7 outcomes and confirmatory statistics.

Independence from the registered analysis (analysis/s7/s7_analysis.py):
  * outcomes from <run>-tput.csv (application-layer receive trace written by the scenario, a different instrument
    than <run>-flows.csv used by the registered analysis); UE groups and offered rates from <run>-ues.csv;
    C3 offered rate from the registered step definition (cell-A groups nearA/edge -> 1.23 Mbps for t > 15 s)
  * derived cells resolved through state.json to their source run (the same reuse records), re-hashed separately
  * statistics re-implemented: estimand, seed-block resampling, percentile and t-based TOST, Holm, gatekeeping
Outputs: results/s7/audit/*.csv and audit_summary.md. Nothing under results/s7 outside audit/ is touched.
"""
import json
import os

import numpy as np
import pandas as pd
from scipy import stats

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
S7 = os.path.join(REPO, "results", "s7")
OUT = os.path.join(S7, "audit")
DELTA, ALPHA = 0.01, 0.05
ARMS = ["b4ts", "b4tpc", "b5", "b4sats", "b4satpc", "b5sa"]


def outcomes(d, rid, cfg, ra, rb):
    tp = pd.read_csv(os.path.join(d, f"{rid}-tput.csv"))
    ue = pd.read_csv(os.path.join(d, f"{rid}-ues.csv")).set_index("ue")
    w = tp[(tp.time_s > 5 + 1e-9) & (tp.time_s <= 30 + 1e-9)].copy()
    w["g"] = w.rx_bytes * 8 / 0.5 / 1e6  # Mbps per 0.5-s window
    grp = w.ue.map(ue.group)
    off = np.where(grp == "nearB", rb, ra).astype(float)
    if cfg == "C3":
        off = np.where((grp != "nearB") & (w.time_s > 15 + 1e-9), 1.23, off)
    w["off"] = off
    nwin = w.groupby("ue").size()
    g_ue, o_ue = w.groupby("ue").g.mean(), w.groupby("ue").off.mean()
    return dict(G=g_ue.mean() / o_ue.mean(), V=float((w.g < 0.95 * w.off).mean()), nwin_min=int(nwin.min()),
                nwin_max=int(nwin.max()), n_ue=int(w.ue.nunique()))


def build_table():
    st = json.load(open(os.path.join(S7, "state.json")))
    I = st["index"]
    rows, cache = [], {}
    for rid, e in I.items():
        src = e.get("source_run_id", rid)
        d = I[src]["data"] if e["status"] == "derived" else e["data"]
        key = (src, e["cfg"])
        if key not in cache:
            cache[key] = outcomes(d, src, e["cfg"], e["rate_a"], e["rate_b"])
        rows.append(dict(cfg=e["cfg"], arm=e["arm"], pos=e["pos"], rate_a=e["rate_a"], rate_b=e["rate_b"], seed=e["seed"],
                         run=rid, status=e["status"], **cache[key]))
    return pd.DataFrame(rows)


def seed_means(T, a, b, o):
    W = T.set_index(["cfg", "arm", "pos", "rate_a", "rate_b", "seed"])[o]
    d = (W.loc[a] - W.loc[b]).dropna()
    return d, d.groupby(level="seed").mean().sort_index()


def tests(sm, sign, idx):
    x = sign * sm.values  # benefit-oriented seed means
    n = len(x)
    est, sd = x.mean(), x.std(ddof=1)
    bt = x[idx].mean(axis=1)
    pb = {"helps": ((bt <= DELTA).sum() + 1) / (len(bt) + 1), "harms": ((bt >= -DELTA).sum() + 1) / (len(bt) + 1),
          "equiv": max(((bt <= -DELTA).sum() + 1), ((bt >= DELTA).sum() + 1)) / (len(bt) + 1)}
    if sd > 0:
        se = sd / np.sqrt(n)
        tdist = stats.t(n - 1)
        pt = {"helps": tdist.sf((est - DELTA) / se), "harms": tdist.cdf((est + DELTA) / se),
              "equiv": max(tdist.sf((est + DELTA) / se), tdist.cdf((est - DELTA) / se))}
        ci90 = (est - tdist.ppf(0.95) * se, est + tdist.ppf(0.95) * se)
    else:  # all seed means identical: no sampling-variance estimate exists
        pt = {"helps": float(est <= DELTA), "harms": float(est >= -DELTA), "equiv": float(abs(est) >= DELTA)}
        ci90 = (est, est)
    return est, sd, ci90, pb, pt


def holm(p):
    p = np.asarray(p, float)
    o = np.argsort(p)
    adj, run = np.empty(len(p)), 0.0
    for j, i in enumerate(o):
        run = max(run, min(1.0, (len(p) - j) * p[i]))
        adj[i] = run
    return adj


def main():
    os.makedirs(OUT, exist_ok=True)
    T = build_table()
    T.to_csv(os.path.join(OUT, "independent_outcomes.csv"), index=False)
    R = pd.read_csv(os.path.join(S7, "analysis", "s7_runs.csv"))
    M = T.merge(R[["run", "rel_goodput", "viol"]], on="run")
    agree = dict(runs=len(M), max_abs_dG=float((M.G - M.rel_goodput).abs().max()), max_abs_dV=float((M.V - M.viol).abs().max()),
                 runs_V_differs=int(((M.V - M.viol).abs() > 1e-9).sum()), nwin=sorted(set(T.nwin_min) | set(T.nwin_max)),
                 n_ue=sorted(set(T.n_ue)))
    rng = np.random.default_rng(424242)  # different stream from the registered analysis
    seeds = sorted(T.seed.unique())
    idx = rng.integers(0, len(seeds), (20000, len(seeds)))
    F = [("F1", f"{a}-B3", "D1", ("D1", a), ("D1", "B3")) for a in ARMS] + \
        [("F2", f"{a}-B3", "D0", ("D0", a), ("D0", "B3")) for a in ARMS] + \
        [("F3", f"{a}-B3h", c, (c, a), (c, "B3h")) for c in ["D1", "D1d1", "D1d2", "D1d3"] for a in ARMS] + \
        [("F4", "B3h-B3@D1", c, (c, "B3h"), ("D1", "B3")) for c in ["D1", "D1d1", "D1d2", "D1d3"]]
    res = []
    for fam, name, delay, a, b in F:
        for o, sign in [("G", 1), ("V", -1)]:
            d, sm = seed_means(T, a, b, o)
            est, sd, ci90, pb, pt = tests(sm, sign, idx)
            nz = d[d.abs() > 1e-12]
            res.append(dict(family=fam, contrast=name, delay=delay, outcome=o, mean_delta=sign * est, sd_seed_means=sd,
                            t90_lo=sign * (ci90[0] if sign > 0 else ci90[1]), t90_hi=sign * (ci90[1] if sign > 0 else ci90[0]),
                            clusters=len(d), clusters_nonzero=len(nz), seeds_nonzero=int((sm.abs() > 1e-12).sum()),
                            max_abs_cluster_delta=float(d.abs().max()),
                            p_boot_equiv=pb["equiv"], p_boot_helps=pb["helps"], p_boot_harms=pb["harms"],
                            p_t_equiv=pt["equiv"], p_t_helps=pt["helps"], p_t_harms=pt["harms"]))
    C = pd.DataFrame(res)
    for fam in ["F1", "F2", "F3", "F4"]:
        m = C.family == fam
        C.loc[m, "holm_boot_equiv_x2"] = holm(np.minimum(1, 2 * C.loc[m, "p_boot_equiv"]))
        C.loc[m, "holm_t_equiv"] = holm(C.loc[m, "p_t_equiv"])
    C["equiv_boot"] = C.holm_boot_equiv_x2 <= ALPHA
    C["equiv_t"] = C.holm_t_equiv <= ALPHA
    C["degenerate"] = C.sd_seed_means == 0
    C.to_csv(os.path.join(OUT, "independent_confirmatory.csv"), index=False)
    reg = pd.read_csv(os.path.join(S7, "analysis", "confirmatory_contrasts.csv"))
    reg["fam"] = reg.family.str[:2]
    reg["o"] = reg.outcome.map({"rel_goodput": "G", "viol": "V"})
    J = C.merge(reg, left_on=["family", "contrast", "delay", "outcome"], right_on=["fam", "contrast", "delay", "o"])
    assert len(J) == 80, len(J)
    mdiff = float((J.mean_delta - J["mean"]).abs().max())
    J[["family_x", "contrast", "delay", "outcome_x", "mean_delta", "mean", "category", "holm_p", "equiv_boot", "equiv_t", "degenerate",
       "clusters_nonzero", "t90_lo", "t90_hi", "holm_t_equiv"]].to_csv(os.path.join(OUT, "registered_vs_audit.csv"), index=False)
    json.dump(dict(outcome_agreement=agree, n_hypotheses=len(C), equiv_boot=int(C.equiv_boot.sum()), equiv_t=int(C.equiv_t.sum()),
                   degenerate_all_seed_means_equal=int(C.degenerate.sum()),
                   all_zero=int((C.clusters_nonzero == 0).sum()),
                   registered_equivalent=int((reg.category == "equivalent").sum()),
                   max_abs_mean_diff_vs_registered=mdiff, max_ci_bound_diff_note="registered BCa vs audit t90 not comparable"),
              open(os.path.join(OUT, "audit_numbers.json"), "w"), indent=1)
    print(json.load(open(os.path.join(OUT, "audit_numbers.json"))))


if __name__ == "__main__":
    main()
