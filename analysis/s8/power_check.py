#!/usr/bin/env python3
"""S8 pre-launch power/precision check (planning only; no S8 data).
E1: n events, planning SD sigma (S7 flagged-cluster SDs: max 0.042, D1; sensitivity 0.02/0.06), delta = 0.01.
    Registered S7-consistent classification: p_H = 2*min(p_help, p_harm, p_eq), Holm over the E1 family (6 hypotheses).
    Per-test one-sided level after the factor 2 and Holm: alpha' = 0.05 / (2 * m_rank), m_rank = 6 (first Holm step) .. 1.
    Equivalence power at theta = 0: 2*Phi(delta/SE - z_{1-alpha'}) - 1.  "Helps"/"harms" power at |theta| = theta1:
    Phi((theta1 - delta)/SE - z_{1-alpha'}).  Normal approximation (df >= 100 for n >= 150 at ~1 event/seed).
E2: interaction contrast I = (arm - B3h)(V_k) - (arm - B3h)(V0) per seed (mean over 6 conditions); SD of S7 seed means of
    arm - B3h over the 6 rate_b = 0.29 conditions (max 0.0050), times sqrt(2) (independent variants; conservative), t with
    df = n - 1, Holm over 12 TOST hypotheses (factor 2 as above)."""
import numpy as np
from scipy import stats
d = 0.01
print("E1 (delta 0.01)")
print(" sigma   n   SE      eq_power(Holm step 6..1)   helps_power theta1=0.02 / 0.025 / 0.03 (step 6..1)")
for s in (0.02, 0.042, 0.06):
    for n in (75, 150, 200):
        se = s / np.sqrt(n)
        eq = [2 * stats.norm.cdf(d / se - stats.norm.ppf(1 - 0.05 / (2 * m))) - 1 for m in (6, 1)]
        hp = [[stats.norm.cdf((t - d) / se - stats.norm.ppf(1 - 0.05 / (2 * m))) for m in (6, 1)] for t in (0.02, 0.025, 0.03)]
        print(f" {s:.3f} {n:4d} {se:.5f}  {max(eq[0],0):.2f}..{max(eq[1],0):.2f}     " + "  ".join(f"{a:.2f}..{b:.2f}" for a, b in hp))
print("\nE2 (delta 0.01; SD seed means 0.0050 x sqrt2)")
for n in (13, 26):
    se = 0.0050 * np.sqrt(2) / np.sqrt(n)
    for m in (12, 1):
        tq = stats.t.ppf(1 - 0.05 / (2 * m), n - 1)
        print(f" n {n}: SE {se:.4f}; equivalence power at I=0 (Holm step {m}): {2 * stats.t.cdf(d / se - tq, n - 1) - 1:.2f}; 95% half-width {stats.t.ppf(.975, n-1)*se:.4f}")
