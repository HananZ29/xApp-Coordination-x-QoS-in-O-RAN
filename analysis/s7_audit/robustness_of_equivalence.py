#!/usr/bin/env python3
"""S7 AUDIT: how robust are the 80 equivalence classifications? (read-only; uses audit outputs)
  * delta_star: the smallest margin at which t-based TOST (90 % t interval inside ±delta) would still hold
  * realised 95 % t half-widths vs the registered precision projection (viol 0.0037, rel. goodput <= 0.0004)
  * degenerate contrasts (all 156 cluster differences exactly 0): exact binomial upper bound on the share of
    clusters with any effect, and the implied bound on the mean if a non-zero cluster effect were as large as the
    largest |cluster delta| observed for that outcome anywhere in the confirmatory families
"""
import os
import numpy as np, pandas as pd
from scipy import stats
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
A = os.path.join(REPO, "results", "s7", "audit")
C = pd.read_csv(os.path.join(A, "independent_confirmatory.csv"))
C["delta_star"] = np.maximum(C.t90_lo.abs(), C.t90_hi.abs())
C["t95_halfwidth"] = (C.t90_hi - C.t90_lo) / 2 * stats.t.ppf(0.975, 12) / stats.t.ppf(0.95, 12)
ub = 1 - 0.05 ** (1 / 156)  # exact one-sided 95 % upper bound on a proportion with 0/156 events
mx = C.groupby("outcome").max_abs_cluster_delta.max()
C["rare_event_bound_on_mean"] = np.where(C.clusters_nonzero == 0, ub * C.outcome.map(mx), np.nan)
C.to_csv(os.path.join(A, "equivalence_robustness.csv"), index=False)
print("max delta_star by family/outcome:\n", C.groupby(["family", "outcome"]).delta_star.max().round(4).to_string())
print("\nworst 6:\n", C.sort_values("delta_star", ascending=False).head(6)[["family", "contrast", "delay", "outcome", "mean_delta", "t90_lo", "t90_hi", "delta_star"]].round(4).to_string(index=False))
print("\nrealised 95% t half-width (non-degenerate) by outcome: max", C[~C.degenerate].groupby("outcome").t95_halfwidth.max().round(4).to_dict(),
      " median", C[~C.degenerate].groupby("outcome").t95_halfwidth.median().round(5).to_dict())
print("\ndegenerate:", int(C.degenerate.sum()), "by family/outcome", C[C.degenerate].groupby(["family", "outcome"]).size().to_dict())
print("0/156 upper bound on cluster share %.4f; largest |cluster delta| by outcome %s; implied bound on mean %s" % (ub, mx.round(4).to_dict(), (ub * mx).round(4).to_dict()))
