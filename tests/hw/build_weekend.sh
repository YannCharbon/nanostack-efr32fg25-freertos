#!/bin/bash
# Builds the images of the weekend LFN campaign (tests/hw/WEEKEND_2026-11-07.md)
# that the GS11 images (build/wk-net, build/wk-lfnr, build/wk-lfn) do not
# cover, from the same stack tree, and records every image of the campaign in
# build/wk-images.txt (stack and firmware commits, md5), which run_weekend.sh
# checks before it starts. Build only: no kit is touched.
#
#   build/wk-int-balanced  wisun_br: interop PHY (EU mode 1a, PHY mode 1, plan 32,
#                          network "hw5lfn"), FW_LFN_PROFILE=balanced (parent:
#                          LFN broadcast 60 s x 15)
#   build/wk-int-eco       the same, FW_LFN_PROFILE=eco (600 s x 60)
#   build/wk-balanced      EU mode 3, wisun_br, wisun_rn, wisun_lfn with
#                          FW_LFN_PROFILE=balanced
#   build/wk-eco           the same with eco
# All with the options of the wk-* (hk-*) images: FW_BOARD_FLASH_DPD=ON,
# FW_FLASH_FS=ON, FW_LFN_PARENT=ON (and FW_RN_LFN_PARENT=ON for mode 3).
#
# usage: build_weekend.sh [--check-base <firmware commit>]
#   --check-base: also builds the default border router, router and LFN
#   (the hk-net/hk-lfn options, FW_LFN_PROFILE empty) from this tree and from
#   that firmware commit (a git worktree), against the same stack, and lists
#   the application objects whose code differs (expected: only cli.c and
#   net_test.c, the ping wait argument).
# Environment: NANOSTACK_DIR (default ../nanostack-generic-standalone), ENV.
# Exit status 0 if every image built.
set -u
HW=$(cd "$(dirname "$0")" && pwd)
FW=$(cd "$HW/../.." && pwd)
source "${ENV:-$FW/../.deps/env.sh}"
NS=${NANOSTACK_DIR:-$FW/../nanostack-generic-standalone}
NS=$(cd "$NS" && pwd)
cd "$FW" || exit 2
rc=0
INTEROP='APP_WS_PHY_MODE_ID=1;APP_WS_CHAN_PLAN_ID=32;APP_NETWORK_NAME="hw5lfn"'
COMMON="-DNANOSTACK_DIR=$NS -DFW_BOARD_FLASH_DPD=ON -DFW_FLASH_FS=ON -DFW_LFN_PARENT=ON"
build() { # dir preset targets... (cmake options in OPTS)
    local d=$1 preset=$2; shift 2
    echo "== $d ($preset ${OPTS[*]}): $*"
    if cmake --preset "$preset" -B "build/$d" $COMMON "${OPTS[@]}" > "build/$d.cfg.log" 2>&1 &&
       cmake --build "build/$d" --target "$@" > "build/$d.build.log" 2>&1; then
        grep -h "FLASH\|RAM" "build/$d.build.log" | tail -2 | sed 's/^/   /'
    else
        echo "FAIL: build/$d (see build/$d.cfg.log, build/$d.build.log)"; rc=1
    fi
}
for p in balanced eco; do
    OPTS=("-DFW_LFN_PROFILE=$p" "-DAPP_EXTRA_DEFS=$INTEROP")
    build "wk-int-$p" fw wisun_br
    OPTS=("-DFW_LFN_PROFILE=$p" -DFW_RN_LFN_PARENT=ON)
    build "wk-$p" fw-mode3 wisun_br wisun_rn wisun_lfn
done

# the record of the campaign's images
{
    echo "# weekend campaign images, built $(date '+%F %T')"
    echo "stack $(git -C "$NS" rev-parse --short HEAD)$(git -C "$NS" diff --quiet HEAD -- . 2>/dev/null || echo ' (working tree differs from the commit)')"
    echo "firmware $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- apps CMakeLists.txt port drivers 2>/dev/null || echo ' (working tree differs from the commit)')"
    for f in build/wk-net/wisun_br.hex build/wk-net/wisun_rn.hex build/wk-lfnr/wisun_lfn.hex build/wk-lfn/wisun_lfn.hex \
             build/wk-int-balanced/wisun_br.hex build/wk-int-eco/wisun_br.hex \
             build/wk-balanced/wisun_br.hex build/wk-balanced/wisun_lfn.hex \
             build/wk-eco/wisun_br.hex build/wk-eco/wisun_lfn.hex \
             ../.deps/lfn_build/wisun_soc_lfn_cli/build/debug/wisun_soc_lfn_cli.s37 \
             ../.deps/lfn_build/wisun_rcp_uart/build/debug/wisun_rcp.s37; do
        if [ -f "$f" ]; then
            ns=""; [ -f "$(dirname "$f")/CMakeCache.txt" ] &&
                ns=" stack-dir $(sed -n 's/^NANOSTACK_DIR:PATH=//p' "$(dirname "$f")/CMakeCache.txt")"
            echo "image $f $(md5sum < "$f" | cut -c1-12) $(date -r "$f" '+%F %T')$ns"
        else
            echo "missing $f"
        fi
    done
} > build/wk-images.txt
cat build/wk-images.txt
grep -q "^missing build/wk-\(int\|balanced\|eco\)" build/wk-images.txt && rc=1
grep -q "^missing build/wk-\(net\|lfnr\|lfn\)/" build/wk-images.txt &&
    echo "note: the GS11 images (build/wk-net, wk-lfnr, wk-lfn) are not built yet"

if [ "${1:-}" = "--check-base" ] && [ -n "${2:-}" ]; then
    B=$(mktemp -d); git worktree add -q "$B/fw" "$2" || exit 2
    for side in new base; do
        src=$FW; [ $side = base ] && src=$B/fw
        ( cd "$src" && cmake --preset fw-mode3 -B "$B/$side" $COMMON -DFW_RN_LFN_PARENT=ON \
              -DSISDK_DIR="$FW/../.deps/simplicity_sdk" -DMBEDTLS_DIR="$FW/../.deps/mbedtls-3.0.0" > "$B/$side.cfg.log" 2>&1 &&
          cmake --build "$B/$side" --target wisun_br wisun_rn wisun_lfn > "$B/$side.build.log" 2>&1 ) ||
            { echo "FAIL: default build ($side), logs in $B"; rc=1; }
    done
    echo "== default images (FW_LFN_PROFILE empty) against firmware $2: application objects whose code differs"
    ( cd "$B/new" && find CMakeFiles/wisun_*.dir/apps -name "*.obj" ) | sort | while read -r o; do
        a=$(arm-none-eabi-objdump -d "$B/new/$o" | tail -n +3 | md5sum)
        b=$(arm-none-eabi-objdump -d "$B/base/$o" 2>/dev/null | tail -n +3 | md5sum)
        [ "$a" = "$b" ] || echo "   differs: $o"
    done
    git worktree remove --force "$B/fw"; rm -rf "$B"
fi
exit $rc
