# Shared helpers of the kit scripts (run_hw_mesh.sh, run_hw_lfn4.sh,
# hw_lfn_power.sh, run_hw_lfn_dl.sh, run_hw_gs11.sh, run_weekend.sh). Source
# it after setting HW (this directory) and cd'ing into the run directory.
#
# - Consoles start with empty logs (console.py), and every search starts at
#   the offset recorded at the kit's last reset, so a line from an earlier
#   boot or run never satisfies a check.
# - Each console runs under a watchdog: if console.py exits (USB link lost,
#   tty re-enumerated, a second process on the port), it is started again on
#   the kit's current tty, appending to the same log, and the log gets a
#   "=== console restarted" line. A lost console no longer ends a run
#   (2026-11-04: a lost border-router console stopped a long run).
# - send gives up after 5 s if nobody reads the command FIFO (a dead
#   console), instead of blocking the run.
# - kit_reset checks the boot banner; a kit sleeping in EM2 (LFN) can miss
#   a reset over the debug port, so it retries, then switches the target
#   supply off and on.
# - check records PASS/FAIL; hw_exit exits 1 if any check failed.
# - HW_DRY=1 (dry run): every hardware action (commander, console, command
#   to a kit, AEM) is printed as "DRY: ..." and not done; waits on a log
#   succeed at once, sleeps are printed and skipped (hw_sleep, hw_now keep a
#   virtual clock), so a script prints its whole sequence in seconds.
#
# Kits are named (br, rn, lfn, ...) and given by J-Link serial number:
# console_start <name> <SN>; the console is found through /dev/serial/by-id.
#
# SPDX-License-Identifier: Apache-2.0

DEV=EFR32FG25B222F1920IM56
BANNER="nanostack-efr32fg25-freertos .* starting"
HW_FAILS=0
HW_PIDS=""
HW_DRY=${HW_DRY:-0}
HW_VT=$(date +%s)
declare -A HW_OFF HW_SN

dry() { [ "$HW_DRY" = 1 ]; }
# virtual clock in a dry run (advanced by hw_sleep), wall clock otherwise
hw_now() { if dry; then echo "$HW_VT"; else date +%s; fi; }
hw_sleep() { if dry; then echo "DRY: sleep $1"; HW_VT=$(( HW_VT + ${1%.*} )); else sleep "$1"; fi; }
# commander, or its command line in a dry run
cmdr() { if dry; then echo "DRY: commander $*" >&2; return 0; fi; commander "$@"; }

tty_of() { readlink -f /dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_*"$1"-if00; }

# tty_users <tty>: PIDs (other than ours) that hold the tty open
tty_users() {
    local p f
    for p in /proc/[0-9]*; do
        for f in "$p"/fd/*; do
            [ "$(readlink "$f" 2>/dev/null)" = "$1" ] && { echo "${p#/proc/}"; break; }
        done
    done 2>/dev/null
}

console_start() { # name SN
    HW_SN[$1]=$2
    HW_OFF[$1]=0
    if dry; then echo "DRY: console $1 on kit $2 -> $1.log, commands $1.cmd"; return 0; fi
    # our images print at 115200 without handshake; Silicon Labs' black-box
    # steps leave a kit's board controller at 9600 RTS/CTS (2026-11-06)
    commander vcom config -s "$2" --baudrate 115200 --handshake none >/dev/null 2>&1
    local t; t=$(tty_of "$2")
    [ -c "$t" ] || { echo "FAIL: no console for kit $2"; exit 2; }
    rm -f "$1.stop"
    # watchdog: console.py again (appending) whenever it exits, until stopped
    (
        trap 'kill $cpid 2>/dev/null; exit 0' TERM
        app=${CONSOLE_APPEND:-0}
        while [ ! -e "$1.stop" ]; do
            t=$(tty_of "$2")
            if [ -c "$t" ]; then
                CONSOLE_APPEND=$app "$HW/console.py" "$t" "$1.log" "$1.cmd" & cpid=$!
                wait $cpid
            fi
            [ -e "$1.stop" ] && break
            app=1
            sleep 3
            echo "$(date +%s.%N | cut -c1-14) === console restarted (kit $2, tty ${t:-none})" >> "$1.log"
        done
    ) & HW_PIDS="$HW_PIDS $!"
}

consoles_stop() {
    dry && return 0
    local p n
    for n in "${!HW_SN[@]}"; do touch "$n.stop" 2>/dev/null; done
    for p in $HW_PIDS; do pkill -TERM -P "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
    HW_PIDS=""
}
trap consoles_stop EXIT

logsize() { stat -c %s "$1.log" 2>/dev/null || echo 0; }

# Log of kit <name> since its last reset
since_reset() { tail -c +$(( ${HW_OFF[$1]:-0} + 1 )) "$1.log" 2>/dev/null; }

# waitfor <name> <pattern> <timeout s> [from byte]: seconds waited, or
# returns 1. Searches from the given offset, by default the kit's last reset.
waitfor() {
    if dry; then echo "DRY: wait for '$2' on $1, up to $3 s" >&2; echo 0; return 0; fi
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
    if dry; then cmdr device reset --serialno "$sn" -d $DEV; waitfor "$1" "$BANNER" 12 >/dev/null; return 0; fi
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

# kits_reset_together <name> <name>: both resets started at the same instant
# (two commander processes); each banner checked, a kit that missed it is
# reset again alone (kit_reset). Prints "boot offset <ms>": the difference of
# the two banners' host times (USB/UART latency: a few ms of uncertainty).
kits_reset_together() {
    local a=$1 b=$2 oa ob ta tb pa pb
    if dry; then
        cmdr device reset --serialno "${HW_SN[$a]}" -d $DEV
        cmdr device reset --serialno "${HW_SN[$b]}" -d $DEV
        echo "boot offset 0 ms (dry run)"; return 0
    fi
    oa=$(logsize "$a"); ob=$(logsize "$b")
    # wait for these two only: a bare wait would also wait for the consoles
    commander device reset --serialno "${HW_SN[$a]}" -d $DEV >/dev/null 2>&1 & pa=$!
    commander device reset --serialno "${HW_SN[$b]}" -d $DEV >/dev/null 2>&1 & pb=$!
    wait $pa $pb
    if waitfor "$a" "$BANNER" 12 "$oa" >/dev/null; then HW_OFF[$a]=$oa; else echo "note: $a missed the paired reset"; kit_reset "$a"; fi
    if waitfor "$b" "$BANNER" 12 "$ob" >/dev/null; then HW_OFF[$b]=$ob; else echo "note: $b missed the paired reset"; kit_reset "$b"; fi
    ta=$(since_reset "$a" | grep -a -m1 "$BANNER" | awk '{print $1}')
    tb=$(since_reset "$b" | grep -a -m1 "$BANNER" | awk '{print $1}')
    if [ -n "$ta" ] && [ -n "$tb" ]; then
        echo "boot offset $(echo "($tb - $ta) * 1000" | bc | awk '{printf "%.0f", ($1 < 0 ? -$1 : $1)}') ms"
    else
        echo "boot offset unknown"
    fi
}

# kit_off <name>: the kit leaves the network until kit_on: target supply
# off, or, where the adapter cannot switch it (BRD4002A), CLI 'halt' (radio
# idle, stack stopped; the FFN images only)
kit_off() {
    dry && { cmdr adapter power -s "${HW_SN[$1]}" off; return 0; }
    commander adapter power -s "${HW_SN[$1]}" off >/dev/null 2>&1 && return 0
    local o; o=$(logsize "$1")
    send "$1" "halt"
    waitfor "$1" "halted:" 10 "$o" >/dev/null && return 0
    fail "$1: could not take it off the network"; return 1
}
# kit_on <name>: supply on, boot banner checked; records the new log offset
kit_on() {
    dry && { cmdr adapter power -s "${HW_SN[$1]}" on; return 0; }
    local o; o=$(logsize "$1")
    commander adapter power -s "${HW_SN[$1]}" on >/dev/null 2>&1
    if waitfor "$1" "$BANNER" 15 "$o" >/dev/null; then HW_OFF[$1]=$o; return 0; fi
    kit_reset "$1"
}

# mark_reset <name>: the kit resets itself now (CLI 'nvm erase'); record the
# offset, then call expect_boot <name>
mark_reset() { HW_OFF[$1]=$(logsize "$1"); }
expect_boot() {
    waitfor "$1" "$BANNER" 15 >/dev/null && return 0
    echo "FAIL: $1 did not reboot (no boot banner)"; HW_FAILS=$((HW_FAILS + 1)); return 1
}

# send <name> <command>: to the kit's console; gives up after 5 s if the
# console does not read its FIFO (reported, the run goes on)
send() {
    if dry; then echo "DRY: $1 <- $2"; return 0; fi
    timeout 5 tee "$1.cmd" <<<"$2" >/dev/null || echo "note: command '$2' to $1 not delivered (console not reading)"
}

# distinct <name> <extended regex> [from byte]: number of distinct matching
# strings in the kit's log since its last reset (the matched text only, so a
# repeated line counts once)
distinct() {
    tail -c +$(( ${3:-${HW_OFF[$1]:-0}} + 1 )) "$1.log" 2>/dev/null | grep -a -o -E "$2" | sort -u | wc -l
}

check() { # description, then a command; PASS/FAIL line
    local d=$1; shift
    if dry; then echo "DRY: check: $d"; return 0; fi
    if "$@"; then echo "PASS: $d"; else echo "FAIL: $d"; HW_FAILS=$((HW_FAILS + 1)); fi
}

fail() { echo "FAIL: $*"; HW_FAILS=$((HW_FAILS + 1)); }

hw_exit() {
    if [ "$HW_FAILS" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
    echo "RESULT: FAIL ($HW_FAILS)"; exit 1
}

# aem_ua <SN> <seconds>: average current in µA over a window (empty if the
# AEM gave nothing). The AEM reads the board controller's current monitor:
# no debugger access to the target, so it does not disturb an LFN.
aem_ua() {
    if dry; then echo "DRY: commander aem measure -s $1 --windowlength $(( $2 * 1000 ))" >&2; echo "0.00"; return 0; fi
    commander aem measure -s "$1" --windowlength $(( $2 * 1000 )) 2>&1 |
        awk '/Current \[mA\]/ {printf "%.2f", $NF * 1000} /Current \[uA\]/ {printf "%.2f", $NF}'
}

# aem_dump <SN> <seconds> <out base>: an AEM dump (100 kHz on BRD4002A,
# 10 kHz on BRD4001A), analysed at once (aem_floor.py -> <base>.json,
# aem_wakes.py -> <base>.wakes.txt); the CSV is compressed unless
# AEM_KEEP_CSV=0 removes it (100 kHz: about 30 MB of CSV a second).
aem_dump() {
    if dry; then
        echo "DRY: commander aem dump -s $1 --duration $2 -o $3.csv; aem_floor.py $3.csv --json $3.json; aem_wakes.py --line $3.json > $3.wakes.txt"
        return 0
    fi
    commander aem dump -s "$1" --duration "$2" -o "$3.csv" >/dev/null 2>&1 || { echo "note: AEM dump $3 failed"; return 1; }
    python3 "$HW/aem_floor.py" "$3.csv" --json "$3.json" > "$3.floor.txt" 2>&1
    python3 "$HW/aem_wakes.py" --line "$3.json" > "$3.wakes.txt" 2>&1
    if [ "${AEM_KEEP_CSV:-1}" = 0 ]; then rm -f "$3.csv"; else gzip -f "$3.csv"; fi
}
