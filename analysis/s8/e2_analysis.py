#!/usr/bin/env python3
"""S8-E2 registered analysis (implements docs/methodology/S8-registry.md §2.4; committed before E2 starts).

Unit: (condition, seed) cluster; 6 conditions (rate_b 0.29) x seeds 3001-3026; variants V0 24/3, V1 27/3, V2 24/6.
E2-A interaction frequency: for V_k in {V1, V2} vs V0, two-sided exact McNemar test on the cluster-level B3h@D1
  'combined' flag (discordant pairs b = V0 only, c = V_k only; binomial(b + c, 0.5)); Holm over the 2 tests.
E2-B robustness of coordination effects: for V_k, a in {B4sa-TS, B4sa-TPC, B5sa}, o in {G, V}:
  I = mean over clusters of [Y(a) - Y(B3h)](V_k) - [Y(a) - Y(B3h)](V0); inference on the 26 per-seed means (mean over the
  6 conditions), t with df = n_seeds - 1; benefit orientation (G +, V -); delta 0.01; S7 classification
  (p_H = 2 min(p_help, p_harm, p_eq), Holm over the non-degenerate hypotheses, alpha 0.05); degenerate (all cluster
  values exactly 0): 'no difference observed', Clopper-Pearson bound on the share of non-zero clusters, excluded from Holm.
Descriptive: arm - B3h per variant (all clusters; flagged-only); B2 - B0 and B3 - B0 per variant (G, V, min-user
  fraction, energy proxy) with t CIs over seeds; TPC reductions / restorations / time at reduced power per variant.
The engineering-check runs (seed 3999) are excluded. Label FINAL only if results/s8/e2/run.log contains 'E2 DONE'.
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

E2 = os.path.join(REPO, "results", "s8", "e2")
OUT = os.path.join(E2, "analysis")
DELTA, ALPHA = 0.01, 0.05
ARMS = {"b4sats": "B4sa-TS", "b4satpc": "B4sa-TPC", "b5sa": "B5sa"}
SIGN = {"G": 1, "V": -1, "min_user_frac": 1, "etx": -1}
K = ["pos", "rate_a", "rate_b", "seed"]


def table():
    st = json.load(open(os.path.join(E2, "state.json")))
    I = st["index"]
    rows = []
    for r, e in I.items():
        if e["status"] not in ("completed", "derived") or e["seed"] == 3999:
            continue
        src = e.get("source_run_id", r)
        d = I[src]["data"] if e["status"] == "derived" else e["data"]
        k = dict(pos=e["pos"], rate_a=e["rate_a"], rate_b=e["rate_b"], mode=e["arm"], seed=e["seed"])
        x = CP.row(d, src, k)
        off = (6 * e["rate_a"] + 3 * e["rate_b"]) / 9
        x.update(variant=e["variant"], arm=e["arm"], run=r, status=e["status"], G=x["mean_goodput"] / off, V=x["viol"],
                 etx=x["etx_c1_Ws"] + x["etx_c2_Ws"],
                 flag=R.flagged(os.path.join(d, src), "combined") if e["arm"] == "B3h" else None)
        rows.append(x)
    return pd.DataFrame(rows)


def holm(p):
    p = np.asarray(p, float)
    o, adj, run = np.argsort(p), np.empty(len(p)), 0.0
    for j, i in enumerate(o):
        run = max(run, min(1.0, (len(p) - j) * p[i]))
        adj[i] = run
    return adj


def seed_t(x):
    """x: per-cluster Series indexed by K. Returns est, se, df on per-seed means."""
    sm = x.groupby(level="seed").mean()
    n = len(sm)
    return sm.mean(), sm.std(ddof=1) / np.sqrt(n), n - 1


def classify(beta, se, df):
    T = stats.t(df)
    p = {"helps": T.sf((beta - DELTA) / se), "harms": T.cdf((beta + DELTA) / se),
         "equivalent": max(T.sf((beta + DELTA) / se), T.cdf((beta - DELTA) / se))}
    c = min(p, key=p.get)
    return min(1.0, 2 * p[c]), c, p


def main():
    os.makedirs(OUT, exist_ok=True)
    final = os.path.exists(os.path.join(E2, "run.log")) and "E2 DONE" in open(os.path.join(E2, "run.log")).read()
    label = "FINAL" if final else "INTERIM (monitoring only; no inferential claims)"
    T = table()
    T.to_csv(os.path.join(OUT, "e2_runs.csv"), index=False)
    W = T.set_index(["variant", "arm"] + K).sort_index()
    # E2-A
    fl = W.xs("B3h", level="arm")["flag"].astype(bool)
    A = []
    for v in ("V1", "V2"):
        f0, fk = fl.xs("V0", level="variant"), fl.xs(v, level="variant")
        j = f0.index.intersection(fk.index)
        b, c = int((f0[j] & ~fk[j]).sum()), int((~f0[j] & fk[j]).sum())
        A.append(dict(variant=v, clusters=len(j), flagged_V0=int(f0[j].sum()), flagged_Vk=int(fk[j].sum()), only_V0=b, only_Vk=c,
                      p_mcnemar_exact=stats.binomtest(min(b, c), b + c, 0.5).pvalue if b + c else 1.0))
    A = pd.DataFrame(A)
    A["holm_p"] = holm(A.p_mcnemar_exact)
    A.to_csv(os.path.join(OUT, "e2A_frequency.csv"), index=False)
    # E2-B
    rows = []
    for v in ("V1", "V2"):
        for a in ARMS:
            for o in ("G", "V"):
                ck = W.loc[(v, a)][o] - W.loc[(v, "B3h")][o]
                c0 = W.loc[("V0", a)][o] - W.loc[("V0", "B3h")][o]
                I = (ck - c0).dropna()
                s = SIGN[o]
                r = dict(variant=v, arm=ARMS[a], outcome=o, clusters=len(I), seeds=I.index.get_level_values("seed").nunique(),
                         mean=I.mean(), nonzero=int((I.abs() > 1e-12).sum()))
                est, se, df = seed_t(I)
                if r["nonzero"] == 0 or not se > 0:
                    r.update(degenerate=True, cp_upper_share_nonzero=1 - 0.05 ** (1 / len(I)))
                else:
                    pH, cat, p = classify(s * est, se, df)
                    q = stats.t.ppf(0.975, df) * se
                    r.update(degenerate=False, est=est, se=se, df=df, t95_lo=est - q, t95_hi=est + q, p_helps=p["helps"],
                             p_harms=p["harms"], p_equiv=p["equivalent"], p_H=pH, best_category=cat)
                rows.append(r)
    Bt = pd.DataFrame(rows)
    nd = ~Bt.degenerate
    Bt["holm_p"], Bt["category"] = np.nan, np.where(Bt.degenerate, "no difference observed", "inconclusive")
    if nd.any():
        Bt.loc[nd, "holm_p"] = holm(Bt.loc[nd, "p_H"])
        rej = nd & (Bt.holm_p <= ALPHA)
        Bt.loc[rej, "category"] = Bt.loc[rej, "best_category"]
    if not final:
        Bt["category"] = "INTERIM - not interpreted"
    Bt.to_csv(os.path.join(OUT, "e2B_interaction_contrasts.csv"), index=False)
    # descriptive
    D = []
    for v in ("V0", "V1", "V2"):
        fv = fl.xs(v, level="variant")
        for a in ARMS:
            for o in ("G", "V"):
                c = (W.loc[(v, a)][o] - W.loc[(v, "B3h")][o]).dropna()
                est, se, df = seed_t(c)
                D.append(dict(variant=v, contrast=f"{ARMS[a]} - B3h", outcome=o, mean_all=est,
                              t95_lo=est - stats.t.ppf(.975, df) * se if se > 0 else est, t95_hi=est + stats.t.ppf(.975, df) * se if se > 0 else est,
                              flagged_clusters=int(fv.sum()), mean_flagged=float(c.reindex(fv[fv].index).mean()) if fv.any() else np.nan))
        for arm in ("B2", "B3"):
            for o in ("G", "V", "min_user_frac", "etx"):
                c = (W.loc[(v, arm)][o] - W.loc[(v, "B0")][o]).dropna()
                est, se, df = seed_t(c)
                D.append(dict(variant=v, contrast=f"{arm} - B0", outcome=o, mean_all=est,
                              t95_lo=est - stats.t.ppf(.975, df) * se if se > 0 else est, t95_hi=est + stats.t.ppf(.975, df) * se if se > 0 else est))
    pd.DataFrame(D).to_csv(os.path.join(OUT, "e2_descriptive.csv"), index=False)
    tp = T[T.arm.isin(["B2", "B3"])].groupby(["variant", "arm"])[["tpc_reduce", "tpc_restore", "t_reduced_c1_s", "t_reduced_c2_s"]].mean()
    tp.to_csv(os.path.join(OUT, "e2_tpc_activity.csv"))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.8))
        for ax, o in zip(axes, ("G", "V")):
            q = Bt[Bt.outcome == o].reset_index(drop=True)
            ax.axvspan(-DELTA, DELTA, color="#e6e6e6", lw=0)
            ax.axvline(0, color="#888", lw=0.8)
            for i, r in q.iterrows():
                if r.degenerate:
                    ax.plot(0, i, "o", color="#222", mfc="white")
                else:
                    ax.errorbar(r.est, i, xerr=[[r.est - r.t95_lo], [r.t95_hi - r.est]], fmt="o", color="#009E73", capsize=2)
            ax.set_yticks(range(len(q)))
            ax.set_yticklabels([f"{r.variant} {r.arm}" for _, r in q.iterrows()], fontsize=6)
            ax.set_title(("Δ relative goodput" if o == "G" else "Δ violation fraction") + ": interaction vs V0", fontsize=7)
            ax.tick_params(labelsize=6)
        fig.suptitle(f"E2 TPC-variant interaction contrasts (95 % t CI; grey ±δ) [{label.split()[0]}]", fontsize=7)
        fig.tight_layout()
        fig.savefig(os.path.join(OUT, "fig_e2_interactions.pdf"))
        plt.close(fig)
    except Exception as ex:
        print("FIGURE ERROR", repr(ex))
    me = open(os.path.abspath(__file__), "rb").read()
    json.dump(dict(label=label, runs=len(T), derived=int((T.status == "derived").sum()), seeds=int(T.seed.nunique()),
                   script_sha256=hashlib.sha256(me).hexdigest(), delta=DELTA, alpha=ALPHA),
              open(os.path.join(OUT, "provenance.json"), "w"), indent=1)
    with open(os.path.join(OUT, "summary.txt"), "w") as f:
        f.write(f"S8-E2 {label}\n\nE2-A frequency\n{A.round(4).to_string(index=False)}\n\nE2-B interaction contrasts\n")
        cols = [c for c in ["variant", "arm", "outcome", "clusters", "est", "t95_lo", "t95_hi", "nonzero", "holm_p", "category",
                            "cp_upper_share_nonzero"] if c in Bt]
        f.write(Bt[cols].round(5).to_string(index=False) + "\n")
    print(open(os.path.join(OUT, "summary.txt")).read())


if __name__ == "__main__":
    main()
