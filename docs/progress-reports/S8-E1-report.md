# S8-E1 technical report (FINAL analysis; registry `docs/methodology/S8-registry.md` §1)

**Purpose:** technical evidence only. Interpretation is for the author.

**Sources**
- Code commit `e83a7c8` for all E1 runs.
- Analysis `analysis/s8/e1_analysis.py`, sha256 `eb1f4f28…`, label FINAL.
- Outputs in `results/s8/e1/analysis/`.

## 1. Execution

| Item | Value |
|---|---|
| Seeds | 2001–2220: 11 blocks of 20. The stopping rule ended after block 11 (155 ≥ 150); the cap (2430) was not reached |
| Sampled clusters (condition × seed) | 440. Every B3@D1 and B3h@D1 reference run is kept, events or not |
| Simulated runs | 1 429, all completed: 0 failed, 0 retries |
| Derived runs | 771 (U-LZh) |
| Reuse verification (seeds 2001–2020, every arm simulated) | 84/84 identical |
| Event source agreement | B3 vs B3h flag disagreements: 0/440 |
| Validity screen | Minimum UL SINR over all simulated runs −1.50 dB: none below the −2.2 dB stuck-UE threshold. RLF events: 0 |
| Wall clock | 13:32–16:53 EET |

## 2. Event frequency

| Condition | Events / clusters | Rate [95 % Clopper–Pearson] | S7 (D1) |
|---|---|---|---|
| x45 1.23 / 0.29 | 112 / 220 | 0.509 [0.441, 0.577] | 6/13 |
| x55 0.92 / 0.29 | 43 / 220 | 0.195 [0.145, 0.254] | 0/13 |
| **Total** | **155 / 440** (132 seeds with ≥ 1 event) | 0.352 | |

## 3. Primary (confirmatory): arm − B3 at D1, among reference-flagged events

- Seed-cluster-robust t, df = 131; δ = 0.01; Holm over 6.
- Benefit orientation: G higher is better; V higher is worse.

| Arm | Outcome | θ̂ | 95 % t CI | 95 % BCa (sensitivity) | non-zero (benefit / harm) | Holm p | Category | CI-inclusion (sensitivity) |
|---|---|---|---|---|---|---|---|---|
| B4sa-TS | G | +0.0011 | [+0.0003, +0.0019] | [−0.0002, +0.0016] | 106 (102 / 4) | < 0.001 | **equivalent** | equivalent |
| B4sa-TS | V | +0.0016 | [−0.0011, +0.0044] | [−0.0003, +0.0062] | 6 (4 / 2) | < 0.001 | **equivalent** | equivalent |
| B4sa-TPC | G | +0.0210 | [+0.0154, +0.0265] | [+0.0158, +0.0270] | 83 (70 / 13) | 0.0006 | **helps** | helps |
| B4sa-TPC | V | +0.0104 | [+0.0039, +0.0169] | [+0.0047, +0.0177] | 45 (24 / 21) | 0.91 | **inconclusive** | inconclusive |
| B5sa | G | +0.0188 | [+0.0139, +0.0237] | [+0.0143, +0.0239] | 155 (143 / 12) | 0.0017 | **helps** | helps |
| B5sa | V | +0.0071 | [+0.0017, +0.0125] | [+0.0025, +0.0134] | 45 (26 / 19) | 0.59 | **inconclusive** | inconclusive |

**Notes**
- For V, both state-aware TPC-priority arms have **positive (worse) violation differences** whose 95 % CIs exclude 0. They are classified inconclusive because the CI overlaps the ±0.01 margin.
- No hypothesis was degenerate.
- Realised SD of event differences in G: 0.005 (B4sa-TS), 0.036 (B4sa-TPC), 0.031 (B5sa). These are within the planning value of 0.042.

## 4. Secondary (descriptive)

| Arm | Min-user fraction | Edge-UE V | Energy proxy (W·s) |
|---|---|---|---|
| B4sa-TS | −0.0072 [−0.0100, −0.0045] (104 of 106 non-zero events worse) | +0.0049 [−0.0032, +0.0130] | +0.43 [+0.27, +0.59] |
| B4sa-TPC | +0.0660 [+0.0474, +0.0845] | +0.0310 [+0.0115, +0.0506] | −5.27 [−6.25, −4.29] |
| B5sa | +0.0522 [+0.0354, +0.0689] | +0.0213 [+0.0050, +0.0375] | −3.82 [−4.71, −2.93] |

- **vs B3h:** identical to vs B3 for every arm (B3h ≡ B3 at D1).
- **Per condition (conditional means; descriptive only):**

  | Arm | x45 (112 events) | x55 (43 events) |
  |---|---|---|
  | B4sa-TPC | ΔG +0.0284, ΔV 0.0000, Δmin-user +0.093 | ΔG +0.0015, ΔV +0.0374, Δmin-user −0.004 |
  | B5sa | ΔG +0.0245, ΔV 0.0000, Δmin-user +0.069 | ΔG +0.0038, ΔV +0.0257, Δmin-user +0.009 |
  | B4sa-TS | ΔG +0.0022 | ΔG −0.0019, ΔV +0.0059 |

## 5. Decision consequences per event (descriptive)

`results/s8/e1/analysis/e1_decision_consequences_DESCRIPTIVE.csv`

| Arm | Decision | Events | ΔG | ΔV | Δmin-user | Handovers completed, arm / B3 | TPC reductions, arm / B3 |
|---|---|---|---|---|---|---|---|
| B4sa-TS | TPC suppressed | 105 | +0.0016 | +0.0024 | −0.0106 | 1.08 / 1.11 | 1.04 / 1.06 |
| B4sa-TS | no intervention | 50 | 0 | 0 | 0 | – | – |
| B4sa-TPC | TS suppressed | 83 | +0.0391 | +0.0193 | +0.1232 | 0.04 / 1.99 | 1.96 / 1.90 |
| B4sa-TPC | no intervention | 72 | 0 | 0 | 0 | – | – |
| B5sa | TPC suppressed | 75 | +0.0025 | −0.0005 | −0.0078 | 1.08 / 1.08 | 1.04 / 1.04 |
| B5sa | TS suppressed | 50 | +0.0239 | +0.0231 | +0.0746 | 0.00 / 2.40 | 2.00 / 2.38 |
| B5sa | both suppressed | 30 | +0.0508 | −0.0005 | +0.1647 | 0.47 / 1.20 | 2.57 / 1.10 |

**Mechanism spot-checks** (per-UE traces; illustrative, not statistical)
- **x45 (seed 2023):** in B3, the handed-over edge UE moves into cell B, which is power-reduced and then restored, and receives 0.79 Mbps. When the TS is suppressed, cell B stays at 24 dBm and all three edge UEs receive 0.99–1.05 Mbps. Every edge-UE window violates in both arms (threshold 0.95 × 1.23).
- **x55 (seed 2007):** B3 completes 3 handovers. When they are suppressed, the edge UEs stay at about 0.87 Mbps, just below the 0.874 threshold. Edge violation shares rise from 0.20–0.22 to 0.35–0.73, at almost unchanged goodput.

## 6. Relation to S7 (descriptive)

**Effect over all 440 sampled clusters** (unflagged clusters derived, Δ = 0):

| Arm | ΔG | ΔV |
|---|---|---|
| B4sa-TPC | +0.0074 [+0.0053, +0.0095] | +0.0037 [+0.0013, +0.0060] |
| B5sa | +0.0066 | +0.0025 |
| B4sa-TS | +0.0004 | +0.0006 |

Event rate 0.352.

**Implied contribution to S7's 12-condition pooled estimand** (only these 2 conditions host joint activity):

| Arm | Outcome | Implied | S7 F1 |
|---|---|---|---|
| B4sa-TPC | G | +0.0012 | +0.0010 |
| B4sa-TPC | V | +0.0006 | 0 (S7 sampled 0 x55 events) |
| B5sa | G | +0.0011 | +0.0006 |
| B5sa | V | +0.0004 | 0 |

- The violation effects fall within the S7 rare-event bound for the degenerate V hypotheses (≤ 0.0034).

## 7. Scope and limitations (as registered)

- The conditional estimand applies to reference-flagged episodes in the 2 conditions only. These conditions were selected from S7 activity evidence.
- R0, one fixed geometry, frozen controllers, D1 timing.
- The pooled conditional effect mixes two heterogeneous conditions (§4). The per-condition results are descriptive.
- Power: the realised precision (G half-width ≤ 0.0056) is adequate for the "helps" classifications. Equivalence was established only for B4sa-TS.
