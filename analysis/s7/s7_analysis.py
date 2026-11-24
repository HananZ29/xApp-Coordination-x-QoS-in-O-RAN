#!/usr/bin/env python3
"""S7 registered analysis (frozen with the S7 registry before any S7 output existed; docs/methodology/S7-registry.md).

Unit: the seed block. Every seed contains all 12 conditions, so the condition-stratified pooled contrast equals the
mean over seeds of the per-seed mean over conditions. Inference resamples SEED BLOCKS jointly across conditions
(preserving policy pairing and condition structure; G6 check 1).

Per hypothesis H = (contrast, outcome), benefit-oriented (goodput +, violation -), margin delta (0.01 both):
  BCa 95 % CI (B = 10 000 seed-block resamples, jackknife acceleration over seeds).
  Category tests on the bootstrap distribution: helps (H0: benefit <= delta), harms (H0: harm <= delta),
  equivalent (TOST, H0: |effect| >= delta).  p_H = 2 * min(p_help, p_harm, p_equiv) (capped at 1).
  Holm within each family at alpha 0.05; a Holm-rejected H takes its category, otherwise "inconclusive".
  Families in registered order with serial gatekeeping (family k+1 is confirmatory only if family k has >= 1
  Holm rejection):  F1 C1-D1 total (arm - B3 @D1)  ->  F2 C1-D0 (arm - B3 @D0)  ->
  F3 C2 arbitration (arm - B3h at equal hold: D1, D1+Delta_k)  ->  F4 C2 overhead (B3h@hold - B3@D1).
  Reported with every H: positive / zero / negative cluster counts, exact sign test on non-zero clusters
  (descriptive; clusters within a seed share the seed) and exact sign test on the 13 per-seed means.
Descriptive (never confirmatory): C2 total delay curve (arm@D1+Delta_k - B3@D1), per-condition contrasts,
  family A (B1/B2/B3 - B0), reversed B3 order (S8a), C3 step dynamics, secondary outcomes, and the SUPPLEMENTARY
  event-conditional analysis (clusters whose reference run has >= 1 combined flag; under-covered if < 10 clusters).
Interim outputs (fewer than 13 complete seeds) are labelled INTERIM and are for integrity monitoring only.
"""
import hashlib
import json
import os
import sys

import numpy as np
import pandas as pd
from scipy import stats

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "analysis", "s5"))
sys.path.insert(0, os.path.join(REPO, "scripts", "run"))
import coord_pilot as CP  # noqa: E402
import s7_run as R  # noqa: E402

S7 = os.path.join(REPO, "results", "s7")
OUT = os.path.join(S7, "analysis")
DELTA = 0.01
ALPHA = 0.05
B = 10000
ARMS = list(R.COORD)
PRIMARY = ["rel_goodput", "viol"]
SECONDARY = ["min_user_frac", "viol_edge", "etx", "rlf", "ho_reversals"]
BENEFIT_SIGN = {"rel_goodput": 1, "viol": -1, "min_user_frac": 1, "viol_edge": -1, "etx": -1, "rlf": -1, "ho_reversals": -1}
HOLDMS = {"D1": 0.131, "D1d1": 50.131, "D1d2": 125.131, "D1d3": 250.131}


# ---------------------------------------------------------------- run rows
def c3_fix(r, d, rid, ra, rb):
    """C3: violation threshold uses the time-varying offered rate (cell-A UEs 0.51 -> 1.23 Mbps at 15 s)."""
    fl = pd.read_csv(os.path.join(d, f"{rid}-flows.csv"))
    fs = pd.read_csv(os.path.join(d, f"{rid}-flowsum.csv")).set_index("ue")
    cell_a = [u for u in fs.index if abs(fs.offered_mbps[u] - rb) > 1e-6]
    post = fl[(fl.time_s > 5 + 1e-9) & (fl.time_s <= 30 + 1e-9)].copy()
    post["A"] = np.where(post.ue.isin(cell_a), np.where(post.time_s > 15 + 1e-9, 1.23, ra), rb)
    v = post.goodput_mbps < 0.95 * post.A
    r["viol"] = float(v.mean())
    r["viol_edge"] = float(v[post.ue.isin([3, 4, 5])].mean())
    g, a = post.groupby("ue").goodput_mbps.mean(), post.groupby("ue").A.mean()
    r["min_user_frac"] = float((g / a).min())
    r["offered"] = float(a.mean())
    ps = post[post.time_s > 15 + 1e-9]
    r["viol_post_step"] = float(v[post.time_s > 15 + 1e-9].mean())
    per_w = ps.assign(v=v[ps.index]).groupby("time_s").v.sum().sort_index()
    rec = np.nan
    w = per_w.values
    for i in range(len(w) - 3):  # recovery: first window from which 4 consecutive windows (2 s) have no violating UE
        if (w[i:i + 4] == 0).all():
            rec = float(per_w.index[i] - 15)
            break
    r["recovery_s"] = rec
    return r


def rows(st, complete):
    out = []
    for r_id, e in st["index"].items():
        if e["seed"] not in complete:
            continue
        src = e.get("source_run_id", r_id)
        k = dict(pos=e["pos"], rate_a=e["rate_a"], rate_b=e["rate_b"], mode=e["arm"], seed=e["seed"])
        r = CP.row(e["data"], src, k)
        r.update(cfg=e["cfg"], arm=e["arm"], run=r_id, status=e["status"], source=src)
        r["offered"] = (6 * e["rate_a"] + 3 * e["rate_b"]) / 9
        if e["cfg"] == "C3":
            r = c3_fix(r, e["data"], src, e["rate_a"], e["rate_b"])
        r["rel_goodput"] = r["mean_goodput"] / r["offered"]
        r["etx"] = r["etx_c1_Ws"] + r["etx_c2_Ws"]
        fl = R.flagged(os.path.join(e["data"], src), "combined")
        r["ref_combined_flag"] = fl
        out.append(r)
    return pd.DataFrame(out)


# ---------------------------------------------------------------- inference
def bca(seed_means, idx):
    """seed_means: (S,) per-seed condition-stratified mean differences. Returns est, lo, hi, boot replicates."""
    est = seed_means.mean()
    bt = seed_means[idx].mean(axis=1)
    if np.allclose(bt, bt[0]) and np.allclose(seed_means, est):
        return est, est, est, bt
    prop = ((bt < est).sum() + 0.5 * np.isclose(bt, est).sum()) / len(bt)
    z0 = stats.norm.ppf(np.clip(prop, 1e-6, 1 - 1e-6))
    jk = np.array([np.delete(seed_means, i).mean() for i in range(len(seed_means))])
    num, den = ((jk.mean() - jk) ** 3).sum(), 6 * (((jk.mean() - jk) ** 2).sum() ** 1.5)
    a = num / den if den > 0 else 0.0
    q = []
    for z in (stats.norm.ppf(0.025), stats.norm.ppf(0.975)):
        q.append(stats.norm.cdf(z0 + (z0 + z) / (1 - a * (z0 + z))))
    lo, hi = np.quantile(bt, q)
    return est, float(lo), float(hi), bt


def category_p(bt_benefit):
    n = len(bt_benefit)
    p_help = ((bt_benefit <= DELTA).sum() + 1) / (n + 1)
    p_harm = ((bt_benefit >= -DELTA).sum() + 1) / (n + 1)
    p_eq = max(((bt_benefit <= -DELTA).sum() + 1) / (n + 1), ((bt_benefit >= DELTA).sum() + 1) / (n + 1))
    ps = {"helps": p_help, "harms": p_harm, "equivalent": p_eq}
    cat = min(ps, key=ps.get)
    return min(1.0, 2 * ps[cat]), cat, ps


def holm(p):
    p = np.asarray(p, float)
    o = np.argsort(p)
    m = len(p)
    adj = np.empty(m)
    run = 0.0
    for j, i in enumerate(o):
        run = max(run, min(1.0, (m - j) * p[i]))
        adj[i] = run
    return adj


def contrast(W, a, b, outcome, seeds, idx):
    """W: run table indexed by (cfg, arm, pos, rate_a, rate_b, seed). a, b = (cfg, arm)."""
    x = W.loc[a[0], a[1]][outcome]
    y = W.loc[b[0], b[1]][outcome]
    d = (x - y).dropna()
    d = d[d.index.get_level_values("seed").isin(seeds)]
    sm = d.groupby(level="seed").mean().reindex(seeds)
    if sm.isna().any():
        return None
    est, lo, hi, bt = bca(sm.values, idx)
    s = BENEFIT_SIGN[outcome]
    pH, cat, ps = category_p(s * bt)
    pos, neg = int((s * d > 1e-12).sum()), int((s * d < -1e-12).sum())
    nz = pos + neg
    spos, sneg = int((s * sm > 1e-12).sum()), int((s * sm < -1e-12).sum())
    se = sm.std(ddof=1) / np.sqrt(len(sm))
    tq = stats.t.ppf(0.975, len(sm) - 1) * se
    return dict(outcome=outcome, n_seeds=len(seeds), clusters=len(d), mean=est, ci_lo=lo, ci_hi=hi,
                t_ci_lo_SENSITIVITY=est - tq, t_ci_hi_SENSITIVITY=est + tq,
                benefit_mean=s * est, clusters_benefit=pos, clusters_zero=len(d) - nz, clusters_harm=neg,
                sign_p_clusters=stats.binomtest(pos, nz).pvalue if nz else 1.0,
                seeds_benefit=spos, seeds_harm=sneg,
                sign_p_seeds=stats.binomtest(spos, spos + sneg).pvalue if spos + sneg else 1.0,
                p_helps=ps["helps"], p_harms=ps["harms"], p_equiv=ps["equivalent"], p_H=pH, best_category=cat)


def families():
    F = []
    for arm in ARMS:
        F.append(("F1 C1-D1 total", f"{arm}-B3", "D1", ("D1", arm), ("D1", "B3")))
    for arm in ARMS:
        F.append(("F2 C1-D0", f"{arm}-B3", "D0", ("D0", arm), ("D0", "B3")))
    for cfg in HOLDMS:
        for arm in ARMS:
            F.append(("F3 C2 arbitration", f"{arm}-B3h", cfg, (cfg, arm), (cfg, "B3h")))
    for cfg in HOLDMS:
        F.append(("F4 C2 overhead", "B3h-B3@D1", cfg, (cfg, "B3h"), ("D1", "B3")))
    return F


def descriptive():
    D = []
    for cfg in ["D1d1", "D1d2", "D1d3"]:
        for arm in ARMS:
            D.append(("C2 total (delay curve)", f"{arm}-B3@D1", cfg, (cfg, arm), ("D1", "B3")))
    for cfg in ["D0", "D1"]:
        for arm in ["B1", "B2", "B3"]:
            D.append(("Family A", f"{arm}-B0", cfg, (cfg, arm), (cfg, "B0")))
    D.append(("S8a reversed B3 order", "B3rev-B3", "D0", ("S8a", "B3rev"), ("D0", "B3")))
    for arm in ["b4sats", "b4satpc", "b5sa"]:
        D.append(("C3 step total", f"{arm}-B3", "C3", ("C3", arm), ("C3", "B3")))
        D.append(("C3 step arbitration", f"{arm}-B3h", "C3", ("C3", arm), ("C3", "B3h")))
    D.append(("C3 step overhead", "B3h-B3", "C3", ("C3", "B3h"), ("C3", "B3")))
    return D


# ---------------------------------------------------------------- figures
def figures(C, DS, label):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    FIG = os.path.join(OUT, "figures")
    os.makedirs(FIG, exist_ok=True)
    col = dict(zip(ARMS + ["B3h"], ["#E69F00", "#56B4E9", "#009E73", "#0072B2", "#D55E00", "#CC79A7", "#000000"]))
    mk = dict(zip(ARMS + ["B3h"], "osD^v<P"))
    lab = {"b4ts": "B4 TS-prio", "b4tpc": "B4 TPC-prio", "b5": "B5", "b4sats": "B4sa TS-prio",
           "b4satpc": "B4sa TPC-prio", "b5sa": "B5sa", "B3h": "B3h (overhead)"}
    ylab = {"rel_goodput": "Δ relative goodput", "viol": "Δ QoS-violation fraction"}
    # Fig 1: C1 paired effects vs B3 at D0 and D1
    fig, axes = plt.subplots(2, 2, figsize=(7, 5), sharex=True)
    for j, (fam, cfg) in enumerate([("F2 C1-D0", "D0"), ("F1 C1-D1 total", "D1")]):
        for i, o in enumerate(PRIMARY):
            ax = axes[i, j]
            x = C[(C.family == fam) & (C.outcome == o)]
            ax.axhspan(-DELTA, DELTA, color="#e6e6e6", lw=0, zorder=0)
            ax.axhline(0, color="#888", lw=0.8)
            for k, arm in enumerate(ARMS):
                r = x[x.contrast == f"{arm}-B3"]
                if len(r):
                    r = r.iloc[0]
                    ax.errorbar(k, r["mean"], yerr=[[r["mean"] - r.ci_lo], [r.ci_hi - r["mean"]]], fmt=mk[arm],
                                color=col[arm], ms=5, lw=1.5, capsize=2)
            ax.set_xticks(range(len(ARMS)))
            ax.set_xticklabels([lab[a] for a in ARMS], rotation=45, ha="right", fontsize=7)
            ax.set_title(f"{cfg}: {ylab[o]} vs B3", fontsize=8)
    fig.suptitle(f"Paired effects (seed-block BCa 95 % CI; grey = ±δ) {label}", fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "fig_s7_c1_effects.pdf"))
    plt.close(fig)
    # Fig 2: delay curve decomposition (arbitration vs B3h, overhead B3h - B3@D1, total vs B3@D1)
    fig, axes = plt.subplots(2, 2, figsize=(7, 5), sharex=True)
    xs = [HOLDMS[c] for c in HOLDMS]
    for i, o in enumerate(PRIMARY):
        for j, (fam, ttl) in enumerate([("F3 C2 arbitration", "arbitration: arm − B3h (equal hold)"),
                                        ("C2 total (delay curve)", "total: arm − B3@D1; overhead: B3h − B3@D1")]):
            ax = axes[i, j]
            ax.axhspan(-DELTA, DELTA, color="#e6e6e6", lw=0, zorder=0)
            ax.axhline(0, color="#888", lw=0.8)
            src = C if fam.startswith("F3") else DS
            for k, arm in enumerate(ARMS):
                if fam.startswith("F3"):
                    x = src[(src.family == fam) & (src.outcome == o) & (src.contrast == f"{arm}-B3h")]
                else:
                    x = pd.concat([C[(C.family == "F1 C1-D1 total") & (C.outcome == o) & (C.contrast == f"{arm}-B3")],
                                   src[(src.family == fam) & (src.outcome == o) & (src.contrast == f"{arm}-B3@D1")]])
                x = x.assign(h=x.delay.map(HOLDMS)).sort_values("h")
                off = (k - 2.5) * 0.02
                ax.errorbar(np.array(x.h) * (1 + off), x["mean"], yerr=[x["mean"] - x.ci_lo, x.ci_hi - x["mean"]],
                            marker=mk[arm], color=col[arm], ms=4, lw=1.2, capsize=2, label=lab[arm])
            if not fam.startswith("F3"):
                x = C[(C.family == "F4 C2 overhead") & (C.outcome == o)].assign(h=lambda t: t.delay.map(HOLDMS)).sort_values("h")
                ax.errorbar(x.h, x["mean"], yerr=[x["mean"] - x.ci_lo, x.ci_hi - x["mean"]], marker="P", color="#000",
                            ls="--", ms=4, lw=1.2, capsize=2, label=lab["B3h"])
            ax.set_xscale("log")
            ax.set_xticks(xs)
            ax.set_xticklabels(["0.131", "50", "125", "250"], fontsize=7)
            ax.set_title(f"{ttl}", fontsize=7)
            ax.set_ylabel(ylab[o], fontsize=7)
    axes[1, 0].set_xlabel("coordination hold (ms)", fontsize=7)
    axes[1, 1].set_xlabel("coordination hold (ms)", fontsize=7)
    axes[0, 1].legend(fontsize=5, ncol=2)
    fig.suptitle(f"Delay sensitivity at D1 + Δ (BCa 95 % CI) {label}", fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, "fig_s7_delay_decomposition.pdf"))
    plt.close(fig)


# ---------------------------------------------------------------- main
def main():
    os.makedirs(OUT, exist_ok=True)
    st = json.load(open(os.path.join(S7, "state.json")))
    need = {}
    for c in R.cells():
        need[c] = True
    complete = []
    for s in R.SEEDS:
        ok = all(st["index"].get(R.rid(cfg, x, ra, rb, arm, s), {}).get("status") in ("completed", "derived")
                 for cfg, x, ra, rb, arm in R.cells())
        if ok:
            complete.append(s)
    label = "" if len(complete) == len(R.SEEDS) else f"[INTERIM {len(complete)}/{len(R.SEEDS)} seeds; monitoring only]"
    if not complete:
        print("no complete seed block yet")
        return
    T = rows(st, complete)
    T.to_csv(os.path.join(OUT, "s7_runs.csv"), index=False)
    W = T.set_index(["cfg", "arm", "pos", "rate_a", "rate_b", "seed"]).sort_index()
    seeds = sorted(complete)
    rng = np.random.default_rng(20261124)
    idx = rng.integers(0, len(seeds), (B, len(seeds)))
    res = []
    for fam, name, delay, a, b in families():
        for o in PRIMARY:
            r = contrast(W, a, b, o, seeds, idx)
            if r:
                res.append(dict(family=fam, contrast=name, delay=delay, **r))
    C = pd.DataFrame(res)
    gate = True
    C["holm_p"], C["category"], C["confirmatory"] = np.nan, "inconclusive", False
    for fam in ["F1 C1-D1 total", "F2 C1-D0", "F3 C2 arbitration", "F4 C2 overhead"]:
        m = C.family == fam
        C.loc[m, "holm_p"] = holm(C.loc[m, "p_H"])
        rej = m & (C.holm_p <= ALPHA)
        C.loc[rej, "category"] = C.loc[rej, "best_category"]
        C.loc[m, "confirmatory"] = gate
        gate = gate and bool(rej.sum() > 0)
    C.to_csv(os.path.join(OUT, "confirmatory_contrasts.csv"), index=False)
    ds = []
    for fam, name, delay, a, b in descriptive():
        for o in PRIMARY + SECONDARY + (["viol_post_step", "recovery_s"] if delay == "C3" else []):
            if o not in BENEFIT_SIGN:
                BENEFIT_SIGN[o] = -1
            r = contrast(W, a, b, o, seeds, idx)
            if r:
                ds.append(dict(family=fam, contrast=name, delay=delay, **r))
    for fam, name, delay, a, b in families():
        for o in SECONDARY:
            r = contrast(W, a, b, o, seeds, idx)
            if r:
                ds.append(dict(family=fam + " (secondary)", contrast=name, delay=delay, **r))
    DS = pd.DataFrame(ds)
    DS.to_csv(os.path.join(OUT, "descriptive_contrasts.csv"), index=False)
    # per-condition (descriptive)
    pc = []
    for fam, name, delay, a, b in families():
        for o in PRIMARY:
            d = (W.loc[a[0], a[1]][o] - W.loc[b[0], b[1]][o]).dropna()
            for (x, ra, rb), g in d.groupby(level=["pos", "rate_a", "rate_b"]):
                pc.append(dict(family=fam, contrast=name, delay=delay, outcome=o, pos=x, rate_a=ra, rate_b=rb,
                               n=len(g), mean=g.mean(), sd=g.std(ddof=1), nonzero=int((g.abs() > 1e-12).sum())))
    pd.DataFrame(pc).to_csv(os.path.join(OUT, "per_condition_DESCRIPTIVE.csv"), index=False)
    # event coverage and SUPPLEMENTARY event-conditional analysis
    ev = []
    for cfg in ["D0", "D1", "D1d1", "D1d2", "D1d3", "C3"]:
        ref = "B3" if cfg == "D0" else "B3h"
        x = T[(T.cfg == cfg) & (T.arm == ref)]
        ev.append(dict(cfg=cfg, reference=ref, clusters=len(x), flagged_combined=int((x.ref_combined_flag == True).sum()),  # noqa: E712
                       under_covered=int((x.ref_combined_flag == True).sum()) < 10))  # noqa: E712
    EV = pd.DataFrame(ev)
    EV.to_csv(os.path.join(OUT, "event_coverage.csv"), index=False)
    sup = []
    for fam, name, delay, a, b in families():
        ref = ("D0", "B3") if delay == "D0" else (delay, "B3h")
        fl = W.loc[ref[0], ref[1]]["ref_combined_flag"]
        keys = fl[fl == True].index  # noqa: E712
        for o in PRIMARY:
            d = (W.loc[a[0], a[1]][o] - W.loc[b[0], b[1]][o]).reindex(keys).dropna()
            sup.append(dict(family=fam, contrast=name, delay=delay, outcome=o, flagged_clusters=len(d),
                            mean=d.mean() if len(d) else np.nan, sd=d.std(ddof=1) if len(d) > 1 else np.nan,
                            benefit=int((BENEFIT_SIGN[o] * d > 1e-12).sum()), harm=int((BENEFIT_SIGN[o] * d < -1e-12).sum()),
                            under_covered=len(d) < 10))
    pd.DataFrame(sup).to_csv(os.path.join(OUT, "event_conditional_SUPPLEMENTARY.csv"), index=False)
    try:
        figures(C, DS, label)
    except Exception as e:  # a figure problem never blocks the tables
        print("FIGURE ERROR", repr(e))
    me = open(os.path.abspath(__file__), "rb").read()
    json.dump(dict(label=label or "FINAL", seeds=seeds, runs=len(T), derived=int((T.status == "derived").sum()),
                   script_sha256=hashlib.sha256(me).hexdigest(), bootstrap_B=B, rng_seed=20261124, delta=DELTA, alpha=ALPHA),
              open(os.path.join(OUT, "provenance.json"), "w"), indent=1)
    with open(os.path.join(OUT, "summary.txt"), "w") as f:
        f.write(f"{label or 'FINAL'}  seeds {seeds}\n\n")
        f.write(C[["family", "contrast", "delay", "outcome", "mean", "ci_lo", "ci_hi", "clusters_benefit", "clusters_zero",
                   "clusters_harm", "holm_p", "category", "confirmatory"]].round(5).to_string(index=False))
        f.write("\n\nEvent coverage\n" + EV.to_string(index=False) + "\n")
    print(open(os.path.join(OUT, "summary.txt")).read()[:3000])


if __name__ == "__main__":
    main()
