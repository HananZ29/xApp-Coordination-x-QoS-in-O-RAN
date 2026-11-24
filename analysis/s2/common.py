"""Shared helpers for the S2 analyses: locating batch data, reading per-run outputs, figure style.

Every analysis reads the LATEST batch of a given name (results/s2/<batch>/<stamp>/BATCH.json ->
external data directory) unless a stamp is passed, and writes tables/figures to
results/s2/analysis/<name>/. Figures follow the dataviz reference palette (light mode):
categorical slots 1-3 only (validated all-pairs), thin marks, recessive grid, one y-axis per
panel, small multiples instead of extra colours.
"""
import glob
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]  # categorical slots 1-3 (blue, orange, aqua)
INK, INK2, GRID, SURFACE = "#1f1f1e", "#5f5e59", "#e4e3de", "#fcfcfb"


def style():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": INK2, "axes.labelcolor": INK, "text.color": INK,
        "xtick.color": INK2, "ytick.color": INK2, "axes.grid": True, "grid.color": GRID,
        "grid.linewidth": 0.6, "axes.spines.top": False, "axes.spines.right": False,
        "lines.linewidth": 1.6, "lines.markersize": 5, "font.size": 9, "axes.titlesize": 9.5,
        "legend.frameon": False, "legend.fontsize": 8,
    })


def batch_dir(batch, stamp=None):
    base = os.path.join(REPO, "results", "s2", batch)
    stamps = sorted(d for d in os.listdir(base) if os.path.isfile(os.path.join(base, d, "BATCH.json")))
    stamp = stamp or stamps[-1]
    meta = json.load(open(os.path.join(base, stamp, "BATCH.json")))
    mans = [json.loads(l) for l in open(os.path.join(base, stamp, "manifests.jsonl"))]
    return meta["external_data"], mans, meta


def out_dir(name):
    d = os.path.join(REPO, "results", "s2", "analysis", name)
    os.makedirs(d, exist_ok=True)
    return d


def rd(data, run, kind):
    return pd.read_csv(os.path.join(data, f"{run}-{kind}.csv"))


def ue_cells(data, run):
    """Initial cell per UE index (from the run's ues.csv)."""
    u = rd(data, run, "ues")
    return dict(zip(u.ue, u.initial_cell))


def save(fig, path):
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
