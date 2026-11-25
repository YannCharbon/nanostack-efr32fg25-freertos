#!/bin/bash
# Build one variant of the measurement firmware.
# usage: build.sh OUTDIR NAME "-DFM_ROLE=0 -DFM_WIN_US=300 ..."
# The project is generated once in OUTDIR/gen; each variant is a make with its defines.
# SLCP=<file>.slcp and RADIOCONF=<file>.radioconf choose the project and the PHY,
# CONF="NAME=VALUE ..." adds configuration entries (use a separate OUTDIR per choice).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
source "$REPO/../.deps/env.sh"
OUT=$(mkdir -p "$1" && cd "$1" && pwd); NAME=$2; DEFS=${3:-}
G=$OUT/gen
if [ ! -f "$G/fmmeas.Makefile" ]; then
  rm -rf "$G"; mkdir -p "$G"
  cp "$HERE/${SLCP:-fmmeas.slcp}" "$G/fmmeas.slcp"; cp "$HERE/app.c" "$G/"
  # CONF="NAME=VALUE ...": extra configuration entries (the project's configuration list is last)
  for c in ${CONF:-}; do printf '  - name: %s\n    value: "%s"\n' "${c%%=*}" "${c#*=}" >> "$G/fmmeas.slcp"; done
  gen() { slc generate -p "$G/fmmeas.slcp" -s "$SISDK" -d "$G" --with brd4271a,brd4001a -o makefile --no-daemon 2>&1 | grep -v "Duplicate definition" | tail -2; }
  gen
  cp "$HERE/${RADIOCONF:-radio_settings.radioconf}" "$G/config/rail/radio_settings.radioconf"
  gen
  grep -q '"result_code": "0"' "$G/autogen/radioconf_generation_log.json" || { echo "radio config failed:"; cat "$G/autogen/radioconf_generation_log.json" | head -40; exit 1; }
fi
cp "$HERE/app.c" "$G/app.c"
: > "$G/fm_cfg.h"
for d in $DEFS; do d=${d#-D}; echo "#define ${d%%=*} ${d#*=}" >> "$G/fm_cfg.h"; done
touch "$G/app.c"
make -C "$G" -f fmmeas.Makefile -j"$(nproc)" ARM_GCC_DIR=/usr POST_BUILD_EXE_LINUX=true >"$G/make.log" 2>&1 || { grep -m5 -B2 -A3 "error" "$G/make.log"; exit 1; }
cp "$G/build/debug/fmmeas.s37" "$OUT/$NAME.s37"
echo "$OUT/$NAME.s37"
