#!/bin/bash
# Mode 3 measurement queue run on 2026-11-09 (kits flashed with the mode3 image).
# usage: queue_mode3.sh <out dir>
set -u
H=$(cd "$(dirname "$0")" && pwd)
O=$1; mkdir -p "$O"
FINE=$(for p in $(seq -80 5 20); do printf "%s:-30," $p; done; for p in $(seq -25 5 20); do printf -- "-30:%s," $p; done)
FINE=${FINE%,}
run() { echo "=== $* $(date -Is)"; python3 "$H/sweep.py" "$O" "$@"; }
# 1. locked frame vs interferer over its whole payload: fine SIR grid around 0 dB
run fine_l50_d900 --delta 900 --len 50 --pairs "$FINE" --n 200 --n-near 200
# 2. capture by an early second frame (during the first's preamble)
run cap_l50_d100 --delta 100 --len 50 --points AB
run cap_l50_d0 --delta 0 --len 50 --points AB
run cap_l50_d700 --delta 700 --len 50 --points AB
# 3. 250-byte frames
run fine_l250_d900 --delta 900 --len 250 --pairs "$FINE" --n 150 --n-near 150
run cap_l250_d100 --delta 100 --len 250 --points AB --n 60 --n-near 200
# 4. second frame over the last ~0.8 ms of the first only
run fine_l50_d2800 --delta 2800 --len 50 --pairs "$FINE" --n 100 --n-near 200
echo "=== done $(date -Is)"
