#!/usr/bin/env python3
"""Batch execution with provenance (data policy D-ORG-2, approved 2026-11-22).

run_batch(name, cases, jobs) runs every case through run_case.py in parallel and stores
  bulk outputs   ~/xapp-research-data/s2/<name>-<UTC>/          (external, not in Git)
  provenance     results/s2/<name>/<UTC>/manifests.jsonl         (Git: one manifest per line)
                 results/s2/<name>/<UTC>/SHA256SUMS              (Git: every external file)
                 results/s2/<name>/<UTC>/BATCH.json              (Git: batch metadata, status counts)
A case is (run_id, family, scenario_id, [sim args]). Returns (data_dir, repo_dir, manifests).
Refuses to start if the repository code is dirty, so every batch maps to one commit.
"""
import concurrent.futures as cf
import datetime as dt
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from run_case import git_state, run_case, sha256_file  # noqa: E402

DATA_ROOT = os.path.expanduser(os.environ.get("XAPP_DATA_ROOT", "~/xapp-research-data/s2"))


def run_batch(name, cases, jobs=4, allow_dirty=False, stage="s2", data_root=None):
    state = git_state(REPO)
    if state["dirty"] and not allow_dirty:
        raise SystemExit(f"code is dirty ({state['dirty_files']}); commit before running batch {name}")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    data = os.path.join(data_root or DATA_ROOT, f"{name}-{stamp}")
    repo = os.path.join(REPO, "results", stage, name, stamp)
    os.makedirs(data)
    os.makedirs(repo)
    t0 = dt.datetime.now(dt.timezone.utc)
    manifests = {}
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(run_case, rid, data, args, fam, scen): rid for rid, fam, scen, args in cases}
        for i, fu in enumerate(cf.as_completed(futs), 1):
            m = fu.result()
            manifests[futs[fu]] = m
            if i % 20 == 0 or i == len(futs):
                print(f"[{name}] {i}/{len(futs)} done", flush=True)
    with open(os.path.join(repo, "manifests.jsonl"), "w") as f:
        for rid, *_ in cases:
            f.write(json.dumps(manifests[rid], sort_keys=True) + "\n")
    with open(os.path.join(repo, "SHA256SUMS"), "w") as f:
        for fn in sorted(os.listdir(data)):
            f.write(f"{sha256_file(os.path.join(data, fn))}  {fn}\n")
    status = {}
    for m in manifests.values():
        status[m["status"]] = status.get(m["status"], 0) + 1
    meta = {"batch": name, "stamp": stamp, "external_data": data, "code": state,
            "runs": len(cases), "status_counts": status,
            "wall_clock_s": round((dt.datetime.now(dt.timezone.utc) - t0).total_seconds(), 1)}
    json.dump(meta, open(os.path.join(repo, "BATCH.json"), "w"), indent=2)
    print(f"[{name}] {status} -> {data}")
    return data, repo, manifests
