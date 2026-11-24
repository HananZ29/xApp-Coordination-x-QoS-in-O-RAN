# S7 consolidated report (FINAL, 2026-11-24)

- Registry: `docs/methodology/S7-registry.md`, frozen at commit `18e8985`.
- Analysis: `analysis/s7/s7_analysis.py`, sha256 `f5d92e4a…`, unchanged since the freeze.
- Generated tables: `manuscript/final/results_tables.md`.
- Manuscript: `manuscript/final/paper.md`.

## 1. Execution

| Item | Value |
|---|---|
| Seeds | 1001–1013 (13), all 536 cells each → **6 968 cells** |
| Simulated | **2 579**, all completed: 0 failed, 0 retries |
| Derived (verified identity rules) | **4 389**: U-LZ 843, U-LZh 3 402, U-B0 144 |
| Block-1 rule checks (seed 1001, all simulated) | U-LZ/U-LZh 354/354, U-EH 236/236, U-B0 12/12; narrowest rule (LZ) everywhere, no fallback needed |
| Provenance | every batch on commit 18e8985 with a clean tree, and one binary (sha `51633f7f…`) |
| Post-run integrity | derived-record source hashes re-checked: 0 mismatches. Waves 1001–1008 re-hashed after the host restart: 37 531 files, 0 mismatches |
| Incident | host restart during wave s7w1009: 156 runs kept after verification, 11 re-run (`S7-recovery.md`) |
| Wall clock | 01:48–05:01 and 08:39–10:19 EET |

Cosmetic: `s7wfinal` appears twice in `state.json` "waves", because both `run_wave` and `main` append it. There is no effect on data.

## 2. Confirmatory results (n = 13, δ = 0.01, seed-block BCa 95 % CI, Holm)

**Gatekeeping and classification**
- Gatekeeping passed at every stage. **All 80 hypotheses are classified *equivalent*.**
- Every paired-t sensitivity CI lies inside ±δ.

| Family | Result |
|---|---|
| **F1** arm − B3 @ D1 | Δ rel. goodput between +0.0000 and +0.0010 (B4sa-TPC [+0.0000, +0.0031]); Δ violation exactly 0 in all 156 clusters for every arm. Holm p 0.0024 |
| **F2** arm − B3 @ D0 | same picture; B4sa-TPC and B5sa violation −0.0002 [−0.0012, +0.0000]. No cluster harmed on a primary outcome |
| **F3** arbitration (arm − B3h) | identical to F1 up to a 125-ms hold. At 250 ms both signs appear (TS priority helps violations in 14/156 clusters; TPC priority and B5 lower goodput in 14/156), all inside ±δ. Holm p 0.0096 |
| **F4** overhead (B3h − B3@D1) | 131 µs: exactly 0. Holds of 50 / 125 / 250 ms raise the violation fraction by +0.0043 [0.0021, 0.0067] / +0.0067 [0.0046, 0.0083] / +0.0070 [0.0045, 0.0091]: **non-zero but inside δ** |

## 3. Descriptive and supplementary results

**Total delay curve.** It tracks the B3h overhead for every arm, so the hold, not the arbitration rule, drives the total effect.

**Family A (vs B0)**

| Arm | Δ violation | Other effects |
|---|---|---|
| B1 (TS) | −0.063 | min-user fraction −0.047 |
| B2 (TPC) | −0.020 | energy −7.9 W·s |
| B3 (both, uncoordinated) | −0.082 [−0.090, −0.075] | 0 harmed clusters |

**Reversed B3 order:** identical to B3 in 156/156 clusters.

**C3 load step**
- B4sa-TPC and B5sa are worse than B3h in condition x55 / 0.29 in all 13 seeds: violation +0.054, post-step violation +0.092. The cause is blocked TS handovers.
  - B4sa-TPC: 3 rejections on priority.
  - B5sa: 3 no-action choices.
- B4sa-TS: no effect.

**Event coverage** (state-aware flag in the reference run)

| Configuration | Flagged clusters | Status |
|---|---|---|
| D0 | 7/156 | UNDER-COVERED |
| D1 | 6/156 | UNDER-COVERED |
| D1 + 50 ms | 6/156 | UNDER-COVERED |
| D1 + 125 ms | 7/156 | UNDER-COVERED |
| D1 + 250 ms | 21/156 | ≥ 10 |
| C3 | 13/52 | ≥ 10 |

No seeds were added.
- In the under-covered clusters, the conditional effects of the state-aware arms are benefit-only (for example B5sa +0.0155 relative goodput in 6/6). This is **not claimable**.

**Post-hoc diagnostics** (not registered): `manuscript/final/posthoc_diagnostics.md`.
- The overhead concentrates in rate_a 0.92 / rate_b 1.43, through hold-induced handovers.
- R0 seeds are near-deterministic: 3–4 of 12 conditions give a single value across all seeds.

## 4. Limitations (full list in paper §7)

- R0 only: R1 is unvalidated.
- E2 delay is modelled; costs come from a single host; energy is an RF proxy.
- Interactions are rare, so event coverage is under-covered up to 125 ms.
- Near-deterministic seeds: the CIs describe within-R0 variability, and some are degenerate.
- B5 is not a QoS guarantee, and no superiority is claimed (Option A).
- The delay sweep induces its own interactions.

## 5. Submission package (2026-11-24, 11:05 EET)

The submission PDF is `manuscript/final/submission/paper_ieee.pdf`: IEEE conference two-column layout, 7 pages, 5 figures, 3 tables, 3 equations, 15 references. The IEEEtran LaTeX source is `paper_ieee.tex`. Details are in `manuscript/final/submission/README.md`.

**Remaining tasks (Hanan)**
1. Read and approve the final text, especially the Discussion interpretations. They are worded as interpretation and are not confirmatory.
2. Confirm the venue and template. IEEE conference (IEEEtran) was assumed, because no required template is recorded in the repository.
3. Compile `paper_ieee.tex` once with pdflatex, for example on Overleaf. No LaTeX was available here, and installing tools was not permitted in this session.
4. Supply the COMIX author first names if initials are required.
5. Choose the supplementary material: `results_tables.md` (the full tables) and `posthoc_diagnostics.md`.
