# S8-E2 technical report (FINAL analysis; registry `docs/methodology/S8-registry.md` §2)

**Purpose:** technical evidence only. Interpretation is for the author.

**Sources**
- Code commit `d3d19f8` for all E2 runs. It adds only the E2 analysis to E1's `e83a7c8`; executor and binary are unchanged.
- Analysis `analysis/s8/e2_analysis.py`, sha256 `ad10d8ef…`, label FINAL.
- Outputs in `results/s8/e2/analysis/`.

## 1. Execution

| Item | Value |
|---|---|
| Variants | V0 24 dBm / 3 dB, V1 27 / 3, V2 24 / 6 (`MinDbm` / `StepDb` attributes; nothing else changed) |
| Engineering check (seed 3999, excluded) | `power.csv` shows V0 min 24 / steps 3; V1 27 / 3; V2 24 / 6. **All OK** |
| Seeds | 3001–3026, shared across variants |
| Conditions | 6 (rate_b 0.29), at D1 |
| Cells | 3 276 = 2 061 simulated (all completed: 0 failed, 0 retries) + 1 215 derived (U-LZh) |
| Reuse verification (seed 3001, every arm simulated) | 15/15 identical for each variant |
| Wall clock | 16:53–20:52 EET |

## 2. E2-A: interaction frequency (B3h@D1 combined flag, 156 paired clusters)

| Variant vs V0 | Flagged V0 | Flagged V_k | Only V0 / only V_k | Exact McNemar p | Holm p |
|---|---|---|---|---|---|
| V1 (27 dBm floor) | 17 | 14 | 3 / 0 | 0.25 | 0.50 |
| V2 (6 dB step) | 17 | 17 | 0 / 0 | 1.00 | 1.00 |

No significant change in interaction frequency. With 17 flagged clusters, the test can detect only large frequency changes.

## 3. E2-B: robustness of coordination effects

Interaction contrast I = [arm − B3h](V_k) − [arm − B3h](V0); seed-level t, df = 25; δ = 0.01; Holm over the non-degenerate hypotheses.

| Variant | Arm | Outcome | Î | 95 % CI | Non-zero clusters | Category |
|---|---|---|---|---|---|---|
| V1 | B4sa-TS | G | – | – | 0 / 156 | no difference observed (CP upper share 0.019) |
| V1 | B4sa-TS | V | – | – | 0 / 156 | no difference observed (0.019) |
| V1 | B4sa-TPC | G | +0.00044 | [−0.00002, +0.00091] | 9 | equivalent |
| V1 | B4sa-TPC | V | +0.00017 | [−0.00006, +0.00041] | 3 | equivalent |
| V1 | B5sa | G | +0.00033 | [−0.00007, +0.00073] | 8 | equivalent |
| V1 | B5sa | V | +0.00017 | [−0.00006, +0.00041] | 3 | equivalent |
| V2 | B4sa-TS | G | −0.00017 | [−0.00025, −0.00009] | 13 | equivalent |
| V2 | B4sa-TS | V | +0.00013 | [−0.00004, +0.00030] | 3 | equivalent |
| V2 | B4sa-TPC | G | −0.00016 | [−0.00033, −0.00000] | 9 | equivalent |
| V2 | B4sa-TPC | V | +0.00015 | [−0.00012, +0.00041] | 5 | equivalent |
| V2 | B5sa | G | −0.00026 | [−0.00041, −0.00010] | 17 | equivalent |
| V2 | B5sa | V | +0.00025 | [−0.00003, +0.00053] | 6 | equivalent |

Every interval lies inside ±0.001, i.e. within a tenth of the margin.

## 4. Descriptive

**arm − B3h, all clusters / flagged clusters only** (relative goodput)

| Arm | V0 | V1 | V2 |
|---|---|---|---|
| B4sa-TPC | +0.0032 / +0.029 | +0.0036 / +0.041 | +0.0030 / +0.028 |
| B5sa | +0.0028 / +0.026 | +0.0031 / +0.035 | +0.0025 / +0.023 |

The flagged-only means rest on 14–17 clusters.

**TPC alone and joint operation vs B0** (all clusters)

| Contrast | Outcome | V0 | V1 | V2 |
|---|---|---|---|---|
| B2 − B0 | G | +0.0101 | +0.0147 | +0.0095 |
| B2 − B0 | V | −0.039 | −0.041 | −0.039 |
| B2 − B0 | energy (W·s) | −15.5 | −10.7 | −16.1 |
| B3 − B0 | V | −0.057 | −0.058 | −0.057 |
| B3 − B0 | energy (W·s) | −10.4 | −7.4 | −11.0 |

**TPC activity in B2:** reductions per run 2.0 / 1.0 / 1.0 (V0 / V1 / V2); time at reduced power in cell 2 is 21.5 s in all three. V1 and V2 reach their final power in one command.

## 5. Scope

- R0, the 6 TPC-active conditions, D1, frozen controllers.
- The robustness statement covers only floor 27 vs 24 dBm and step 6 vs 3 dB.
- E2 uses all-cluster contrasts. Interaction events are 9–11 % of clusters, so the conditional (event) effects under each variant are descriptive only.
