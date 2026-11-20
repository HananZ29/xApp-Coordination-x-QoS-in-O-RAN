"""Phase 1 exit check: delivered vs offered throughput per cell.

Usage: python3 plot_phase1.py ~/xapp-analysis/phase1
Reads a<rate>_r<run>-tput.csv files written by phase1-sweep.sh and saves phase1_load_curve.png.
"""
import glob
import os
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

STATS_START = 5.0  # traffic-start (3 s) + warm-up (2 s); keep windows ending after this

d = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/xapp-analysis/phase1")
cell_rows, group_rows = [], []

for f in sorted(glob.glob(os.path.join(d, "a*_r*-tput.csv"))):
    m = re.search(r"a([\d.]+)_r(\d+)-tput\.csv$", f)
    rate, run = float(m.group(1)), int(m.group(2))
    df = pd.read_csv(f)
    df = df[df.time_s > STATS_START]

    per_cell = (
        df.groupby(["time_s", "cell_id"])
        .agg(delivered=("tput_mbps", "sum"), offered=("offered_mbps", "sum"))
        .groupby("cell_id")
        .mean()
        .reset_index()
    )
    for _, r in per_cell.iterrows():
        cell_rows.append(dict(rate_a=rate, run=run, cell=int(r.cell_id),
                              offered=r.offered, delivered=r.delivered))

    per_group = df.groupby("group").agg(delivered=("tput_mbps", "mean"),
                                        offered=("offered_mbps", "mean")).reset_index()
    for _, r in per_group.iterrows():
        group_rows.append(dict(rate_a=rate, run=run, group=r.group,
                               offered=r.offered, delivered=r.delivered))

if not cell_rows:
    sys.exit(f"No *-tput.csv files found in {d}")

cells = (pd.DataFrame(cell_rows).groupby(["cell", "rate_a"])
         .agg(offered=("offered", "mean"), delivered=("delivered", "mean"),
              sd=("delivered", "std")).reset_index())
groups = (pd.DataFrame(group_rows).groupby(["group", "rate_a"])
          .agg(offered=("offered", "mean"), delivered=("delivered", "mean")).reset_index())

pd.set_option("display.width", 120)
print("Per cell (Mbps):\n", cells.round(3).to_string(index=False))
print("\nPer UE, by group (Mbps):\n", groups.round(3).to_string(index=False))

fig, ax = plt.subplots(1, 2, figsize=(11, 4.2))
for cell, g in cells.groupby("cell"):
    ax[0].errorbar(g.offered, g.delivered, yerr=g.sd.fillna(0), marker="o", capsize=3,
                   label=f"cell {cell}")
lim = cells.offered.max() * 1.05
ax[0].plot([0, lim], [0, lim], "k--", lw=1, label="delivered = offered")
ax[0].set(xlabel="Offered load per cell (Mbps)", ylabel="Delivered throughput (Mbps)",
          title="Cell 1 should saturate; cell 2 should stay flat")
ax[0].legend()

for grp, g in groups[groups.group != "nearB"].groupby("group"):
    ax[1].plot(g.offered, g.delivered, marker="o", label=grp)
lim = groups.offered.max() * 1.05
ax[1].plot([0, lim], [0, lim], "k--", lw=1)
ax[1].set(xlabel="Offered per UE in cell 1 (Mbps)", ylabel="Delivered per UE (Mbps)",
          title="Edge UEs should fall short first")
ax[1].legend()

fig.tight_layout()
out = os.path.join(d, "phase1_load_curve.png")
fig.savefig(out, dpi=150)
print(f"\nSaved {out}")
