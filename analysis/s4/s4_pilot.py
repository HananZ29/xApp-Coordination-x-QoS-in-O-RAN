#!/usr/bin/env python3
"""S4 pilot and TPC-calibration analysis (independent controllers only; no B3-B5).

Batches: s4pilot (seeds 201-202; B0 noop / B1 TS / B2 TPC; 4 scenarios x 3 loads), s4cal (TPC only, calibration
seed 1; PrbLow {0.3,0.5,0.7} x StepDb {3,6}).
Outputs (results/s4/analysis/):
  pilot_runs.csv          per run: receiver outcomes 5-30 s, actions, outcomes, reversals, power/activity proxy
  pilot_paired.csv        B1-B0 and B2-B0 per scenario x load (paired by seed)
  pilot_safeguards.csv    decision-log reason counts per controller
  pilot_telemetry.csv     I3b vs receiver violation per window in B1/B2 runs (after control actions)
  pilot_x55_attach.csv    0.55-ISD retained-connection check (B0 runs)
  pilot_power_proxy_check.csv  applied vs radiated power (TPC runs)
  cal_runs.csv            calibration outcomes
"""
import glob
import json
import os
import sys

import numpy as np
import pandas as pd

REPO = os.path.expanduser("~/ns-3.42/scratch/xapp-coord")
sys.path.insert(0, os.path.join(REPO, "analysis", "s2"))
from common import rd  # noqa: E402

OUT = os.path.join(REPO, "results", "s4", "analysis")
T0, T1 = 5.0, 30.0
ALPHA = 0.95
PAY = 1024 / 1054
N_RE = -123.24


def batch(name):
    r = sorted(glob.glob(os.path.join(REPO, "results", "s4", name, "*")))[-1]
    mans = [json.loads(l) for l in open(os.path.join(r, "manifests.jsonl"))]
    return json.load(open(os.path.join(r, "BATCH.json")))["external_data"], mans


def parse(rid):
    p = rid.split("_")
    k = {"pos": p[1], "rate_a": float(p[2][1:]), "rate_b": float(p[3][1:])}
    if p[0] == "pil":
        k.update(ctl=p[4], seed=int(p[5][1:]))
    else:
        k.update(prb_low=float(p[4][2:]), step=float(p[5][2:]), seed=1)
    return k


def power_proxy(d, rid):
    """Applied DL power timeline (power.csv) integrated over (T0, T1] per cell: E_tx = sum P[W] dt (always-on
    proxy) and E_act = sum P[W] * data-RB fraction dt (activity-weighted, data RBs from prb.csv). Watts of RF
    transmit power, NOT electrical energy."""
    pw = rd(d, rid, "power")
    pr = rd(d, rid, "prb")
    out = {}
    for c in (1, 2):
        tl = pw[pw.cell == c].sort_values("time_s")[["time_s", "new_dbm"]].values
        x = pr[(pr.cell == c) & (pr.time_s > T0) & (pr.time_s <= T1 + 1e-9)]
        e_tx = e_act = 0.0
        tmin = 0.0
        for row in x.itertuples():
            ws = row.time_s - 0.5
            p = tl[tl[:, 0] <= ws + 1e-9][-1, 1] if (tl[:, 0] <= ws + 1e-9).any() else tl[0, 1]
            w = 10 ** (p / 10) / 1000
            e_tx += w * 0.5
            e_act += w * 0.5 * row.data_rb_sum / (25 * row.subframes) if row.subframes else 0
            tmin += 0.5 if p < 29.999 else 0
        out[f"etx_c{c}_Ws"] = e_tx
        out[f"eact_c{c}_Ws"] = e_act
        out[f"t_reduced_c{c}_s"] = tmin
    return out


def run_row(d, rid):
    k = parse(rid)
    fl = rd(d, rid, "flows")
    fs = rd(d, rid, "flowsum").set_index("ue")
    post = fl[(fl.time_s > T0 + 1e-9) & (fl.time_s <= T1 + 1e-9)].copy()
    post["A"] = post.ue.map(fs.offered_mbps)
    g = post.groupby("ue").goodput_mbps.mean()
    frac = g / fs.offered_mbps
    r = dict(k, run=rid, mean_goodput=g.mean(), total_goodput=g.sum(), min_user_frac=frac.min(),
             viol=float((post.goodput_mbps < ALPHA * post.A).mean()),
             viol_edge=float((post[post.ue.isin([3, 4, 5])].goodput_mbps < ALPHA * post[post.ue.isin([3, 4, 5])].A).mean()))
    c = rd(d, rid, "ctrl")
    r["rlf"] = int((c.event == "radio_link_failure_ue").sum())
    ho = c[c.event == "ho_end_ok_ue"]
    r["ho_completed"] = len(ho)
    # handover reversals: UE returns to the cell it left within 10 s
    rev = 0
    for imsi, h in c[c.event.isin(["ho_start_enb"])].groupby("imsi"):
        h = h.sort_values("time_s")
        for i in range(1, len(h)):
            if h.iloc[i].target_cell == h.iloc[i - 1].cell and h.iloc[i].time_s - h.iloc[i - 1].time_s < 10:
                rev += 1
    r["ho_reversals"] = rev
    lf = os.path.join(d, f"{rid}-cmdlife.csv")
    if os.path.exists(lf):
        L = pd.read_csv(lf, keep_default_na=False)
        F = L[L.stage == "final"]
        for t in ("handover", "tx_power"):
            x = F[F.cmd_type == t]
            r[f"{t}_cmds"] = len(x)
            for o, n in x.outcome.value_counts().items():
                r[f"{t}_{o}"] = n
            rj = x[x.outcome == "rejected"].reason.value_counts()
            r[f"{t}_reject_reasons"] = ";".join(f"{a}:{b}" for a, b in rj.items())
    df = os.path.join(d, f"{rid}-xappdec.csv")
    if os.path.exists(df):
        X = pd.read_csv(df, keep_default_na=False)
        r["tpc_reduce"] = int((X.decision == "propose_reduce").sum())
        r["tpc_restore"] = int((X.decision == "propose_restore").sum())
        r["ts_proposals"] = int((X.decision == "propose_handover").sum())
        r["ts_unconfirmed"] = int((X.decision == "ho_unconfirmed").sum())
        ages = X[X.decision == "propose_handover"].detail.str.extract(r"load_age ([0-9.]+)")[0].astype(float)
        r["ts_load_age_max_s"] = ages.max() if len(ages) else np.nan
    r.update(power_proxy(d, rid))
    return r


def telemetry_windows(d, rid):
    k = parse(rid)
    t = pd.read_csv(os.path.join(d, f"{rid}-telemetry.csv"))
    t = t[t.type == "ns3::OranReportLteUeService"]
    fl = rd(d, rid, "flows")
    fs = rd(d, rid, "flowsum")
    i2u, A = dict(zip(fs.imsi, fs.ue)), dict(zip(fs.ue, fs.offered_mbps))
    g = fl.set_index(["ue", fl.time_s.round(3)]).goodput_mbps
    acts = []
    lf = os.path.join(d, f"{rid}-cmdlife.csv")
    if os.path.exists(lf):
        L = pd.read_csv(lf, keep_default_na=False)
        acts = L[L.stage == "applied"].time_s.astype(float).tolist()
    rows, hist = [], {}
    for r in t.itertuples():
        p = dict(kv.split("=") for kv in r.payload.split(";"))
        t1, t0 = round(r.report_time_s, 3), float(p["t0"])
        dt = t1 - t0
        if dt <= 0:
            continue
        key = (int(p["imsi"]), int(p["cell"]))
        dem = int(p["in"]) * 8 / dt / 1e6 * PAY
        hist.setdefault(key, []).append(dem)
        dm = np.mean(hist[key][-4:])
        if dt < 0.499 or t1 <= T0:
            continue
        ue = i2u[int(p["imsi"])]
        if (ue, t1) not in g.index:
            continue
        served = int(p["out"]) * 8 / dt / 1e6 * PAY
        al = int(p["acked"]) + int(p["harqlost"])
        hl = int(p["harqlost"]) / al if al else 0
        flag = (dm > 0) and (served < ALPHA * dm or hl > 1 - ALPHA)
        since = min([t1 - a for a in acts if a <= t1] or [1e9])
        rows.append(dict(k, run=rid, ue=ue, t=t1, flag=flag, viol=g[(ue, t1)] < ALPHA * A[ue],
                         after_action=since <= 3.0, age_ms=r.age_ms))
    return rows


def main():
    os.makedirs(OUT, exist_ok=True)
    d, mans = batch("s4pilot")
    R = pd.DataFrame([run_row(d, m["run_id"]) for m in mans])
    R.to_csv(os.path.join(OUT, "pilot_runs.csv"), index=False)
    # paired effects
    key = ["pos", "rate_a", "rate_b", "seed"]
    b0 = R[R.ctl == "b0"].set_index(key)
    pr = []
    for ctl in ("b1", "b2"):
        x = R[R.ctl == ctl].set_index(key)
        for col in ("mean_goodput", "min_user_frac", "viol", "viol_edge", "etx_c1_Ws", "etx_c2_Ws", "eact_c1_Ws", "eact_c2_Ws"):
            diff = (x[col] - b0[col]).groupby(level=[0, 1, 2]).agg(["mean", "min", "max"])
            for idx, v in diff.iterrows():
                pr.append(dict(zip(["pos", "rate_a", "rate_b"], idx), ctl=ctl, outcome=col, d_mean=v["mean"],
                               d_min=v["min"], d_max=v["max"]))
    pd.DataFrame(pr).to_csv(os.path.join(OUT, "pilot_paired.csv"), index=False)
    # safeguard / decision reasons
    sg = []
    for m in mans:
        f = os.path.join(d, f"{m['run_id']}-xappdec.csv")
        if os.path.exists(f):
            X = pd.read_csv(f, keep_default_na=False)
            for (xa, dec, why), n in X.groupby(["xapp", "decision", "reason"]).size().items():
                sg.append(dict(parse(m["run_id"]), run=m["run_id"], xapp=xa, decision=dec, reason=why, n=n))
    S = pd.DataFrame(sg)
    S.to_csv(os.path.join(OUT, "pilot_safeguards.csv"), index=False)
    # telemetry after actions
    tw = []
    for m in mans:
        if parse(m["run_id"])["ctl"] in ("b1", "b2"):
            tw += telemetry_windows(d, m["run_id"])
    TW = pd.DataFrame(tw)
    TW.to_csv(os.path.join(OUT, "pilot_telemetry_windows.csv"), index=False)
    tv = []
    for name, x in (("all", TW), ("after_action_3s", TW[TW.after_action]), ("b1", TW[TW.ctl == "b1"]),
                    ("b2", TW[TW.ctl == "b2"]), ("x55", TW[TW.pos == "x55"])):
        v, f = x.viol, x.flag
        tp, fp, fn, tn = (v & f).sum(), (~v & f).sum(), (v & ~f).sum(), (~v & ~f).sum()
        n = len(x)
        po, pe = (tp + tn) / n, ((tp + fp) * (tp + fn) + (fn + tn) * (fp + tn)) / n ** 2
        tv.append({"subset": name, "windows": n, "viol_rate": v.mean(), "kappa": (po - pe) / (1 - pe),
                   "false_safe_rate": fn / max(tp + fn, 1), "false_alarm_rate": fp / max(fp + tn, 1),
                   "age_ms_max": x.age_ms.max()})
    pd.DataFrame(tv).to_csv(os.path.join(OUT, "pilot_telemetry.csv"), index=False)
    # 0.55-ISD retained connection check (B0)
    at = []
    for m in mans:
        k = parse(m["run_id"])
        if k["ctl"] != "b0" or k["pos"] != "x55":
            continue
        rid = m["run_id"]
        u = rd(d, rid, "ues")
        c = rd(d, rid, "ctrl")
        me = rd(d, rid, "meas")
        est = c[c.event == "rrc_connection_established_ue"].groupby("imsi").cell.agg(list)
        for ue in (3, 4, 5):
            imsi = int(u.set_index("ue").imsi[ue])
            mm = me[(me.ue == ue) & (me.time_s > T0)]
            gam = []
            for tt, gg in mm.groupby("time_s"):
                lin = 10 ** (gg.rsrp_dbm / 10)
                s_ = lin[gg.serving == 1].sum()
                gam.append(10 * np.log10(s_ / (lin[gg.serving == 0].sum() + 10 ** (N_RE / 10))) if s_ > 0 else np.nan)
            at.append(dict(k, run=rid, ue=ue, configured_cell=int(u.set_index("ue").initial_cell[ue]),
                           established_cells=str(est.get(imsi, [])),
                           rlf=int(((c.event == "radio_link_failure_ue") & (c.imsi == imsi)).sum()),
                           gamma_ctrl_min_db=np.nanmin(gam), gamma_ctrl_mean_db=np.nanmean(gam),
                           rsrp_b_minus_a_db=(mm[mm.cell_id == 2].rsrp_dbm.mean() - mm[mm.cell_id == 1].rsrp_dbm.mean())))
    pd.DataFrame(at).to_csv(os.path.join(OUT, "pilot_x55_attach.csv"), index=False)
    # applied vs radiated power (TPC runs)
    pc = []
    for m in mans:
        if parse(m["run_id"])["ctl"] != "b2":
            continue
        rid = m["run_id"]
        L = pd.read_csv(os.path.join(d, f"{rid}-cmdlife.csv"), keep_default_na=False)
        a = L[(L.cmd_type == "tx_power") & (L.stage == "applied")]
        e = L[(L.cmd_type == "tx_power") & (L.stage == "phy_effect")]
        for r in a.itertuples():
            ee = e[e.cmd_id == r.cmd_id]
            pc.append({"run": rid, "cmd": r.cmd_id, "applied_dbm": r.new_dbm,
                       "radiated_dbm": ee.new_dbm.iloc[0] if len(ee) else np.nan,
                       "lag_ms": (ee.time_ns.iloc[0] - r.time_ns) / 1e6 if len(ee) else np.nan})
    pd.DataFrame(pc).to_csv(os.path.join(OUT, "pilot_power_proxy_check.csv"), index=False)
    # calibration
    dc, mc = batch("s4cal")
    C = pd.DataFrame([run_row(dc, m["run_id"]) for m in mc])
    C.to_csv(os.path.join(OUT, "cal_runs.csv"), index=False)
    pd.set_option("display.width", 250)
    print(R.groupby("ctl")[["ts_proposals", "tpc_reduce", "tpc_restore", "ho_completed", "ho_reversals", "rlf"]].sum())
    print(pd.DataFrame(tv).round(4).to_string(index=False))


if __name__ == "__main__":
    main()
