# O-RAN xApp coordination

This research artifact evaluates coordination between Traffic Steering and
Transmit Power Control in a two-cell ns-3.42 LTE simulation with NIST ns3-oran.
It contains the completed S7 confirmatory study, E1 reference-event study, and
E2 transmit-power sensitivity study. See the frozen registries in
`docs/methodology/` and the tables and figures in `results/publication/`.
The findings apply to the tested simulated configurations.

## Build and reproduce

Use a fresh Linux or WSL environment. Check out ns-3 and ns3-oran at the exact
revisions in `environment/ENVIRONMENT.md`. Place the artifact at
`~/ns-3.42/scratch/xapp-coord` and ns3-oran at `~/ns-3.42/contrib/ns3-oran`.
Build the scratch target with the documented ns-3 build configuration.
The canonical checkout location is needed by the retained analysis helpers.

From the repository root, run `python3 results/publication/make_publication.py`
to rebuild the publication package from the included analysis outputs.
For trace-level reanalysis, lay out the archive under `data/s7/` and `data/s8/`
as identified by the batch records. Verify the archived files against each
batch's `SHA256SUMS`. Run the entry points in `analysis/s7/` and `analysis/s8/`
from the repository root. Work in a separate clone when regenerating outputs.
For fresh simulations, use another clean checkout with fresh run-state and
output directories. See `scripts/run/s7_run.py` and `scripts/run/s8_run.py`.

Public deposit of the raw traces is pending. Trace-level reanalysis requires the archive.

## Provenance and attribution

This curated release was exported from internal snapshot `f06f86fb9d446426e41197079130fe14abbe35ee`. The
frozen study revisions are recorded in the batch manifests. Public copies
replace local path strings in JSON and JSONL metadata. The recorded
configuration, binary and output hashes identify the original run evidence.
Derived records identify the simulated source run and its hashes.

Research by Hanan Zoueihed. Claude Code assisted with implementation and
analysis tooling under the researcher's direction. The simulation uses
ns-3 and the NIST ns3-oran module, credited here as upstream dependencies.
