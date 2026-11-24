# G5 review: coordination correctness (P-1.0 Gate G5), final

Date: 2026-11-23. Decision basis: Hanan chose **Option A** (2026-11-23).

**Frozen as evaluated:**
- B5 and B5sa;
- κ_η = 0.85;
- calibration ε: vulnerable 0.26606, low-demand 0.07604, already-violating 0.31076, other 0.00683;
- the all-user pessimism rule (S-a′ "all").

**Not changed:**
- estimator, safeguard, detectors and controller rules;
- traffic scenarios and power parameters.

## 1. Pre-specified criteria

**P-1.0 Gate G5** (protocol p. 9, verbatim):

> Hand-crafted unit tests show correct detection, priority resolution, QoS-aware choice, stale/invalid-command handling, and no future-information leakage. B3–B5 generate consistent event traces.

The S3 report gives the required G5 evidence (verbatim list):
- hand-crafted tests;
- a no-future-information audit, in which every B5 input timestamp is ≤ t_q;
- estimator MAE, bias and ranking accuracy on independent data;
- consistent B3–B5 traces on identical seeds.

The project's itemised list (S5-part1 §8, items 1–8) adds two requirements:
- measured coordination computation;
- non-degeneracy: "a design whose coordinated arms are identical to B3 in every condition fails G5".

These criteria define G5 as **correctness and causal validity**, not performance. Hanan's instruction is the same: "A negative performance result is acceptable; an unresolved correctness or causal-validity failure is not."

## 2. Verdict per criterion

Each requirement has its own subsection below. Every one is **MET**.

### 1. Detection (same-cycle and combined)

**Verdict: MET.**

Evidence:
- **KATs:**
  - DET-1..5, same-cycle;
  - LAB-1..9, combined / state / evidence / windows;
  - SA-1..8.
- **Scripted cross-cycle cases** (`s5sa` acceptance C.*):
  - pending vs completed handovers;
  - stale power;
  - gap 3 s, under combined, win_2 and win_5.
- **Coverage reported:**
  - combined: 2/24 independent runs;
  - same-cycle: 0/24.

### 2. Priority resolution

**Verdict: MET.**

Evidence:
- B4-TS-1, B4-TPC-1, B4-2, B4-3 (same-cycle, both directions).
- SA-1..8 (state-aware, asymmetric):
  - only the later proposal is rejected;
  - `priority_unenforceable` is logged.
- Integration: s5 25/25, s5sa C.*.

### 3. QoS-aware choice (B5 / B5sa) and estimator on independent data

**Verdict: MET as correctness.** B5's QoS performance is not established; see §3.

Evidence:
- **KATs:**
  - B5-K1..7: each candidate outcome, safeguard binding, empty A_safe → N, ties, unverifiable UEs;
  - B5SA-K1..5;
  - EST-1..6.
- **Online choice = offline choice:** 36/36.
- **Estimator on independent data:**
  - held-out seeds 304–311: MAE 0.033, bias +0.004, ε-coverage 0.94–0.95;
  - ranking accuracy: 0.33 under the frozen "all" pessimism;
  - sequential seeds 341–342: reported in full.

### 4. Stale / invalid command handling under coordination

**Verdict: MET.**

Evidence:
- V1(f) identity rejection after a coordinator pass: b3, b4sa, b5sa (`badrnti`).
- Pending and confirmed state.
- MaxUncertain / unverifiable → N (B5SA-K4).
- S3 V1/V2 cases: 121/121 on the current code (`results/validation/s3/20261123T172607Z`).

### 5. No future-information leakage

**Verdict: MET.**

Evidence:
- **Code audit:** the CMM and xApps read only:
  - the RIC store;
  - the lifecycle intended-IMSI;
  - the dispatched-command history;
  - the evidence registered by the xApps.
- **Logs:**
  - 163/163 B5/B5sa decisions have newest input ≤ t_c, with age 0.500 s;
  - 354/354 detector history and evidence references are ≤ t_c;
  - xApp decision logs are identical to B3 before the first differing intervention: 144/144 independent, 60/60 replay.
- **Ground truth:** used offline only.

### 6. Consistent B3–B5 event traces

**Verdict: MET.**

Evidence:
- Identical seeds and configurations give identical traces up to the first intervention, and identical KPIs in every run without one (`sa_*_identity.csv`).
- Reference-policy regression is byte-identical:
  - 10/10 runs (compute_us masked);
  - 180/180 data files after the `--ric-start` addition.

### 7. Measured coordination computation

**Verdict: MET.**

Evidence:
- **In simulation** (steady_clock, logged only):
  - B5: median 73 µs, p95 127 µs;
  - B5sa: median 82 µs, p95 131 µs.
- **Independent check** (`results/validation/preP3/20261123T172553Z`):
  - steady_clock and CPU clock agree within 1.5 %;
  - the 20 ms known interval measures correctly;
  - bench decision cost 14–31 µs;
  - TS 4–5 µs, TPC 5–6 µs.
- Wall-clock time on this host is never RIC latency.

### 8. Non-degenerate comparison

**Verdict: MET (minimal).**

Evidence:
- The coordinated arms are **not** identical to B3 in every condition: B4sa / B5sa differ in 2/24 independent clusters.
- Rarity was accepted as a finding (Hanan, part 2).
- **Same-cycle B4/B5 alone would still fail this item.** In independent runs they equal B3 everywhere (0/24).

### 9. No unresolved correctness or causal-validity failure

**Verdict: MET.**

Evidence:
- The one acceptance FAIL (`compute_us` byte compare) is a wall-clock column that is never read by any decision. It is validated independently (item 7).
- The seven §6 implementation choices are all causal (diagnosis §6).
- The diagnosed B5 behaviours are model limitations, not defects: the code does what the frozen specification says.

## 3. Scientific findings carried forward (Option A; not G5 failures)

These are reported as results and limitations. They are not defects.

1. **No QoS benefit is established for any coordinated arm.**
   - In the independent pilot (24 clusters), B4sa / B5sa differed from B3 in 2/24 runs.
   - No L3 events (RLF, handover failure, reversal) occurred in any arm.
   - The paired QoS deltas are single events of mixed sign.
2. **B5 does not guarantee QoS and does not outperform simpler coordination.** No such claim is made.
3. **All-user pessimism can overcount predicted violations.**
   - The ε deductions are applied to every user under any action candidate.
   - Every UE with demand below ε_low / (1 − α) ≈ 1.52 Mbps therefore adds one predicted violation to every action and none to N.
   - At cell-B demand of 0.29 Mbps this adds 3 to v̂ for every action. In both diagnosed cases, "4 vs 1" was 1 genuine predicted violation plus 3 counted by this rule.
4. **The estimator does not model subsequent xApp responses.** It scores one action over a 2–5 s horizon, with the other agents frozen.
   - **Immediate handover consequences** (both seeds, first 3 s after the handover; predicted in direction):
     - the steered UE drops to 0.25–0.73 Mbps in the reduced cell;
     - cell-2 PRB rises to about 0.93.
   - **Later closed-loop responses** (outside the model):
     - the TPC xApp restores cell 2 (24 → 27 → 30 dBm, at 14.5 / 15.5 s). This raises interference on the cell-1 edge UEs by 3–5 dB of SINR.
     - **Seed 222:** cell-1 HOL crossed the TS trigger (106 / 133 ms ≥ 78 ms). The TS xApp moved both remaining edge UEs, and allowing the first handover was beneficial over the run.
     - **Seed 202:** HOL stayed at 62 / 53 ms. There was no follow-up, the interference persisted, and allowing the handover was harmful over the run.
   - Neither seed is representative (n = 1 each).
5. **Sequential-state prediction for action candidates is less reliable than for no-action.**
   - Action candidates: MAE 0.046, bias −0.027, ε-coverage 0.88–0.90; steered-UE MAE about 0.25.
   - Held-out snapshot reference: MAE 0.033, coverage 0.94–0.95.
   - Decisions: 0/72 false-safe and 20/72 conservative misses.

## 4. Decision

**G5 (coordination correctness) is met.** Every pre-specified criterion has passing evidence, and no correctness or causal-validity failure remains open.

**What G5 does not certify**
- G5 certifies neither a QoS benefit nor B5's safety. Both are reported as negative or inconclusive performance results (§3).
- D1 values and the Δ grid are G6 items that depend on P3, per the S3 report: "Delay values for D1 fixed after the P3 timing pilot. The Δ-grid overlap rule is stated (U5)."

**Next steps under the instruction**
- Proceed to P3, then P4, in stages.
- The pre-launch record is `docs/progress-reports/P3-P4-prelaunch.md`.
- The confirmatory matrix stays unlaunched until the pilot-informed parameters and the analysis plan are frozen (G6/G7).
