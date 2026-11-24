# S7 confirmatory experiment registry (FROZEN before any S7 output)

- Approved by Hanan (G6 decision), 2026-11-24.
- Basis: `G6-freeze-proposal.md` (as approved, full design, **not** Option N).
- Executor: `scripts/run/s7_run.py`.
- Analysis: `analysis/s7/s7_analysis.py`.
- Both are committed together with this file; the commit hash is recorded in every run manifest.

## 1. Pre-launch checks requested at G6 (both passed)

### Check 1: dependence across conditions that share a seed (P4 data, 10 seeds × 12 conditions)

**Inference unit.** The paired bootstrap resamples **seed blocks** jointly across all 12 conditions. Policy pairing and condition structure are preserved, and seed–condition observations are never treated as independent.

**Effect of this choice.**
- Condition-centred seed means, for seed-block SE vs. cluster-iid SE:

  | Contrast | Outcome | SE ratio (block / iid) |
  |---|---|---|
  | b4sa-ts − B3 | rel. goodput | 1.05 |
  | b4sa-tpc − B3 | rel. goodput | 1.05 |
  | b4sa-tpc − B3 | violation | 1.05 |
  | b5sa − B3 | rel. goodput | 1.14 |
  | b5sa − B3 | violation | 1.05 |
  | B3 − B0 | rel. goodput | 0.91 |
  | B3 − B0 | violation | 1.15 |
  | B1 − B0 | rel. goodput / violation | 1.03 / 1.00 |

- Within-seed correlation across conditions is therefore weak.
- The design effect is at most 1.15.

**Pooled precision, re-checked with the seed block as the unit.**
- n_seeds = (1.96·SD_upper(seed means)/δ)², with the χ² upper limit at df 9.
- Confirmatory contrasts (arm − B3) need at most **2 seeds** for δ = 0.01.
- At n = 13, the projected 95 % half-width is:
  - violation fraction: 0.0037;
  - relative goodput: ≤ 0.0004.
- **The registered pooled precision criterion remains credible.**
- Disclosed limitation: the family-A contrasts (B1/B2/B3 − B0) on violation fraction would need 28–32 seeds for ±0.01. At n = 13 their half-width is about 0.015. They are descriptive only, and were never confirmatory.

### Check 2: timing decomposition and information access at the exact D1

Engineering runs: 16 runs on flagged P4 clusters, scratchpad only, never analysed for effects. Evidence is in `results/validation/g6-smoke/`.

**Realised timing at D1**

| Component | B3 | B3h | Arms |
|---|---|---|---|
| Report age at RIC | 6.0 ms | 6.0 ms | 6.0 ms |
| LM processing | 0.012 ms | 0.012 ms | 0.012 ms |
| Coordination hold | **0** | 0.131 ms | 0.131 ms |
| E2 downlink | 2.0 ms | 2.0 ms | 2.0 ms |
| xApp input age | 0.5 s | 0.5 s | 0.5 s |

Report age is 3 · 2 ms (semantics a).

**Information access (semantics b)**
- Every telemetry arrival lies 6 ms after the 0.5-s grid. No report arrives within the 12 µs processing window.
- So D1 processing gives the coordinator **no fresher input** than the xApps. Semantics (b) cannot arise at D1.
- B5sa input age is 0.500012 s, which equals the xApp age plus processing. The only "newer" input is the coordinator's own previous-command record (t + 0.143 ms), which is the same for every arm.
- B3h and the coordinated arms therefore see identical information. They differ only in arbitration.

**What the contrasts isolate**
- B3h − B3 at D1 differs **only in `power.csv` actuation timestamps** (+0.1 ms). It is a pure delay effect, with no information effect.
- So "overhead" (B3h − B3@D1) is pure coordination delay, and "arbitration" (arm − B3h) is pure decision.
- At D1 + Δ_k, the hold also delays actuation relative to the next cycle's reports. The overhead contrast is the *joint effect of the hold on the closed loop* (late actuation plus the next cycle's decisions being made on pre-actuation state). It is labelled as such, never as computation cost.

## 2. Frozen design

**Fixed elements**
- Seeds: 1001–1013 (n = 13, never used before).
- Conditions: 12 (x45 / x55 × rate_a 0.51 / 0.92 / 1.23 × rate_b 0.29 / 1.43).
- Common random numbers.
- Thresholds, traffic, positions and controller activation (C-A) are as in P4.

**Timing**
- D0: all components 0.
- D1:
  - E2 uplink 2 ms (modelled), via `TransmissionDelayRv`;
  - E2 downlink 2 ms (`--e2-cmd-delay`);
  - LM processing 12 µs (`--proc-delay`);
  - coordination hold 131 µs (`--coord-delay`) for every coordinated arm and B3h; B3 hold 0;
  - 0.5-s observation age.
- D1 + Δ_k: hold 131 µs + {50, 125, 250} ms.

**Cells**

| Block | Arms |
|---|---|
| D0 | B0, B1, B2, B3, b4ts, b4tpc, b5, b4sats, b4satpc, b5sa |
| D1 | the D0 arms + B3h |
| D1 + Δ_k | B3h + the 6 coordinated arms |
| C3 | D1 + load step (`--rate-step-time=15 --rate-a-after=1.23`; rate_a 0.51; 4 scenarios x45/x55 × rate_b) with B3, B3h, b4sats, b4satpc, b5sa |
| S8a | D0, B3 with `--coord-reverse=true` (reversed command order) |

- Size: **536 cells per seed.**
- Arguments at D0 are byte-identical to P4, apart from the seed (verified).

## 3. Execution rules (approved)

**Block 1** (seed 1001) simulates every cell, then verifies each rule per (configuration, detector family) on 16 output files.

| Rule | Statement | If block 1 shows a counter-example |
|---|---|---|
| U-LZ | At D0, an unflagged arm equals B3 | fall back to U-EH |
| U-LZh | At D1, D1 + Δ and C3, an unflagged arm equals B3h | fall back to U-EH |
| U-EH | Unflagged arms of a family equal the family leader | fall back to FULL (every arm simulated) |
| U-B0 | B0@D1 equals B0@D0 | B0@D1 is simulated |

**Seeds 1002–1013**
- Always simulated: 152 runs per seed.
- Conditional arms are simulated if their reference flags under the arm's detector, or if the reference failed.
- Otherwise a derived record names the source run and the sha256 of each file.

**Failures** are retried once in a separate batch. The failed outputs are kept.

**Expected size:** ≈ 2 708 runs, ≈ 5 h at 540 runs/h.

## 4. Analysis plan (implemented in `analysis/s7/s7_analysis.py`)

**Outcomes**
- **Co-primary:** relative goodput (mean per-UE goodput / offered) and QoS violation fraction (all UEs, 0.5-s windows, goodput < 0.95 × offered).
  - Window (5, 30] s.
  - For C3, the offered rate is time-varying.
- Margins δ = 0.01 for both.

**Estimator**
- The condition-stratified pooled paired mean, i.e. the mean over seeds of the per-seed mean over 12 conditions.
- Seed-block BCa 95 % CI (B = 10 000, RNG seed 20261124).
- Paired-t CI over the seed means, reported as a sensitivity column only.

**Categories** (protocol §9.2)
- Tests:
  - helps: H0 benefit ≤ δ;
  - harms: H0 harm ≤ δ;
  - equivalent: TOST on ±δ.
- p_H = 2 · min of the three.
- Holm within each family; a non-rejected H is labelled **inconclusive**.

**Families, in order, with serial gatekeeping**

| Family | Contrasts |
|---|---|
| F1 C1-D1 | arm − B3 |
| F2 C1-D0 | arm − B3 |
| F3 C2 arbitration | arm − B3h at D1 and at D1 + Δ_k |
| F4 C2 overhead | B3h@hold − B3@D1 |

**Per-hypothesis reporting**
- Counts of positive / zero / negative clusters.
- Exact sign test, both over clusters and over seeds.

**Descriptive only**
- C2 total curve (arm@D1+Δ − B3@D1).
- Per-condition contrasts.
- Family A.
- S8a.
- C3 (total, arbitration and overhead, plus post-step violation and recovery time).
- Secondary outcomes: min-user fraction, edge violation, RF energy proxy, RLF, handover reversals.

**SUPPLEMENTARY**
- The event-conditional analysis (reference run has ≥ 1 combined flag).
- **Under-covered** if fewer than 10 flagged clusters. No seeds are added.

**Interim outputs**
- They are produced while S7 runs and labelled INTERIM, for integrity monitoring only.
- Nothing is stopped, added or changed on their basis.

## 5. Commitments

1. No change to policies, QoS thresholds, traffic, delays, seeds or analysis after S7 outputs are observed.
   - A *software defect* in the analysis code may be fixed only with a documented diff and an unchanged method. During S7 the fixed copy runs outside the repository, and it is committed after S7.
2. Every run is retained: failures, negative results and zero-effect cases.
3. Claims cover R0 only. R1 is unvalidated.
4. No deployment-level latency or energy claims:
   - E2 = 2 ms is modelled;
   - T3/T4 are single-host measurements;
   - energy is an RF-power proxy.
