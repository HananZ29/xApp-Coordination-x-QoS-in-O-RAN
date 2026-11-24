#!/usr/bin/env python3
"""Manuscript figures not produced by the registered S7 analysis:
  fig_timeline.pdf      control-loop timeline at D1 (realised values from the G6 check-2 logs; schematic, not to scale)
  fig_family_a.pdf      single-xApp and joint effects vs B0 (descriptive; from descriptive_contrasts.csv)
  fig_per_condition.pdf per-condition mean paired effects of the state-aware arms vs B3 at D0/D1 (descriptive)
Colours: Okabe–Ito (CVD-safe), fixed per policy; every series also has a marker and a direct label.
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
A = os.path.join(REPO, "results", "s7", "analysis")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "figures")
INK, MUTED = "#222222", "#6b6b6b"


def timeline():
    steps = [  # (label, realised value, segment colour)
        ("report generated\n(t − 0.5 s)", None, None),
        ("E2 uplink\n6 ms report age\n(= 3 × 2 ms, ns3-oran\nsend-phase anchoring)", "6 ms", "#56B4E9"),
        ("waiting for the\nnext query instant t", "≈ 494 ms", "#BBBBBB"),
        ("xApp processing\n12 µs", "12 µs", "#E69F00"),
        ("coordination hold\n131 µs (+ Δ ∈ {50, 125, 250} ms)\nB3: 0 · B3h and arms: hold", "131 µs (+Δ)", "#D55E00"),
        ("E2 downlink\n2 ms", "2 ms", "#0072B2"),
        ("actuation\n(TPC ≤ 1 ms; HO ≈ 26 ms)", None, None),
    ]
    fig, ax = plt.subplots(figsize=(7.2, 2.4))
    widths = [0, 1.0, 2.2, 0.8, 1.6, 1.0, 0]
    x = 0.2
    for (lab, val, colr), w in zip(steps, widths):
        if w == 0:
            ax.plot([x, x], [0.35, 0.65], color=INK, lw=1.5)
            ax.text(x, 0.72, lab, ha="center", va="bottom", fontsize=6.5, color=INK)
            x += 0.15
            continue
        ax.add_patch(plt.Rectangle((x, 0.4), w - 0.04, 0.2, color=colr, lw=0))
        ax.text(x + (w - 0.04) / 2, 0.5, val, ha="center", va="center", fontsize=6.5, color="white" if colr not in ("#BBBBBB", "#56B4E9", "#E69F00") else INK)
        ax.text(x + (w - 0.04) / 2, 0.33, lab, ha="center", va="top", fontsize=6, color=MUTED)
        x += w
    ax.set_xlim(0, x + 0.4)
    ax.set_ylim(-0.25, 1.15)
    ax.axis("off")
    ax.set_title("D1 control loop as realised in the logs (schematic; widths not to scale)", fontsize=8, color=INK)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_timeline.pdf"))
    plt.close(fig)


def family_a():
    p = os.path.join(A, "descriptive_contrasts.csv")
    if not os.path.exists(p):
        return
    D = pd.read_csv(p)
    D = D[(D.family == "Family A")]
    fig, axes = plt.subplots(1, 2, figsize=(7, 2.6))
    col = {"B1": "#E69F00", "B2": "#56B4E9", "B3": "#000000"}
    mk = {"B1": "o", "B2": "s", "B3": "D"}
    for ax, o, yl in zip(axes, ["rel_goodput", "viol"], ["Δ relative goodput vs B0", "Δ violation fraction vs B0"]):
        ax.axhline(0, color="#888", lw=0.8)
        ax.axhspan(-0.01, 0.01, color="#e6e6e6", lw=0, zorder=0)
        for i, cfg in enumerate(["D0", "D1"]):
            for j, arm in enumerate(["B1", "B2", "B3"]):
                r = D[(D.outcome == o) & (D.delay == cfg) & (D.contrast == f"{arm}-B0")]
                if not len(r):
                    continue
                r = r.iloc[0]
                xx = i * 4 + j
                ax.errorbar(xx, r["mean"], yerr=[[r["mean"] - r.ci_lo], [r.ci_hi - r["mean"]]], fmt=mk[arm], color=col[arm], ms=5, lw=1.5, capsize=2)
                ax.annotate(arm, (xx, r.ci_hi), textcoords="offset points", xytext=(0, 3), ha="center", fontsize=6)
        ax.set_xticks([1, 5])
        ax.set_xticklabels(["D0", "D1"], fontsize=7)
        ax.set_ylabel(yl, fontsize=7)
        ax.tick_params(labelsize=6)
    fig.suptitle("Single-xApp (B1 TS, B2 TPC) and joint uncoordinated (B3) effects vs both-shadow B0 (descriptive; BCa 95 % CI)", fontsize=7)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_family_a.pdf"))
    plt.close(fig)


def per_condition():
    p = os.path.join(A, "per_condition_DESCRIPTIVE.csv")
    if not os.path.exists(p):
        return
    D = pd.read_csv(p)
    D = D[D.family.isin(["F1 C1-D1 total", "F2 C1-D0"]) & D.contrast.isin(["b4sats-B3", "b4satpc-B3", "b5sa-B3"])]
    col = {"b4sats-B3": "#0072B2", "b4satpc-B3": "#D55E00", "b5sa-B3": "#CC79A7"}
    mk = {"b4sats-B3": "^", "b4satpc-B3": "v", "b5sa-B3": "P"}
    lab = {"b4sats-B3": "B4sa-TS", "b4satpc-B3": "B4sa-TPC", "b5sa-B3": "B5sa"}
    D = D.assign(cond=D.pos + " a" + D.rate_a.astype(str) + " b" + D.rate_b.astype(str))
    conds = sorted(D.cond.unique())
    fig, axes = plt.subplots(2, 2, figsize=(7.2, 4.6), sharex=True)
    for i, o in enumerate(["rel_goodput", "viol"]):
        for j, dl in enumerate(["D0", "D1"]):
            ax = axes[i, j]
            ax.axhline(0, color="#888", lw=0.8)
            ax.axhspan(-0.01, 0.01, color="#e6e6e6", lw=0, zorder=0)
            for k, (c, g) in enumerate(D[(D.outcome == o) & (D.delay == dl)].groupby("contrast")):
                g = g.set_index("cond").reindex(conds)
                ax.plot([x + (k - 1) * 0.2 for x in range(len(conds))], g["mean"], mk[c], color=col[c], ms=4, label=lab[c])
            ax.set_title(f"{dl}: {'Δ rel. goodput' if o == 'rel_goodput' else 'Δ violation fraction'} vs B3 (mean over 13 seeds)", fontsize=7)
            ax.tick_params(labelsize=6)
            if i == 1:
                ax.set_xticks(range(len(conds)))
                ax.set_xticklabels(conds, rotation=60, ha="right", fontsize=5.5)
    axes[0, 0].legend(fontsize=6)
    fig.suptitle("Per-condition paired effects (descriptive; not confirmatory)", fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_per_condition.pdf"))
    plt.close(fig)


def delay_curve():
    """Manuscript version of the registered delay-decomposition figure (same CSV values; legend moved outside the axes)."""
    C = pd.read_csv(os.path.join(A, "confirmatory_contrasts.csv"))
    D = pd.read_csv(os.path.join(A, "descriptive_contrasts.csv"))
    hold = {"D1": 0.131, "D1d1": 50.131, "D1d2": 125.131, "D1d3": 250.131}
    arms = ["b4ts", "b4tpc", "b5", "b4sats", "b4satpc", "b5sa"]
    lab = {"b4ts": "B4-TS", "b4tpc": "B4-TPC", "b5": "B5", "b4sats": "B4sa-TS", "b4satpc": "B4sa-TPC", "b5sa": "B5sa"}
    col = {"b4ts": "#E69F00", "b4tpc": "#56B4E9", "b5": "#009E73", "b4sats": "#0072B2", "b4satpc": "#D55E00", "b5sa": "#CC79A7"}
    mk = {"b4ts": "o", "b4tpc": "s", "b5": "D", "b4sats": "^", "b4satpc": "v", "b5sa": "P"}
    xs = list(hold)
    fig, axes = plt.subplots(2, 2, figsize=(7.2, 4.6), sharex=True)
    for i, o in enumerate(["rel_goodput", "viol"]):
        for j, kind in enumerate(["arb", "tot"]):
            ax = axes[i, j]
            ax.axhspan(-0.01, 0.01, color="#e6e6e6", lw=0, zorder=0)
            ax.axhline(0, color="#888", lw=0.8)
            for k, a in enumerate(arms):
                pts = []
                for d in xs:
                    if kind == "arb":
                        r = C[(C.family == "F3 C2 arbitration") & (C.contrast == f"{a}-B3h") & (C.delay == d) & (C.outcome == o)]
                    elif d == "D1":
                        r = C[(C.family == "F1 C1-D1 total") & (C.contrast == f"{a}-B3") & (C.outcome == o)]
                    else:
                        r = D[(D.family == "C2 total (delay curve)") & (D.contrast == f"{a}-B3@D1") & (D.delay == d) & (D.outcome == o)]
                    if len(r):
                        pts.append((xs.index(d) + (k - 2.5) * 0.06, r.iloc[0]))
                if pts:
                    ax.errorbar([p[0] for p in pts], [p[1]["mean"] for p in pts],
                                yerr=[[p[1]["mean"] - p[1].ci_lo for p in pts], [p[1].ci_hi - p[1]["mean"] for p in pts]],
                                fmt=mk[a] + "-", color=col[a], ms=3.5, lw=0.9, capsize=1.5, label=lab[a])
            if kind == "tot":
                r = [C[(C.family == "F4 C2 overhead") & (C.delay == d) & (C.outcome == o)].iloc[0] for d in xs]
                ax.errorbar([x + 0.21 for x in range(4)], [q["mean"] for q in r], yerr=[[q["mean"] - q.ci_lo for q in r], [q.ci_hi - q["mean"] for q in r]],
                            fmt="k*--", ms=5, lw=1.1, capsize=2, label="B3h − B3 (overhead, F4)")
            ax.set_title(("arbitration: arm − B3h at equal hold (F3)" if kind == "arb" else "total: arm − B3@D1; overhead: B3h − B3@D1")
                         + "\n" + ("Δ relative goodput" if o == "rel_goodput" else "Δ violation fraction"), fontsize=7)
            ax.tick_params(labelsize=6.5)
            ax.set_xticks(range(4))
            ax.set_xticklabels(["0.131", "50.131", "125.131", "250.131"])
            if i == 1:
                ax.set_xlabel("coordination hold (ms)", fontsize=7)
    h, l = axes[0, 1].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=7, fontsize=6.5, frameon=False)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    fig.savefig(os.path.join(OUT, "fig_delay_decomposition_ms.pdf"))
    plt.close(fig)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    timeline()
    family_a()
    per_condition()
    delay_curve()
    print("figures in", OUT)
