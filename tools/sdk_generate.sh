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
# usage: tools/sdk_generate.sh [SISDK_DIR]
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
SISDK_DIR=${1:-${SISDK_DIR:-$REPO/../.deps/simplicity_sdk}}
SISDK_DIR=$(cd "$SISDK_DIR" && pwd)
OUT=$REPO/sdk/gen
SLC=${SLC:-slc}

rm -rf "$OUT"
"$SLC" signature trust --sdk "$SISDK_DIR" >/dev/null
"$SLC" generate -p "$REPO/sdk/efr32fg25_sdk.slcp" -s "$SISDK_DIR" -d "$OUT" \
    -o cmake -nocp --no-daemon 2>&1 | grep -v "^Warning: Duplicate definition of register" || true
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
