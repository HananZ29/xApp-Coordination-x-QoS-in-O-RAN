#!/usr/bin/env python3
"""S7 AUDIT: pairing. For every SIMULATED coordinated-arm run, compare with its own-configuration reference
(B3 at D0/S8a, B3h at D1/D1+Delta/C3): the first simulated time at which the delivered-traffic trace (tput.csv)
differs must not precede the first time the command-lifecycle log (cmdlife.csv) differs. Otherwise something other
than the coordinator's intervention changed the run (pairing violation)."""
import json, os
import numpy as np, pandas as pd
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
S7 = os.path.join(REPO, "results", "s7")
st = json.load(open(os.path.join(S7, "state.json")))["index"]
COORD = {"b4ts", "b4tpc", "b5", "b4sats", "b4satpc", "b5sa"}
K = ["time_ns", "stage", "cmd_type", "cell", "target_cell", "imsi", "value_dbm", "outcome", "reason"]
def path(rid):
    e = st[rid]; src = e.get("source_run_id", rid); return os.path.join(st[src]["data"], src)
def first_div(a, b, cols, tcol):
    n = min(len(a), len(b))
    x, y = a[cols].iloc[:n].astype(str).values, b[cols].iloc[:n].astype(str).values
    neq = np.where((x != y).any(axis=1))[0]
    if len(neq):
        i = neq[0]; return min(float(a[tcol].iloc[i]), float(b[tcol].iloc[i]))
    if len(a) != len(b):
        return float((a if len(a) > len(b) else b)[tcol].iloc[n])
    return np.inf
rows = []
for rid, e in st.items():
    if e["status"] != "completed" or e["arm"] not in COORD:
        continue
    ref = rid.replace(f"_{e['arm']}_", "_B3_" if e["cfg"] == "D0" else "_B3h_")
    p, q = path(rid), path(ref)
    ca, cb = pd.read_csv(p + "-cmdlife.csv", keep_default_na=False), pd.read_csv(q + "-cmdlife.csv", keep_default_na=False)
    ta, tb = pd.read_csv(p + "-tput.csv"), pd.read_csv(q + "-tput.csv")
    t_cmd = first_div(ca, cb, K, "time_s")
    t_out = first_div(ta, tb, ["time_s", "ue", "rx_bytes"], "time_s")
    # tput row labelled t covers (t-0.5, t]: a command at t_cmd can change rows labelled >= t_cmd only
    rows.append(dict(run=rid, cfg=e["cfg"], arm=e["arm"], t_cmd=t_cmd, t_out=t_out,
                     violation=bool(t_out < np.inf and t_out + 1e-9 < t_cmd)))
R = pd.DataFrame(rows)
R.to_csv(os.path.join(S7, "audit", "pairing_check.csv"), index=False)
print(len(R), "simulated arm runs; output diverged:", int(np.isfinite(R.t_out).sum()), "; control diverged:", int(np.isfinite(R.t_cmd).sum()),
      "; output differs but control identical:", int((np.isfinite(R.t_out) & ~np.isfinite(R.t_cmd)).sum()), "; PAIRING VIOLATIONS:", int(R.violation.sum()))
print(R[R.violation].head(10).to_string())
