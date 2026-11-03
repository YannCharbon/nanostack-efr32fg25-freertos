# Shared helpers of the kit scripts (run_hw_mesh.sh, run_hw_lfn4.sh,
# hw_lfn_power.sh). Source it after setting HW (this directory) and cd'ing
# into the run directory.
#
# - Consoles start with empty logs (console.py), and every search starts at
#   the offset recorded at the kit's last reset, so a line from an earlier
#   boot or run never satisfies a check.
# - kit_reset checks the boot banner; a kit sleeping in EM2 (LFN) can miss
#   a reset over the debug port, so it retries, then switches the target
#   supply off and on.
# - check records PASS/FAIL; hw_exit exits 1 if any check failed.
#
# Kits are named (br, rn, lfn, ...) and given by J-Link serial number:
# console_start <name> <SN>; the console is found through /dev/serial/by-id.
#
# SPDX-License-Identifier: Apache-2.0

DEV=EFR32FG25B222F1920IM56
BANNER="nanostack-efr32fg25-freertos .* starting"
HW_FAILS=0
HW_PIDS=""
declare -A HW_OFF HW_SN

tty_of() { readlink -f /dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_*"$1"-if00; }

console_start() { # name SN
    local t; t=$(tty_of "$2")
    [ -c "$t" ] || { echo "FAIL: no console for kit $2"; exit 2; }
    HW_SN[$1]=$2
    HW_OFF[$1]=0
    "$HW/console.py" "$t" "$1.log" "$1.cmd" & HW_PIDS="$HW_PIDS $!"
}

consoles_stop() { [ -n "$HW_PIDS" ] && kill $HW_PIDS 2>/dev/null; }
trap consoles_stop EXIT

logsize() { stat -c %s "$1.log" 2>/dev/null || echo 0; }

# Log of kit <name> since its last reset
since_reset() { tail -c +$(( ${HW_OFF[$1]:-0} + 1 )) "$1.log" 2>/dev/null; }

# waitfor <name> <pattern> <timeout s> [from byte]: seconds waited, or
# returns 1. Searches from the given offset, by default the kit's last reset.
waitfor() {
    local t0 from=${4:-${HW_OFF[$1]:-0}}
    t0=$(date +%s)
    while ! tail -c +$(( from + 1 )) "$1.log" 2>/dev/null | grep -a -q "$2"; do
        sleep 1
        if [ $(( $(date +%s) - t0 )) -ge "$3" ]; then return 1; fi
    done
    echo $(( $(date +%s) - t0 ))
}

# kit_reset <name>: reset, check the boot banner. Records the new log offset.
kit_reset() {
    local k o sn=${HW_SN[$1]}
    for k in 1 2 3 4; do
        o=$(logsize "$1")
        if [ "$k" -le 2 ]; then
            commander device reset --serialno "$sn" -d $DEV >/dev/null 2>&1
        else
            # a sleeping LFN can miss the debug-port reset: power cycle
            commander adapter power -s "$sn" off >/dev/null 2>&1
            sleep 6
            commander adapter power -s "$sn" on >/dev/null 2>&1
        fi
        if waitfor "$1" "$BANNER" 12 "$o" >/dev/null; then
            HW_OFF[$1]=$o
            [ "$k" -gt 1 ] && echo "note: $1 reset needed $k attempts"
            return 0
        fi
    done
    echo "FAIL: $1 (kit $sn) did not reboot (no boot banner)"
    HW_FAILS=$((HW_FAILS + 1))
    HW_OFF[$1]=$o
    return 1
}

# mark_reset <name>: the kit resets itself now (CLI 'nvm erase'); record the
# offset, then call expect_boot <name>
mark_reset() { HW_OFF[$1]=$(logsize "$1"); }
expect_boot() {
    waitfor "$1" "$BANNER" 15 >/dev/null && return 0
    echo "FAIL: $1 did not reboot (no boot banner)"; HW_FAILS=$((HW_FAILS + 1)); return 1
}

send() { echo "$2" > "$1.cmd"; }

# distinct <name> <extended regex> [from byte]: number of distinct matching
# strings in the kit's log since its last reset (the matched text only, so a
# repeated line counts once)
distinct() {
    tail -c +$(( ${3:-${HW_OFF[$1]:-0}} + 1 )) "$1.log" 2>/dev/null | grep -a -o -E "$2" | sort -u | wc -l
}

check() { # description, then a command; PASS/FAIL line
    local d=$1; shift
    if "$@"; then echo "PASS: $d"; else echo "FAIL: $d"; HW_FAILS=$((HW_FAILS + 1)); fi
}

fail() { echo "FAIL: $*"; HW_FAILS=$((HW_FAILS + 1)); }

hw_exit() {
    if [ "$HW_FAILS" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
    echo "RESULT: FAIL ($HW_FAILS)"; exit 1
}

# aem_ua <SN> <seconds>: average current in µA over a window (empty if the
# AEM gave nothing)
aem_ua() {
    commander aem measure -s "$1" --windowlength $(( $2 * 1000 )) 2>&1 |
        awk '/Current \[mA\]/ {printf "%.2f", $NF * 1000} /Current \[uA\]/ {printf "%.2f", $NF}'
}
