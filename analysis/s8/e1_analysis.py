#!/usr/bin/env python3
"""S8-E1 registered analysis (frozen with docs/methodology/S8-registry.md before any E1 output).

Event (unit): a (condition, seed) cluster whose B3@D1 reference run is flagged by the 'combined' detector.
Estimand: theta(a, o) = mean over events of Y_o(a) - Y_o(B3) at D1, a in {B4sa-TS, B4sa-TPC, B5sa}, o in {G, V}.
  G = mean UE goodput / mean offered (5 < t <= 29.5 s), V = share of UE-windows below 0.95 x offered (as S7).
Inference (primary): seed-cluster-robust t. theta_hat = sum_s S_s / sum_s n_s (S_s, n_s: sum of differences and number
  of events in seed s); Var = G/(G-1) * sum_s (S_s - theta_hat n_s)^2 / (sum n_s)^2; df = G - 1 (G = seeds with events).
  Benefit orientation beta = theta (G), -theta (V); margin delta = 0.01 both.
  p_help = P(T >= (beta-delta)/SE), p_harm = P(T <= (beta+delta)/SE), p_eq = max(P(T >= (beta+delta)/SE), P(T <= (beta-delta)/SE))
  p_H = 2 min(...), Holm over the non-degenerate hypotheses of the family (6 at most), alpha 0.05; category or 'inconclusive'.
  Degenerate (every event difference exactly 0): 'no difference observed in n/n events', exact Clopper-Pearson 95 % upper
  bound on the share of events with a non-zero difference; excluded from Holm.
Sensitivity (labelled): seed-block bootstrap (B 10 000, rng 20261124) percentile and BCa 95 % CIs; CI-inclusion
  classification with the 90 % t interval (no factor 2).
Secondary / descriptive: theta vs B3h; min-user fraction, edge-UE V, RF energy proxy; decision consequences per event
  (TS suppressed / TPC suppressed / none) with outcome differences; flag rates (Clopper-Pearson) per condition and block;
  sampled-condition population effect = mean over ALL sampled clusters (unflagged arm runs derived = B3h) and its
  decomposition rate x conditional; comparison with S7 flag rates. No result is generalised beyond the two conditions.
Label: FINAL only if results/s8/e1/run.log contains 'E1 DONE'; otherwise INTERIM (monitoring only, no inference).
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
sys.path.insert(0, os.path.join(REPO, "analysis", "s7"))
import coord_pilot as CP  # noqa: E402

E1 = os.path.join(REPO, "results", "s8", "e1")
OUT = os.path.join(E1, "analysis")
DELTA, ALPHA, B = 0.01, 0.05, 10000
ARMS = {"b4sats": "B4sa-TS", "b4satpc": "B4sa-TPC", "b5sa": "B5sa"}
SIGN = {"G": 1, "V": -1, "min_user_frac": 1, "viol_edge": -1, "etx": -1}
S7_RATES = {("x45", 1.23, 0.29): (6, 13), ("x55", 0.92, 0.29): (0, 13)}


def table():
    st = json.load(open(os.path.join(E1, "state.json")))
    I = st["index"]
    rows = []
    for r, e in I.items():
        if e["status"] not in ("completed", "derived"):
            continue
        src = e.get("source_run_id", r)
        d = I[src]["data"] if e["status"] == "derived" else e["data"]
        k = dict(pos=e["pos"], rate_a=e["rate_a"], rate_b=e["rate_b"], mode=e["arm"], seed=e["seed"])
        x = CP.row(d, src, k)
        x.update(arm=e["arm"], run=r, status=e["status"])
        off = (6 * e["rate_a"] + 3 * e["rate_b"]) / 9
        x["G"], x["V"], x["etx"] = x["mean_goodput"] / off, x["viol"], x["etx_c1_Ws"] + x["etx_c2_Ws"]
        rows.append(x)
    return pd.DataFrame(rows), st


def clustered(d):
    """d: Series indexed by (pos, rate_a, rate_b, seed). Returns est, se, df, G, n."""
    g = d.groupby(level="seed")
    S, n = g.sum(), g.size()
    N, G = n.sum(), len(n)
    est = S.sum() / N
    se = np.sqrt(G / (G - 1) * ((S - est * n) ** 2).sum()) / N if G > 1 else np.nan
    return est, se, G - 1, G, int(N)


def tests(beta, se, df):
    T = stats.t(df)
    p = {"helps": T.sf((beta - DELTA) / se), "harms": T.cdf((beta + DELTA) / se),
         "equivalent": max(T.sf((beta + DELTA) / se), T.cdf((beta - DELTA) / se))}
    cat = min(p, key=p.get)
    return min(1.0, 2 * p[cat]), cat, p


def boot(d, rng):
    g = d.groupby(level="seed")
    S, n = g.sum().values, g.size().values
    idx = rng.integers(0, len(S), (B, len(S)))
    bt = S[idx].sum(1) / n[idx].sum(1)
    est = S.sum() / n.sum()
    lo, hi = np.quantile(bt, [0.025, 0.975])
    prop = ((bt < est).sum() + 0.5 * np.isclose(bt, est).sum()) / B
    z0 = stats.norm.ppf(np.clip(prop, 1e-6, 1 - 1e-6))
    jk = np.array([(S.sum() - S[i]) / (n.sum() - n[i]) for i in range(len(S))])
    num, den = ((jk.mean() - jk) ** 3).sum(), 6 * (((jk.mean() - jk) ** 2).sum() ** 1.5)
    a = num / den if den > 0 else 0.0
    q = [stats.norm.cdf(z0 + (z0 + z) / (1 - a * (z0 + z))) for z in (stats.norm.ppf(0.025), stats.norm.ppf(0.975))]
    blo, bhi = np.quantile(bt, q)
    return lo, hi, blo, bhi


def holm(p):
    p = np.asarray(p, float)
    o, adj, run = np.argsort(p), np.empty(len(p)), 0.0
    for j, i in enumerate(o):
        run = max(run, min(1.0, (len(p) - j) * p[i]))
        adj[i] = run
    return adj


def contrast(W, ev, arm, ref, o, rng):
    d = (W.xs(arm, level="arm")[o] - W.xs(ref, level="arm")[o]).reindex(ev).dropna()
    s = SIGN[o]
    r = dict(arm=ARMS.get(arm, arm), reference=ref, outcome=o, events=len(d), seeds=d.index.get_level_values("seed").nunique(),
             mean=d.mean(), nonzero=int((d.abs() > 1e-12).sum()), benefit=int((s * d > 1e-12).sum()), harm=int((s * d < -1e-12).sum()))
    if r["nonzero"] == 0:
        r.update(degenerate=True, cp_upper_share_nonzero=1 - 0.05 ** (1 / len(d)) if len(d) else np.nan)
        return r
    est, se, df, G, N = clustered(d)
    if not se > 0:
        r.update(degenerate=True)
        return r
    T = stats.t(df)
    pH, cat, p = tests(s * est, se, df)
    lo, hi, blo, bhi = boot(d, rng)
    h90 = T.ppf(0.95) * se
    b_lo, b_hi = s * est - h90, s * est + h90
    ci_cat = "helps" if b_lo > DELTA else "harms" if b_hi < -DELTA else "equivalent" if (b_lo > -DELTA and b_hi < DELTA) else "inconclusive"
    r.update(degenerate=False, est=est, se=se, df=df, t95_lo=est - T.ppf(0.975) * se, t95_hi=est + T.ppf(0.975) * se,
             boot_pct_lo=lo, boot_pct_hi=hi, boot_bca_lo=blo, boot_bca_hi=bhi, p_helps=p["helps"], p_harms=p["harms"],
             p_equiv=p["equivalent"], p_H=pH, best_category=cat, SENS_ci_inclusion_category=ci_cat)
    return r


def cp(k, n):
    lo = stats.beta.ppf(0.025, k, n - k + 1) if k > 0 else 0.0
    hi = stats.beta.ppf(0.975, k + 1, n - k) if k < n else 1.0
    return lo, hi


def main():
    os.makedirs(OUT, exist_ok=True)
    final = os.path.exists(os.path.join(E1, "run.log")) and "E1 DONE" in open(os.path.join(E1, "run.log")).read()
    label = "FINAL" if final else "INTERIM (monitoring only; no inferential claims)"
    T, st = table()
    T.to_csv(os.path.join(OUT, "e1_runs.csv"), index=False)
    EV = pd.DataFrame([json.loads(x) for x in open(os.path.join(E1, "events.jsonl"))])
    EV.to_csv(os.path.join(OUT, "e1_events_all_sampled_clusters.csv"), index=False)
    K = ["pos", "rate_a", "rate_b", "seed"]
    W = T.set_index(K + ["arm"]).sort_index()
    ok = EV[~EV.ref_failed]
    ev = pd.MultiIndex.from_frame(ok[ok.event][K])
    rng = np.random.default_rng(20261124)
    P = pd.DataFrame([contrast(W, ev, a, "B3", o, rng) for a in ARMS for o in ("G", "V")])
    nd = ~P.degenerate
    P["holm_p"], P["category"] = np.nan, np.where(P.degenerate, "no difference observed", "inconclusive")
    if nd.any():
        P.loc[nd, "holm_p"] = holm(P.loc[nd, "p_H"])
        rej = nd & (P.holm_p <= ALPHA)
        P.loc[rej, "category"] = P.loc[rej, "best_category"]
    if not final:
        P["category"] = "INTERIM - not interpreted"
    P.to_csv(os.path.join(OUT, "e1_primary.csv"), index=False)
    S = pd.DataFrame([contrast(W, ev, a, ref, o, rng) for a in ARMS for (ref, o) in
                      [("B3h", "G"), ("B3h", "V"), ("B3", "min_user_frac"), ("B3", "viol_edge"), ("B3", "etx")]])
    S.to_csv(os.path.join(OUT, "e1_secondary_DESCRIPTIVE.csv"), index=False)
    # decision consequences (descriptive)
    dc = []
    for a in ARMS:
        X = W.xs(a, level="arm").reindex(ev)
        Bx = W.xs("B3", level="arm").reindex(ev)
        hrr, tpr = X.handover_reject_reasons.fillna("").astype(str), X.tx_power_reject_reasons.fillna("").astype(str)
        ts_sup = hrr.str.contains("b4sa_priority_tpc|b5sa_choice", regex=True)
        tpc_sup = tpr.str.contains("b4sa_priority_ts|b5sa_choice", regex=True)
        typ = np.where(ts_sup & tpc_sup, "both suppressed", np.where(ts_sup, "TS suppressed", np.where(tpc_sup, "TPC suppressed", "no intervention")))
        for t in sorted(set(typ)):
            m = typ == t
            dc.append(dict(arm=ARMS[a], decision=t, events=int(m.sum()), dG_mean=float((X.G - Bx.G)[m].mean()),
                           dV_mean=float((X.V - Bx.V)[m].mean()), dMinUser_mean=float((X.min_user_frac - Bx.min_user_frac)[m].mean()),
                           ho_completed_arm=float(X.ho_completed[m].mean()), ho_completed_B3=float(Bx.ho_completed[m].mean()),
                           tpc_reduce_arm=float(X.tpc_reduce[m].mean()), tpc_reduce_B3=float(Bx.tpc_reduce[m].mean())))
    pd.DataFrame(dc).to_csv(os.path.join(OUT, "e1_decision_consequences_DESCRIPTIVE.csv"), index=False)
    # flag rates and relation to S7
    fr = []
    for (x, ra, rb), g in ok.groupby(["pos", "rate_a", "rate_b"]):
        k, n = int(g.event.sum()), len(g)
        lo, hi = cp(k, n)
        s7k, s7n = S7_RATES[(x, ra, rb)]
        fr.append(dict(pos=x, rate_a=ra, rate_b=rb, events=k, clusters=n, rate=k / n, cp_lo=lo, cp_hi=hi, s7_events=s7k, s7_clusters=s7n,
                       b3_b3h_flag_disagreements=int((g.b3_flag != g.b3h_flag).sum())))
    FR = pd.DataFrame(fr)
    FR.to_csv(os.path.join(OUT, "e1_flag_rates.csv"), index=False)
    allc = pd.MultiIndex.from_frame(ok[K])
    pop = []
    for a in ARMS:
        for o in ("G", "V"):
            d = (W.xs(a, level="arm")[o] - W.xs("B3", level="arm")[o]).reindex(allc).dropna()
            est, se, df, G, N = clustered(d)
            pop.append(dict(arm=ARMS[a], outcome=o, sampled_clusters=N, mean_all_sampled=est,
                            t95_lo=est - stats.t.ppf(.975, df) * se if se > 0 else est, t95_hi=est + stats.t.ppf(.975, df) * se if se > 0 else est,
                            event_rate=len(ev) / N, conditional_mean=float(d.reindex(ev).mean()), nonevent_mean=float(d.drop(ev, errors="ignore").mean())))
    pd.DataFrame(pop).to_csv(os.path.join(OUT, "e1_sampled_population_DESCRIPTIVE.csv"), index=False)
    # figures
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.8))
        for ax, o in zip(axes, ("G", "V")):
            q = P[P.outcome == o].reset_index(drop=True)
            ax.axvspan(-DELTA, DELTA, color="#e6e6e6", lw=0)
            ax.axvline(0, color="#888", lw=0.8)
            for i, r in q.iterrows():
                if r.degenerate:
                    ax.plot(0, i, "o", color="#222", mfc="white")
                else:
                    ax.errorbar(r.est, i, xerr=[[r.est - r.t95_lo], [r.t95_hi - r.est]], fmt="o", color="#0072B2", capsize=2)
            ax.set_yticks(range(len(q)))
            ax.set_yticklabels(q.arm, fontsize=7)
            ax.set_title(("Δ relative goodput" if o == "G" else "Δ violation fraction") + " vs B3 (events)", fontsize=7)
            ax.tick_params(labelsize=6)
        fig.suptitle(f"E1 conditional effects among reference-flagged events (95 % cluster-robust t CI; grey ±δ) [{label.split()[0]}]", fontsize=7)
        fig.tight_layout()
        fig.savefig(os.path.join(OUT, "fig_e1_conditional_effects.pdf"))
        plt.close(fig)
        e = ok.sort_values("seed").assign(cum=lambda z: z.event.cumsum())
        fig, ax = plt.subplots(figsize=(4.5, 2.4))
        ax.plot(e.seed.values, e.cum.values, color="#0072B2")
        ax.axhline(150, color="#D55E00", ls="--", lw=1)
        ax.set_xlabel("seed", fontsize=7)
        ax.set_ylabel("cumulative events", fontsize=7)
        ax.tick_params(labelsize=6)
        fig.tight_layout()
        fig.savefig(os.path.join(OUT, "fig_e1_event_accrual.pdf"))
        plt.close(fig)
    except Exception as ex:  # figures never block tables
        print("FIGURE ERROR", repr(ex))
    me = open(os.path.abspath(__file__), "rb").read()
    json.dump(dict(label=label, events=len(ev), sampled_clusters=len(ok), failed_reference_clusters=int(EV.ref_failed.sum()),
                   seeds=int(ok.seed.nunique()), runs=len(T), derived=int((T.status == "derived").sum()),
                   script_sha256=hashlib.sha256(me).hexdigest(), delta=DELTA, alpha=ALPHA, bootstrap_B=B),
              open(os.path.join(OUT, "provenance.json"), "w"), indent=1)
    with open(os.path.join(OUT, "summary.txt"), "w") as f:
        f.write(f"S8-E1 {label}\nevents {len(ev)} / sampled clusters {len(ok)}\n\n")
        f.write(FR.round(4).to_string(index=False) + "\n\n")
        cols = [c for c in ["arm", "outcome", "events", "seeds", "est", "t95_lo", "t95_hi", "nonzero", "benefit", "harm", "holm_p", "category",
                            "SENS_ci_inclusion_category", "cp_upper_share_nonzero"] if c in P]
        f.write(P[cols].round(5).to_string(index=False) + "\n")
    print(open(os.path.join(OUT, "summary.txt")).read())


if __name__ == "__main__":
    main()
