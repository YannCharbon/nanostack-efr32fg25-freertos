#!/bin/bash
# Build Silicon Labs' RAILtest for the BRD4271A with a Wi-SUN EU PHY, for the
# co-channel / capture measurements (see docs/COCHANNEL_RESULTS.md).
#
# usage: tests/hw/cochannel/build_railtest.sh <mode3|mode1a> <out dir>
#   mode3  : PHY_WISUN_FAN_1v1_863MHz_Plan33_2FSK_3_EU  (150 kbit/s, h = 0.5, 35 ch x 200 kHz)
#   mode1a : PHY_WISUN_FAN_1v1_863MHz_Plan32_2FSK_1a_EU (50 kbit/s, h = 0.5; the radioconf of
#            the firmware's brd4271a target, channels 256..324 x 100 kHz grid)
# Output: <out dir>/<mode>/build/debug/rail_soc_railtest.s37
#
# One change to RAILtest (a copy of app_main.c, the SDK is not touched):
# received packets are printed with only the first 4 payload bytes and no
# per-packet extras, so the 115200-baud console keeps up with ~30 frames/s.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
MODE=${1:?usage: build_railtest.sh <mode3|mode1a> <out dir>}
OUT=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)/$MODE
source "$REPO/../.deps/env.sh"
case "$MODE" in
    mode3)  RC=$REPO/sdk/gen/brd4271a-mode3/config/rail/radio_settings.radioconf ;;
    mode1a) RC=$REPO/sdk/gen/brd4271a/config/rail/radio_settings.radioconf ;;
    *) echo "unknown mode $MODE" >&2; exit 1 ;;
esac
SLCP=$SISDK/app/rail/example/soc/rail_soc_railtest/rail_soc_railtest.slcp
gen() { slc generate -p "$SLCP" -s "$SISDK" -d "$OUT" --with brd4271a,brd4001a -o makefile --no-daemon 2>&1 \
        | grep -v "Duplicate definition" | tail -1; }
rm -rf "$OUT"; mkdir -p "$OUT"
gen
cp "$RC" "$OUT/config/rail/radio_settings.radioconf"
gen
grep -q '"result_code": "0"' "$OUT/autogen/radioconf_generation_log.json" || { echo "radio config failed" >&2; exit 1; }
# compact rxPacket print
mkdir -p "$OUT/patched"
python3 - "$SISDK/platform/radio/rail_lib/apps/railtest/app_main.c" "$OUT/patched/app_main.c" <<'PY'
import sys
s = open(sys.argv[1]).read()
old_loop = 'for (int i = 0; i < dataLength; i++) {\n      RAILTEST_PRINTF(" 0x%.2x", data[i]);'
assert old_loop in s
s = s.replace(old_loop, 'for (int i = 0; i < dataLength && i < 4; i++) {\n      RAILTEST_PRINTF(" 0x%.2x", data[i]);')
old = '"len:%d,timeUs:%u,timePos:%u,durationUs:%u,crc:%s,filterMask:0x%x,rssi:%d,lqi:%d,phy:%d",'
assert old in s
s = s.replace(old, '"len:%d,timeUs:%u,timePos:%u,durationUs:%u,crc:%s,f:0x%x,rssi:%d,lqi:%d,phy:%d",')
old2 = '    responsePrintContinue(\n      "isAck:%s,syncWordId:%d,antenna:%d,channelHopIdx:%d,channel:%u",'
assert old2 in s
s = s.replace(old2, '    if (0) responsePrintContinue(\n      "isAck:%s,syncWordId:%d,antenna:%d,channelHopIdx:%d,channel:%u",')
open(sys.argv[2], "w").write(s)
PY
sed -i "s|\$(SDK_PATH)/platform/radio/rail_lib/apps/railtest/app_main.c|$OUT/patched/app_main.c|g" "$OUT/rail_soc_railtest.project.mak"
# app_main.c includes headers by relative name from its own directory
sed -i "s|^ -I\$(SDK_PATH)/platform/radio/rail_lib/apps/railtest \\\\| -I\$(SDK_PATH)/platform/radio/rail_lib/apps/railtest \\\\|" "$OUT/rail_soc_railtest.project.mak"
make -C "$OUT" -f rail_soc_railtest.Makefile -j"$(nproc)" ARM_GCC_DIR=/usr POST_BUILD_EXE_LINUX=true >"$OUT/make.log" 2>&1 \
    || { tail -30 "$OUT/make.log"; exit 1; }
ls -l "$OUT/build/debug/rail_soc_railtest.s37"
