#!/bin/bash
# Regenerate the committed SDK part (sdk/gen/) from sdk/efr32fg25_sdk.slcp.
#
# Only needed when the SDK component list or a component configuration
# changes. A normal firmware build does NOT need Java or slc: it uses the
# committed output in sdk/gen/.
#
# Needs: slc-cli (>= 6.0) + Java 21, and the Simplicity SDK checkout
# (git-lfs objects for the xG25 RAIL library pulled). See README.md.
#
# usage: tools/sdk_generate.sh <target> [SISDK_DIR]
#   target: brd4271a       (EFR32FG25, 863-870 MHz, on BRD4001A) -> EU_1_1a radio config
#           brd4271a-mode3 (same board) -> EU plan 33 (863.1 MHz + n*200 kHz, 35 channels),
#                          PHY mode 3 (150 kbit/s 2-FSK, h = 0.5) = FAN 1.0 EU class 2 mode 3
#           brd4270b       (EFR32FG25, 902-928 MHz, on BRD4002A) -> NA_1_1b radio config
# The radio config is picked by the SDK component radio_config_brd_wisun_singlephy
# from the board. That component has no mode 3 config for BRD4271A, so for
# brd4271a-mode3 the generated radio_settings.radioconf is switched to the SDK
# PHY PHY_WISUN_FAN_1v1_863MHz_Plan33_2FSK_3_EU and the project is generated a
# second time (slc keeps an existing project config file and regenerates
# rail_config.c from it). Output: sdk/gen/<target>/.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
TARGET=${1:?usage: tools/sdk_generate.sh <brd4271a|brd4271a-mode3|brd4270b> [SISDK_DIR]}
PHY_OVERRIDE=
case "$TARGET" in
    brd4271a) BOARD=brd4271a; MAINBOARD=brd4001a ;;
    brd4271a-mode3) BOARD=brd4271a; MAINBOARD=brd4001a
        PHY_OVERRIDE="PHY_WISUN_FAN_1v1_863MHz_Plan32_2FSK_1a_EU 256 324 PHY_WISUN_FAN_1v1_863MHz_Plan33_2FSK_3_EU 0 34" ;;
    brd4270b) BOARD=brd4270b; MAINBOARD=brd4002a ;;
    *) echo "unknown target $TARGET" >&2; exit 1 ;;
esac
SISDK_DIR=${2:-${SISDK_DIR:-$REPO/../.deps/simplicity_sdk}}
SISDK_DIR=$(cd "$SISDK_DIR" && pwd)
OUT=$REPO/sdk/gen/$TARGET
SLC=${SLC:-slc}

slc_generate() {
    "$SLC" generate -p "$REPO/sdk/efr32fg25_sdk.slcp" -s "$SISDK_DIR" -d "$OUT" \
        --with "$BOARD,$MAINBOARD" -o cmake -nocp --no-daemon 2>&1 | grep -v "^Warning: Duplicate definition of register" || true
}
rm -rf "$OUT"
"$SLC" signature trust --sdk "$SISDK_DIR" >/dev/null
slc_generate
if [ -n "$PHY_OVERRIDE" ]; then
    set -- $PHY_OVERRIDE
    RC=$OUT/config/rail/radio_settings.radioconf
    grep -q "$1" "$RC" || { echo "$RC does not select $1" >&2; exit 1; }
    sed -i -e "s|$1|$4|g" -e "s|<channel_number_start>$2<|<channel_number_start>$5<|" \
        -e "s|<channel_number_end>$3<|<channel_number_end>$6<|" "$RC"
    slc_generate
    grep -q '"result_code": "0"' "$OUT/autogen/radioconf_generation_log.json" || { echo "Radio Configurator failed for $4" >&2; exit 1; }
fi
test -f "$OUT/cmake_gcc/efr32fg25_sdk.cmake" || { echo "slc generate failed" >&2; exit 1; }
test -f "$OUT/autogen/rail_config.c" || { echo "rail_config.c not generated (Radio Configurator failed)" >&2; exit 1; }

# Make the generated CMake fragment relocatable and consumable from our own
# top-level CMakeLists.txt:
#  - SDK_PATH comes from the SISDK_DIR cache variable (no absolute paths);
#  - project-relative paths ("../autogen", "../config") are anchored to this file;
#  - the SLC template main.c is dropped (apps/common/main.c is used instead);
#  - the map-file option referring to the template target is dropped;
#  - the SLT package path is dropped (not used by this project).
F=$OUT/cmake_gcc/efr32fg25_sdk.cmake
sed -i \
    -e 's|^set(SDK_PATH .*|if(NOT DEFINED SDK_PATH)\n  message(FATAL_ERROR "SDK_PATH (Simplicity SDK root) must be set before including this file")\nendif()|' \
    -e '/^set(COPIED_SDK_PATH /d' \
    -e '/^set(PKG_PATH /d' \
    -e '/"\.\.\/main\.c"/d' \
    -e '/-Wl,-Map=\$<TARGET_FILE_DIR:efr32fg25_sdk>/d' \
    -e 's|"\.\./|"${CMAKE_CURRENT_LIST_DIR}/../|g' \
    -e 's|^   "\.\./|   "${CMAKE_CURRENT_LIST_DIR}/../|' \
    -e '/^# BEGIN_SIMPLICITY_STUDIO_METADATA/d' \
    "$F"
# Template files we do not use.
rm -rf "$OUT/cmake_iar" "$OUT/main.c" "$OUT/cmake_gcc/CMakeLists.txt" "$OUT/cmake_gcc/CMakePresets.json" \
       "$OUT/cmake_gcc/toolchain.cmake" "$OUT/cmake_gcc/autogen_toolchain.cmake" \
       "$OUT/cmake_gcc/readme.md" "$OUT/autogen/linkerfile.icf"
# The absolute SDK path also lands in a few metadata files; strip it.
grep -rl "$SISDK_DIR" "$OUT" | while read -r f; do sed -i "s|$SISDK_DIR|\${SDK_PATH}|g" "$f"; done
echo "SDK part regenerated in $OUT"
