#!/bin/bash
# Mode 3, capture timing at a finer grid (2026-11-09). usage: queue_mode3c.sh <out dir>
set -u
H=$(cd "$(dirname "$0")" && pwd)
O=$1; mkdir -p "$O"
for d in 150 250 350 450 550 650 750 1000 2000; do
  echo "=== delta $d $(date -Is)"
  python3 "$H/sweep.py" "$O" "cap_l50_d$d" --delta $d --len 50 --pairs "-51:-150,30:-150,150:-150" --n 60 --n-near 150
done
echo "=== done $(date -Is)"
