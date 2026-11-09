#!/bin/bash
# usage: flash.sh <image.s37> <kit serial>...   (mass erase, then VCOM 115200 8N1 no handshake)
set -euo pipefail
source "$(cd "$(dirname "$0")/../../../.." && pwd)/.deps/env.sh"
IMG=$1; shift
for sn in "$@"; do
    out=$(commander flash "$IMG" -s "$sn" --device EFR32FG25B222F1920IM56 --masserase 2>&1 || true)
    grep -q "Flashing completed successfully" <<<"$out" || { echo "$out" | tail -5; echo "flash failed on $sn" >&2; exit 1; }
    commander vcom config -s "$sn" --baudrate 115200 --handshake none >/dev/null 2>&1 || true
    echo "flashed $sn"
done
