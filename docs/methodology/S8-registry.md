# S8 registry: FROZEN before any S8 output (approved by Hanan, 2026-11-24)

**Supersedes the draft** `S8-preregistration-DRAFT.md`. The draft is kept unchanged as history. Where this registry differs from the draft, the difference is listed in §4.

**Frozen code**
- `scripts/run/s8_run.py` (executor).
- `analysis/s8/e1_analysis.py` (E1 analysis).
- `analysis/s8/power_check.py` (pre-launch power).
- They are committed together with this file, and the commit hash is recorded in every S8 run manifest.
- The E2 analysis (`analysis/s8/e2_analysis.py`) implements §2.4 exactly. It is written during E1 and committed **before E2 starts**, with no S8 output inspected beyond integrity.

**Unchanged from S7** (`docs/methodology/methodology-reference.md`)
- Binary, controllers (frozen; Option A), D1 timing and C-A.
- The R0 radio model, layouts, CBR traffic.
- Outcome definitions G and V over (5, 29.5] s, α = 0.95.
- Provenance via `run_batch`.

**No controller, threshold, or traffic change is made to increase conflict frequency.**

## 1. E1: Event-enriched conditional study (primary)

**Conditions**
- x45 rate_a 1.23 / rate_b 0.29, and x55 0.92 / 0.29, at D1.
- **Selection disclosure:** these are the 2 of the 12 S7 conditions in which both xApps executed actions in B3. They were chosen from S7 *activity* evidence, not from effects.
- No inference extends to other conditions.

**Seeds:** fresh, 2001–2430 (never used), in blocks of 20.

**Runs per (condition, seed)**
- **B3@D1 and B3h@D1 are always simulated and kept,** including clusters without an event.
- Arms B4sa-TS, B4sa-TPC and B5sa (D1) are simulated if B3 or B3h is combined-flagged, and for every cluster of the verification block (seeds 2001–2020).
- **Otherwise** a derived record is written (U-LZh: arm = B3h). The rule was verified in S7 (354/354) and is re-verified on the verification block. If any counter-example appears, every later arm run is simulated.

**Event**
- A cluster whose **B3@D1 reference** run has ≥ 1 row in `interact.csv` with `definition == combined` and `flagged == 1`.
- It is defined on the uncoordinated reference trajectory only, never on any QoS outcome or arm run.
- In S7, B3 and B3h at D1 flag identically in 156/156 clusters. Any disagreement is reported.

**Stopping rule** (outcome-blind)
- A new block is launched only while the event count over completed blocks is **< 150**. All events of the last block are kept.
- The cap is seed 2430.
- If the cap is reached first, the result is reported as **below target**, with its achieved precision.
- A failed reference run (after one retry) counts as no event. It is retained and reported.

**Estimand:** θ(a, o) = mean over events of [Y_o(a) − Y_o(B3)] at D1, for a ∈ {B4sa-TS, B4sa-TPC, B5sa} and o ∈ {G, V}. This is a conditional effect among reference-flagged episodes, **distinct from the S7 all-run population estimand**.

**Primary inference** (`e1_analysis.py`)
- Seed-cluster-robust t: θ̂ = Σ S_s / Σ n_s, with CR1 variance and df = G − 1, where G is the number of seeds with events. This accounts for multiple events from the same seed.
- Margin **δ = 0.01** for both outcomes.
- Classification as in S7: p_help, p_harm and the TOST p_eq; p_H = 2·min; Holm over the non-degenerate hypotheses (≤ 6), at α = 0.05. Otherwise "inconclusive".
- **Degenerate case** (all event differences are 0): reported as "no difference observed in n/n", with the Clopper–Pearson upper bound on the share of non-zero differences. It is not given an equivalence label.

**Sensitivity (labelled)**
- Seed-block bootstrap (B = 10 000, RNG 20261124): percentile and BCa intervals.
- A CI-inclusion classification using the 90 % t interval.

**Secondary and descriptive outcomes**
- θ against B3h.
- Weakest-user service (min-user fraction), edge-UE V, energy proxy.
- **Decision consequences per event:** TS suppressed, TPC suppressed, or no intervention, read from the coordinator rejection and choice logs. For each type: outcome differences and the completed handovers and TPC reductions in the arm vs B3.
- Flag rate per condition (Clopper–Pearson) against S7.
- The sampled-condition effect over **all** sampled clusters, with its decomposition into event rate × conditional mean.

**Interpretation is fixed:** helps / harms / equivalent / inconclusive / no difference observed. Nothing is generalised beyond the two conditions, R0 and these controllers.

**Validated power** (`power_check.py`; normal approximation; Holm step 6 to 1; S7-consistent factor 2)

| Planning SD σ | 95 % half-width at N = 150 | Equivalence power (θ = 0) | "Helps"/"harms" power, \|θ\| = 0.02 | \|θ\| = 0.025 | \|θ\| = 0.03 |
|---|---|---|---|---|---|
| 0.020 | 0.0032 | 1.00 | 1.00 | 1.00 | 1.00 |
| **0.042** (S7 max, n = 6) | 0.0067 | **0.22–0.66** | 0.61–0.83 | 0.96–0.99 | 1.00 |
| 0.060 | 0.0096 | 0.00–0.06 | 0.28–0.53 | 0.66–0.86 | 0.93–0.98 |

**Equivalence and effect detection are not interchangeable.**
- At N = 150 the design is well powered to detect an effect of 0.025 or more.
- It is **underpowered to establish equivalence unless σ ≤ about 0.03**.
- Inconclusive results are reported as such. No seeds are added beyond the cap.

**Expected runtime**
- About 5.4 runs per seed, plus about 60 verification-block runs.
- At a flag rate of about 0.46 per seed (S7, x45 condition), 150 events take about 330 seeds, i.e. about 1 800 runs, about 3.4 h at 535 runs/h.
- At the cap: about 2 300 runs, about 4.3 h.

## 2. E2: TPC sensitivity (robustness, second)

| Variant | Floor | Step | Setting |
|---|---|---|---|
| V0 | 24 dBm | 3 dB | defaults |
| V1 | 27 dBm | 3 dB | `--ns3::OranLmXappTpc::MinDbm=27` |
| V2 | 24 dBm | 6 dB | `--ns3::OranLmXappTpc::StepDb=6` |

**Design**
- The variants are the values registered in S5. TPC's objective and safeguards are unchanged, and nothing else changes.
- **Conditions:** the 6 with rate_b 0.29, at D1.
- **Seeds:** fresh, 3001–3026, shared across variants.
- **Always simulated:** B0, B2, B3, B3h.
- **Arms** B4sa-TS, B4sa-TPC, B5sa: simulated if B3h is combined-flagged, and on verification seed 3001 for every variant. Otherwise derived (U-LZh). Group-1 derivations wait until the per-variant verification.
- **Engineering check first:** seed 3999, B2, x45 0.51/0.29, every variant, excluded from the analysis. `power.csv` must show the variant's floor and step. On failure, E2 is blocked and reported.

**2.4 Hypotheses**

| Family | Hypotheses | Test |
|---|---|---|
| **E2-A** (interaction frequency) | V1 vs V0 and V2 vs V0 | two-sided exact McNemar on cluster-level B3h@D1 combined flags (156 paired clusters) |
| **E2-B** (robustness of coordination effects; Holm over 12) | V_k ∈ {V1, V2}, a ∈ {B4sa-TS, B4sa-TPC, B5sa}, o ∈ {G, V}, with interaction contrast I = mean over clusters of [Y(a) − Y(B3h)](V_k) − [Y(a) − Y(B3h)](V0) | seed-block t; the S7 classification at δ = 0.01. Degenerate cases are handled as in E1 |

- **Descriptive:** arm − B3h per variant (all clusters and flagged-only); B2/B3 − B0 for G, V, min-user fraction and energy; TPC command counts.
- **Power:** S7 seed-mean SD 0.0050 × √2 at 26 seeds gives SE 0.0014, a 95 % half-width of 0.0029, and equivalence power ≈ 1.00. 26 seeds is therefore adequate, not excessive. (At 13 seeds the power would be 0.86–0.99.)
- **Runtime:** about 1 870 reference runs plus about 200 arm and verification runs, about 2 070 runs, about 3.9 h.

## 3. Execution order and incompleteness rule

1. E1 runs to its stopping rule.
2. E2 starts only after E1 completes, and only if the remaining time allows it to **complete**. It is never presented as complete when only a subset has run. A partial E2 is labelled incomplete, with no inference.
3. E3, E5 and the delay sweeps are not run.
4. INTERIM analyses are for monitoring only.

## 4. Differences from the draft (decided at approval, before any output)

- **Event source:** B3@D1 (the uncoordinated reference), as instructed. The draft used B3h@D1. They agree in 156/156 S7 clusters.
- **Primary comparator:** B3 (paired with the event source). B3h is secondary.
- **D0** is not run in E1 (it was secondary in the draft), to protect E1 throughput.
- **Blocks of 20 seeds** (draft: 10), for parallel efficiency. The overshoot of the target is bounded by one block.
- **E1 power corrected:** the draft stated 0.80 equivalence power, ignoring the factor 2 and Holm. The correct value is 0.22–0.66 at σ = 0.042.
- **E2 power corrected:** the draft projected a half-width of 0.0076 at 13 seeds; the data-based value is 0.0043.
