#!/usr/bin/env python3
"""Run one xapp-coord simulation and write its run manifest (protocol Appendix A, §9.4).

Usage:
  run_case.py --run-id ID --out-dir DIR [--family F] [--scenario S] [--binary B] -- <sim args>

Writes into DIR (which must not exist or must be empty):
  <ID>-*.csv, <ID>-config.json        simulator outputs (prefix set via --out)
  <ID>.log                            stdout+stderr
  <ID>-manifest.json                  provenance and status

Manifest fields: run_id, experiment_family, scenario_id, status, return_code, error_tail,
wall-clock start/end, git commit/branch/dirty state (+ diff hash when dirty), binary path and
SHA-256, ns-3 and ns3-oran commits, full argv, config_hash (SHA-256 of the canonical JSON of the
resolved parameters + RNG settings, i.e. independent of output paths), RNG seed/run/stream map,
and every output file with size and SHA-256.

Importable: run_case(...) returns the manifest dict.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import uuid
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NS3_ROOT = os.environ.get("NS3_ROOT", os.path.expanduser("~/ns-3.42"))
DEFAULT_BIN = os.path.join(NS3_ROOT, "build/scratch/xapp-coord/ns3.42-xapp-coord-default")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(args, cwd):
    try:
        return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                              check=True).stdout.strip()
    except Exception:
        return None


CODE_PREFIXES = ("simulation/", "scripts/", "configs/", "analysis/", "CMakeLists.txt")


def git_state(path):
    """Commit plus dirty state. Dirty = tracked modifications, or UNTRACKED files under code
    paths (an untracked source file changes the build but is invisible to `git diff`).
    Untracked result/data files do not make the tree dirty."""
    try:
        raw = subprocess.run(["git", "status", "--porcelain", "--untracked-files=all"], cwd=path,
                             capture_output=True, text=True, check=True).stdout
    except Exception:
        raw = ""
    dirty_files = []
    for line in raw.splitlines():
        code, name = line[:2], line[3:]
        if code == "??" and not name.startswith(CODE_PREFIXES):
            continue
        dirty_files.append(f"{code.strip()} {name}")
    state = {
        "commit": git(["rev-parse", "HEAD"], path),
        "branch": git(["rev-parse", "--abbrev-ref", "HEAD"], path),
        "dirty": bool(dirty_files),
    }
    if dirty_files:
        diff = git(["diff", "HEAD"], path) or ""
        state["diff_sha256"] = hashlib.sha256(diff.encode()).hexdigest()
        state["dirty_files"] = dirty_files
    return state


def config_hash(config):
    canon = {"params": config.get("params", {}), "rng": config.get("rng", {})}
    return hashlib.sha256(json.dumps(canon, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def run_case(run_id, out_dir, sim_args, family="", scenario="", binary=DEFAULT_BIN):
    os.makedirs(out_dir, exist_ok=True)
    if os.listdir(out_dir) and any(f.startswith(run_id) for f in os.listdir(out_dir)):
        raise SystemExit(f"refusing to overwrite existing outputs for {run_id} in {out_dir}")
    out_dir = os.path.abspath(out_dir)
    prefix = os.path.join(out_dir, run_id)
    # The ns3-oran SQLite repository commits every insert with fsync (synchronous=FULL), which made runs I/O-bound.
    # It is written on tmpfs and moved next to the other outputs after the run; simulation outputs are byte-identical
    # (verified 18/18 CSVs; DB tables identical except upstream defect U11's address-dependent cmdname bytes).
    shm = "/dev/shm/xapp-coord-db"
    os.makedirs(shm, exist_ok=True)
    db_tmp = os.path.join(shm, f"{run_id}-{uuid.uuid4().hex}.db")  # unique per run (batch workers are threads of one process)
    argv = [binary, f"--out={prefix}", f"--db-file={db_tmp}", *sim_args]
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = os.path.join(NS3_ROOT, "build/lib") + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")

    start = dt.datetime.now(dt.timezone.utc)
    with open(prefix + ".log", "w") as log:
        rc = subprocess.run(argv, cwd=out_dir, stdout=log, stderr=subprocess.STDOUT, env=env).returncode
    end = dt.datetime.now(dt.timezone.utc)
    if os.path.exists(db_tmp):  # durable copy next to the other outputs (fsync), then remove the tmpfs file
        shutil.copyfile(db_tmp, prefix + ".db")
        fd = os.open(prefix + ".db", os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
        os.remove(db_tmp)

    cfg_path = prefix + "-config.json"
    config = json.load(open(cfg_path)) if os.path.exists(cfg_path) else {}
    with open(prefix + ".log") as f:
        tail = f.read()[-2000:]
    # A DB file is only produced by RIC runs; remove an empty placeholder never happens here.
    outputs = []
    for name in sorted(os.listdir(out_dir)):
        if name.startswith(run_id) and not name.endswith("-manifest.json"):
            p = os.path.join(out_dir, name)
            outputs.append({"file": name, "bytes": os.path.getsize(p), "sha256": sha256_file(p)})

    manifest = {
        "schema": "xapp-coord-run-manifest/1",
        "run_id": run_id,
        "experiment_family": family,
        "scenario_id": scenario,
        "status": "completed" if rc == 0 else "failed",
        "return_code": rc,
        "error_tail": "" if rc == 0 else tail,
        "wall_clock": {"start_utc": start.isoformat(), "end_utc": end.isoformat(),
                       "seconds": round((end - start).total_seconds(), 3)},
        "code": {
            "repository": git_state(REPO),
            "ns3": {"commit": git(["rev-parse", "HEAD"], NS3_ROOT)},
            "ns3_oran": {"commit": git(["rev-parse", "HEAD"], os.path.join(NS3_ROOT, "contrib/oran"))},
            "binary": {"path": binary, "sha256": sha256_file(binary)},
        },
        "argv": argv,
        "config_hash": config_hash(config) if config else None,
        "rng": config.get("rng"),
        "params": config.get("params"),
        "outputs": outputs,
    }
    with open(prefix + "-manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)
    return manifest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--family", default="")
    ap.add_argument("--scenario", default="")
    ap.add_argument("--binary", default=DEFAULT_BIN)
    ap.add_argument("sim_args", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    sim_args = a.sim_args[1:] if a.sim_args[:1] == ["--"] else a.sim_args
    m = run_case(a.run_id, a.out_dir, sim_args, a.family, a.scenario, a.binary)
    print(json.dumps({k: m[k] for k in ("run_id", "status", "return_code", "config_hash")}))
    sys.exit(0 if m["status"] == "completed" else 1)


if __name__ == "__main__":
    main()
