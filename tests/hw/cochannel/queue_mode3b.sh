#!/bin/bash
# Mode 3, second queue (2026-11-09): 250-byte frames at higher SIR, then
# continuous interferers (co-channel and adjacent channels).
# usage: queue_mode3b.sh <out dir>
set -u
H=$(cd "$(dirname "$0")" && pwd)
O=$1; mkdir -p "$O"
run() { echo "=== $* $(date -Is)"; python3 "$H/sweep.py" "$O" "$@"; }
st() { echo "=== stream $* $(date -Is)"; python3 "$H/stream.py" "$O" "$@"; }
HI=$(for p in 25 30 35 40 50 60 70 90 110 130 150; do printf -- "-30:%s," $p; done); HI=${HI%,}
run fine_l250_d900 --delta 900 --len 250 --pairs "$HI" --n 150 --n-near 150
CO=$(for p in $(seq -150 10 150); do printf -- "%s:-150," $p; done); CO=${CO%,}
st co_tone --mode tone --pairs "$CO" --n 200
st co_pn9 --mode pn9 --pairs "$CO" --n 200
ADJ=$(for p in $(seq -150 20 150); do printf -- "-150:%s," $p; done); ADJ=${ADJ%,}
for m in tone pn9; do
  for o in 1 -1; do
    st adj_${m}_o${o} --mode $m --ioffset $o --pairs "$ADJ" --n 200
    st adj_${m}_o${o}_X --mode $m --ioffset $o --ikit X --pairs "-150:0,-150:100,-150:150" --n 200
  done
done
echo "=== done $(date -Is)"
