#!/bin/bash
# Mode 3: modulated interferer two channels away (alternate channel), 2026-11-09.
set -u
H=$(cd "$(dirname "$0")" && pwd)
O=$1; mkdir -p "$O"
st() { echo "=== stream $* $(date -Is)"; python3 "$H/stream.py" "$O" "$@"; }
ADJ=$(for p in $(seq -150 20 150); do printf -- "-150:%s," $p; done); ADJ=${ADJ%,}
for o in 2 -2; do
  st alt_pn9_o${o} --mode pn9 --ioffset $o --pairs "$ADJ" --n 200
  st alt_pn9_o${o}_X --mode pn9 --ioffset $o --ikit X --pairs "-150:0,-150:100,-150:150" --n 200
done
echo "=== done $(date -Is)"
