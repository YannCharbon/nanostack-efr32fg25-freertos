#!/bin/bash
# Run the two-frame sweeps of one PHY (kits flashed with build_railtest.sh's
# image for that PHY). usage: run_capture.sh <out dir> <len> <delta>...
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1; LEN=$2; shift 2
mkdir -p "$OUT"
for D in "$@"; do
    echo "=== len $LEN delta $D $(date -Is)"
    python3 "$HERE/sweep.py" "$OUT" "cap_l${LEN}_d${D}" --delta "$D" --len "$LEN" ${SWEEP_ARGS:-}
done
