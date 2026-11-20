#!/usr/bin/env bash
# Phase 1 load sweep: raise the offered rate per UE in cell 1 (A), keep cell 2 (B) fixed,
# RIC off, 2 seeds per point. Results go to ~/xapp-analysis/phase1/.
set -euo pipefail

cd ~/ns-3.42
OUT=~/xapp-analysis/phase1
mkdir -p "$OUT"

for RATE in 0.5 1 1.5 2 3 4 5 6; do
  for RUN in 1 2; do
    TAG="a${RATE}_r${RUN}"
    ./ns3 run "xapp-coord --ric=false --rate-a=${RATE} --rate-b=1 --sim-time=30s \
      --RngRun=${RUN} --out=${OUT}/${TAG}" > "${OUT}/${TAG}.log" 2>&1
    echo "done rate-a=${RATE} Mbps/UE, run ${RUN}"
  done
done
echo "All runs finished. Now: python3 plot_phase1.py ${OUT}"
