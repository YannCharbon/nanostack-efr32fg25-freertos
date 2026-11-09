#!/bin/bash
# EU mode 1a (50 kbit/s, 8-byte preamble: SHR 1600 us) on 2026-11-09; kits
# flashed with build_railtest.sh's mode1a image. RAIL channel 256 = 863.1 MHz.
# usage: queue_mode1a.sh <out dir> <offset us>
set -u
H=$(cd "$(dirname "$0")" && pwd)
O=$1; OFF=$2; mkdir -p "$O"
P="--bitrate 50000 --shr 10 --channel 256 --dfirst 5000 --offset $OFF"
run() { echo "=== $* $(date -Is)"; python3 "$H/sweep.py" "$O" "$@" $P; }
FINE=$(for p in $(seq -80 5 20); do printf "%s:-30," $p; done; for p in $(seq -25 5 20) 30 40 50 70 90 110 150; do printf -- "-30:%s," $p; done)
FINE=${FINE%,}
# locked frame vs interferer over its whole payload (second frame after the first's SFD)
run fine_l50_d1800 --delta 1800 --len 50 --pairs "$FINE" --n 120 --n-near 200
# capture timing: second frame 6 dB stronger at several offsets inside the first's SHR
for d in 0 100 200 400 600 800 1000 1200 1400 1500 1700; do
  run dscan_l50_d$d --delta $d --len 50 --pairs "-30:-90,30:-150" --n 60 --n-near 150
done
run cap_l50_d0 --delta 0 --len 50 --points AB --n 60 --n-near 200
echo "=== done $(date -Is)"
