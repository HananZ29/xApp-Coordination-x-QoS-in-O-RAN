#!/usr/bin/env python3
"""Generate the manuscript's result tables from the frozen S7 analysis outputs (no hand-transcribed numbers).
Input:  results/s7/analysis/*.csv, results/s7/{state.json, block1_rules.json, block1_rule_checks.csv, retries.jsonl}
Output: manuscript/final/results_tables.md
"""
import json
import os

import pandas as pd

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
A = os.path.join(REPO, "results", "s7", "analysis")
S7 = os.path.join(REPO, "results", "s7")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results_tables.md")
LAB = {"b4ts": "B4-TS (same-cycle)", "b4tpc": "B4-TPC (same-cycle)", "b5": "B5 (same-cycle)", "b4sats": "B4sa-TS",
       "b4satpc": "B4sa-TPC", "b5sa": "B5sa", "B3h": "B3h", "B1": "B1 (TS only)", "B2": "B2 (TPC only)", "B3": "B3",
       "B3rev": "B3 reversed order"}
OUTN = {"rel_goodput": "rel. goodput", "viol": "violation fraction", "min_user_frac": "min-user fraction",
        "viol_edge": "edge violation", "etx": "RF energy proxy (W·s)", "rlf": "RLF count", "ho_reversals": "HO reversals",
        "viol_post_step": "post-step violation", "recovery_s": "recovery time (s)"}
HOLD = {"D0": "0", "D1": "0.131 ms", "D1d1": "50.131 ms", "D1d2": "125.131 ms", "D1d3": "250.131 ms", "C3": "C3 (D1)",
        "S8a": "D0"}


def f(x, nd=4):
    return "—" if pd.isna(x) else f"{x:+.{nd}f}"


def ci(r, nd=4):
    return f"{f(r['mean'], nd)} [{f(r.ci_lo, nd)}, {f(r.ci_hi, nd)}]"


def arm_of(c):
    return LAB.get(c.split("-")[0], c.split("-")[0])


def table(D, cols, heads):
    s = "| " + " | ".join(heads) + " |\n|" + "---|" * len(heads) + "\n"
    for _, r in D.iterrows():
        s += "| " + " | ".join(c(r) for c in cols) + " |\n"
    return s


def main():
    P = json.load(open(os.path.join(A, "provenance.json")))
    st = json.load(open(os.path.join(S7, "state.json")))
    idx = pd.DataFrame(st["index"].values())
    md = [f"# S7 result tables (generated; {P['label']}; seeds {P['seeds'][0]}–{P['seeds'][-1]}, n = {len(P['seeds'])})\n",
          f"Analysis script sha256 `{P['script_sha256'][:16]}…`, bootstrap B = {P['bootstrap_B']}, δ = {P['delta']}, α = {P['alpha']}.\n",
          "Conventions: Δ = arm − reference in the outcome's own units, pooled over the 12 conditions (mean over seeds of the "
          "per-seed condition mean). For violation fractions, energy, RLF and reversals a positive Δ is WORSE. "
          "Cluster counts are benefit-oriented (benefit / exactly zero / harm). Categories follow protocol §9.2 at δ = 0.01.\n"]
    # T1 execution
    sim = idx[idx.status != "derived"]
    md.append("## Table 1. Execution\n")
    md.append(f"- Cells: {len(idx)} ({len(sim)} simulated, {int((idx.status == 'derived').sum())} derived by verified identity rules).")
    md.append(f"- Simulated runs completed: {int((sim.status == 'completed').sum())}; not completed: {int((sim.status != 'completed').sum())}; retried: {int(sim.get('retried', pd.Series(dtype=bool)).fillna(False).sum())}.")
    rr = json.load(open(os.path.join(S7, "block1_rules.json")))
    chk = pd.read_csv(os.path.join(S7, "block1_rule_checks.csv"))
    g = chk.groupby(["cfg", "det", "rule"]).identical.agg(["sum", "count"]).reset_index()
    md.append("\nBlock-1 (seed 1001, every cell simulated) identity checks per configuration and detector family:\n")
    md.append(table(g, [lambda r: r.cfg, lambda r: r.det, lambda r: r.rule, lambda r: f"{int(r['sum'])}/{int(r['count'])}"],
                    ["configuration", "detector family", "rule", "identical"]))
    md.append("Rules applied from seed 1002: " + ", ".join(f"{k} → {v}" for k, v in rr["rules"].items())
              + f"; U-B0 {'applied' if rr['ub0'] else 'not applied'}.\n")
    # T2 confirmatory F1/F2
    C = pd.read_csv(os.path.join(A, "confirmatory_contrasts.csv"))
    cols = [lambda r: arm_of(r.contrast), lambda r: OUTN[r.outcome], lambda r: ci(r),
            lambda r: f"{int(r.clusters_benefit)}/{int(r.clusters_zero)}/{int(r.clusters_harm)}",
            lambda r: f"{r.holm_p:.3g}", lambda r: r.category + ("" if r.confirmatory else " (gate not passed: exploratory)"),
            lambda r: "within ±δ" if (abs(r.t_ci_lo_SENSITIVITY) < 0.01 and abs(r.t_ci_hi_SENSITIVITY) < 0.01) else
            f"[{r.t_ci_lo_SENSITIVITY:+.4f}, {r.t_ci_hi_SENSITIVITY:+.4f}]"]
    heads = ["arm", "outcome", "mean Δ [BCa 95 % CI]", "clusters benefit/zero/harm", "Holm p", "category", "paired-t 95 % CI (sensitivity)"]
    for fam, title in [("F1 C1-D1 total", "Table 2a. F1 — coordinated arm − B3 at D1 (registered total effect)"),
                       ("F2 C1-D0", "Table 2b. F2 — coordinated arm − B3 at D0")]:
        md.append(f"\n## {title}\n")
        md.append(table(C[C.family == fam], cols, heads))
    md.append("\n## Table 3a. F3 — arbitration: arm − B3h at equal coordination hold\n")
    x = C[C.family == "F3 C2 arbitration"].copy()
    md.append(table(x, [lambda r: HOLD[r.delay]] + cols, ["hold"] + heads))
    md.append("\n## Table 3b. F4 — overhead: B3h(hold) − B3 at D1\n")
    x = C[C.family == "F4 C2 overhead"]
    md.append(table(x, [lambda r: HOLD[r.delay]] + cols[1:], ["hold"] + heads[1:]))
    DS = pd.read_csv(os.path.join(A, "descriptive_contrasts.csv"))
    dcols = [lambda r: arm_of(r.contrast), lambda r: HOLD.get(r.delay, r.delay), lambda r: OUTN.get(r.outcome, r.outcome),
             lambda r: ci(r), lambda r: f"{int(r.clusters_benefit)}/{int(r.clusters_zero)}/{int(r.clusters_harm)}"]
    dheads = ["contrast", "config", "outcome", "mean Δ [BCa 95 % CI]", "clusters benefit/zero/harm"]
    md.append("\n## Table 4. Descriptive: total delay curve (arm@D1+Δ − B3@D1), primary outcomes\n")
    md.append(table(DS[(DS.family == "C2 total (delay curve)") & DS.outcome.isin(["rel_goodput", "viol"])], dcols, dheads))
    md.append("\n## Table 5. Descriptive: single-xApp and joint effects vs B0 (family A)\n")
    md.append(table(DS[(DS.family == "Family A") & DS.outcome.isin(["rel_goodput", "viol", "min_user_frac", "etx"])], dcols, dheads))
    md.append("\n## Table 6. Descriptive robustness: reversed B3 order (D0) and C3 load step\n")
    md.append(table(DS[DS.family.isin(["S8a reversed B3 order", "C3 step total", "C3 step arbitration", "C3 step overhead"])
                       & DS.outcome.isin(["rel_goodput", "viol", "viol_post_step", "recovery_s"])], [lambda r: r.family] + dcols,
                    ["family"] + dheads))
    md.append("\n## Table 7. Secondary outcomes of the confirmatory contrasts (descriptive)\n")
    sec = DS[DS.family.str.endswith("(secondary)") & DS.family.str.startswith(("F1", "F2"))]
    md.append(table(sec, [lambda r: r.family.replace(" (secondary)", "")] + dcols, ["family"] + dheads))
    EV = pd.read_csv(os.path.join(A, "event_coverage.csv"))
    md.append("\n## Table 8. Event coverage (reference runs with ≥ 1 combined flag)\n")
    md.append(table(EV, [lambda r: HOLD.get(r.cfg, r.cfg), lambda r: r.reference, lambda r: f"{int(r.flagged_combined)}/{int(r.clusters)}",
                         lambda r: "UNDER-COVERED" if r.under_covered else "≥ 10"], ["config", "reference", "flagged clusters", "coverage"]))
    SU = pd.read_csv(os.path.join(A, "event_conditional_SUPPLEMENTARY.csv"))
    SU = SU[SU.contrast.str.contains("sa")]
    md.append("\n## Table 9. SUPPLEMENTARY event-conditional contrasts (flagged clusters only; state-aware arms)\n")
    md.append(table(SU, [lambda r: r.family, lambda r: arm_of(r.contrast), lambda r: HOLD.get(r.delay, r.delay),
                         lambda r: OUTN[r.outcome], lambda r: str(int(r.flagged_clusters)), lambda r: f(r["mean"]),
                         lambda r: f"{int(r.benefit)}/{int(r.harm)}", lambda r: "under-covered" if r.under_covered else ""],
                    ["family", "arm", "hold", "outcome", "flagged clusters", "mean Δ", "benefit/harm clusters", "note"]))
    open(OUT, "w").write("\n".join(md))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
