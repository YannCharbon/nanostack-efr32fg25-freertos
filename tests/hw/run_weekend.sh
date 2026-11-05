#!/bin/bash
# The weekend LFN campaign of 2026-11-07/08 on the four FG25 kits
# (tests/hw/WEEKEND_2026-11-07.md): one run, phases in turn, built on the kit
# scripts (hwlib.sh, run_hw_gs11.sh, run_hw_lfn_dl.sh, lfn_blackbox/s8_run.py).
#
#   A  our network (border router, router, two LFNs; GS11 images): GS11 paired
#      resets, AEM dumps of the idle LFNs (timer wakes after the housekeeping
#      change), router restarts with keys in flash            about 1 h 05
#   B  two pairs at the same time, about 16.5 h:
#      X  our border router + our LFN with the Balanced, then Eco settings:
#         downlinks after silences, AEM windows and dumps
#      Y  Silicon Labs' LFN (Balanced, then Eco) under our border router with
#         the same profile; then Eco and Balanced under wsbrd
#   C  our network again: long idle with downlinks after silences up to 2 h,
#      AEM windows on both LFN kits, dumps on the 100 kHz kit   about 8 h
#
# usage: run_weekend.sh [--dry-run] [--detach] [--phases ABC] [--out DIR]
#        run_weekend.sh --summary DIR      (summary of an existing output only)
#   --detach   start in its own session (setsid, nohup), output in DIR/run.log;
#              closing the terminal or logging out does not stop it
#   --dry-run  print every command (flashing, resets, console commands, AEM,
#              sub-scripts and their own commands) without touching the kits,
#              without taking the kits' lock, and with sleeps skipped
# Environment: ECO_GAPS (the silences of pair X's Eco part, s; default
# "3600 39600 1800 600": a downlink right after the join, one an hour later, one
# after 11 h of silence, which spans the 10 h sync, and one 30 min later),
# OUT (output directory), ENV.
# Output: DIR/<phase>_<step>/ for each step, DIR/summary.txt (one line per
# check: PASS, FAIL or FIGURE), rebuilt after every step.
# The kits' lock (flock on .../locks/silabs-kits) is held for the whole run;
# a line in silabs-kits.notes records the start, the end and the images left.
# Exit status 0 if every check passed.
#
# Rules learnt on the kits: no RAM read of an LFN during a run (it misses sync
# frames); one console process per kit at a time (a step's script owns its
# kits' consoles; the steps of a pair use disjoint kits); consoles restart
# themselves if lost (hwlib); every reset checked by the boot banner.
set -u
HW=$(cd "$(dirname "$0")" && pwd)
FW=$(cd "$HW/../.." && pwd)
ROOT=$(cd "$FW/.." && pwd)
LOCKDIR=/tmp/claude-1000/-home-yann-mesh-emulator-rework/d4ddb9d0-f90a-45e0-b53f-a1c82161aa85/scratchpad/locks
DRY=0; DETACH=0; PHASES=ABC; OUT=${OUT:-$ROOT/.deps/captures/weekend-2026-11-07}
while [ $# -gt 0 ]; do
    case $1 in
        --dry-run) DRY=1 ;;
        --detach) DETACH=1 ;;
        --phases) PHASES=$2; shift ;;
        --out) OUT=$2; shift ;;
        --summary) exec python3 "$HW/weekend_summary.py" "$2" ;;
        *) echo "usage: $0 [--dry-run] [--detach] [--phases ABC] [--out DIR] | --summary DIR"; exit 2 ;;
    esac
    shift
done
ECO_GAPS=${ECO_GAPS:-3600 39600 1800 600}
export HW_DRY=$DRY
mkdir -p "$OUT"
if [ "$DETACH" = 1 ]; then
    args=(--phases "$PHASES" --out "$OUT"); [ "$DRY" = 1 ] && args+=(--dry-run)
    setsid nohup "$0" "${args[@]}" > "$OUT/run.log" 2>&1 < /dev/null &
    echo "started, pid $!, log $OUT/run.log, summary $OUT/summary.txt"
    exit 0
fi
trap '' HUP
source "${ENV:-$ROOT/.deps/env.sh}"

# Kits (J-Link serial numbers; README "LFN multicast subscriptions ...")
K1=440240046   # BRD4271A on BRD4001A, AEM does not measure: border router / Silicon Labs' parent
K2=440235475   # BRD4270B on BRD4001A, AEM does not see the radio: router / pair X border router
K3=440290182   # BRD4271A on BRD4002A, AEM at 100 kHz: our LFN
K4=440235509   # BRD4271A on BRD4001A, AEM at 10 kHz: our LFN / Silicon Labs' LFN
B=$FW/build
SL_LFN=$ROOT/.deps/lfn_build/wisun_soc_lfn_cli/build/debug/wisun_soc_lfn_cli.s37
SL_RCP=$ROOT/.deps/lfn_build/wisun_rcp_uart/build/debug/wisun_rcp.s37
DEV=EFR32FG25B222F1920IM56
FAILS=0

say() { echo "$(date '+%F %T') $*"; }
run() { # a command, or its printout in a dry run
    if [ "$DRY" = 1 ]; then echo "DRY: $*"; return 0; fi
    "$@"
}
summary() { python3 "$HW/weekend_summary.py" "$OUT" > /dev/null 2>&1 || true; }
note() { [ "$DRY" = 1 ] && { echo "DRY: notes <- $*"; return; }; echo "$(date '+%F %T') $*" >> "$LOCKDIR/silabs-kits.notes"; }

# flash <kit> <image> [erase]: mass erase first with "erase" (keys and frame
# counters gone: a cold join with EAP-TLS follows)
flash() {
    local sn=$1 img=$2 log="$OUT/flash.log"
    say "flash $sn $(basename "$(dirname "$img")")/$(basename "$img")${3:+ (mass erase)}" | tee -a "$log"
    if [ "$DRY" = 1 ]; then
        [ -n "${3:-}" ] && echo "DRY: commander device masserase --serialno $sn -d $DEV"
        echo "DRY: commander flash $img --serialno $sn -d $DEV"
        return 0
    fi
    [ -n "${3:-}" ] && commander device masserase --serialno "$sn" -d $DEV >> "$log" 2>&1
    if commander flash "$img" --serialno "$sn" -d $DEV >> "$log" 2>&1; then return 0; fi
    sleep 5   # once more (a sleeping LFN can miss the first connection)
    commander flash "$img" --serialno "$sn" -d $DEV >> "$log" 2>&1 && return 0
    echo "FAIL: flashing $img on $sn" | tee -a "$log"; FAILS=$((FAILS + 1)); return 1
}

# step <dir> <command...>: a sub-script with its output in DIR/<dir>/out.txt
step() {
    local d=$OUT/$1; shift
    mkdir -p "$d"
    echo "$*" > "$d/cmd.txt"
    say "step $(basename "$d"): $*"
    if [ "$DRY" = 1 ]; then "$@" 2>&1 | tee "$d/out.txt" | sed 's/^/    /'; else "$@" 2>&1 | head -c 500M > "$d/out.txt"; fi   # a runaway step stops at 500 MB
    local rc=${PIPESTATUS[0]}
    [ "$DRY" = 1 ] && rc=0
    echo "exit $rc" >> "$d/out.txt"
    [ "$rc" = 0 ] || FAILS=$((FAILS + 1))
    say "step $(basename "$d") done, exit $rc"
    summary
    return 0
}

# ------------------------------------------------------------------ preflight
preflight() {
    local ok=1 f sn t u
    say "preflight"
    for f in "$B/wk-net/wisun_br.hex" "$B/wk-net/wisun_rn.hex" "$B/wk-lfnr/wisun_lfn.hex" \
             "$B/wk-int-balanced/wisun_br.hex" "$B/wk-int-eco/wisun_br.hex" \
             "$B/wk-balanced/wisun_br.hex" "$B/wk-balanced/wisun_lfn.hex" \
             "$B/wk-eco/wisun_br.hex" "$B/wk-eco/wisun_lfn.hex" "$SL_LFN" "$SL_RCP"; do
        [ -f "$f" ] || { echo "preflight: missing image $f"; ok=0; }
    done
    if [ -f "$B/wk-images.txt" ]; then
        sed 's/^/  images: /' "$B/wk-images.txt"
        grep -q "differs from the commit" "$B/wk-images.txt" && echo "preflight: note: some images were built from a working tree, not a commit"
    else
        echo "preflight: build/wk-images.txt missing (run tests/hw/build_weekend.sh)"; ok=0
    fi
    command -v commander > /dev/null || { echo "preflight: commander not found"; ok=0; }
    getcap "$ROOT/.deps/wisun-br-linux-v2.10.3/build/wsbrd" 2>/dev/null | grep -q cap_net_admin ||
        { echo "preflight: wsbrd lacks its capabilities (README of lfn_blackbox)"; ok=0; }
    for sn in $K1 $K2 $K3 $K4; do
        t=$(readlink -f /dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_*"$sn"-if00 2>/dev/null)
        if [ "$DRY" = 1 ]; then echo "DRY: kit $sn console ${t:-not found}"; continue; fi
        [ -c "$t" ] || { echo "preflight: kit $sn: no console tty"; ok=0; continue; }
        source "$HW/hwlib.sh"
        u=$(tty_users "$t")
        [ -z "$u" ] || { echo "preflight: kit $sn: $t is open by pid(s) $u (another console or tool): stop it first"; ok=0; }
    done
    [ "$(df -BG --output=avail "$OUT" | tail -1 | tr -dc 0-9)" -ge 20 ] || { echo "preflight: under 20 GB free in $OUT"; ok=0; }
    [ "$ok" = 1 ] || { [ "$DRY" = 1 ] && { echo "DRY: preflight problems above would stop the run"; return 0; }; return 1; }
}

# ---------------------------------------------------------------------- lock
lock() {
    if [ "$DRY" = 1 ]; then echo "DRY: flock -n $LOCKDIR/silabs-kits (held for the whole run)"; return 0; fi
    exec 9> "$LOCKDIR/silabs-kits"
    flock -n 9 || { echo "the kits' lock is held by another session (see $LOCKDIR/silabs-kits.notes)"; exit 3; }
    echo $$ > "$LOCKDIR/weekend.pid"
    note "LOCK taken by the weekend campaign (tests/hw/run_weekend.sh, phases $PHASES), holder pid $$ (weekend.pid), output $OUT; do not use the kits"
}
unlock() {
    [ "$DRY" = 1 ] && { echo "DRY: lock released"; return; }
    note "weekend campaign: state at release: ${STATE:-unknown}"
    note "LOCK released by the weekend campaign"
    flock -u 9
}

# ------------------------------------------------------------------- phases
phase_A() {
    say "=== phase A: our network, GS11 pairs, AEM dumps, router restarts"
    flash $K1 "$B/wk-net/wisun_br.hex" erase
    flash $K2 "$B/wk-net/wisun_rn.hex" erase
    flash $K3 "$B/wk-lfnr/wisun_lfn.hex" erase
    flash $K4 "$B/wk-lfnr/wisun_lfn.hex" erase
    # GS11: 10 paired resets (the first one a cold join: the kits are erased)
    step A1_gs11 env BR_SN=$K1 RN_SN=$K2 LFN_A=$K3 LFN_B=$K4 PAIRS=10 "$HW/run_hw_gs11.sh" "$OUT/A1_gs11"
    # the LFNs idle under the router (no console open): timer wakes, 1.02 s
    # wakes and floor, both kits at once
    mkdir -p "$OUT/A2_aem"
    say "step A2_aem: AEM dumps of 240 s on $K3 and $K4"
    ( cd "$OUT/A2_aem" && source "$HW/hwlib.sh" && trap - EXIT &&
      { aem_dump $K3 240 aem_${K3}_1 & aem_dump $K4 240 aem_${K4}_1 & wait; } ) > "$OUT/A2_aem/out.txt" 2>&1
    [ "$DRY" = 1 ] && cat "$OUT/A2_aem/out.txt" | sed 's/^/    /'
    summary
    # router restarts with keys in flash: 3 runs, 10 datagrams 30 s apart to
    # both LFNs, the router reset before the 4th (2026-11-04: traffic back
    # after 97-194 s, 1-3 of 8 datagrams lost per run: MIN_FRAC 70)
    for r in 1 2 3; do
        step A3_rnreset_$r env BR_SN=$K1 RN_SN=$K2 LFN_SNS="$K3 $K4" LISTEN_S=5 \
            GAPS="30 30 30 30 30 30 30 30 30 30" RN_RESET_BEFORE=4 MIN_FRAC=70 \
            "$HW/run_hw_lfn_dl.sh" "$OUT/A3_rnreset_$r"
    done
    STATE="$K1 wisun_br wk-net, $K2 wisun_rn wk-net, $K3 and $K4 wisun_lfn wk-lfnr (EU mode 3, keys in flash)"
}

pair_X() {
    flash $K2 "$B/wk-balanced/wisun_br.hex" erase
    flash $K3 "$B/wk-balanced/wisun_lfn.hex" erase
    # Balanced: listen 60 s, LFN broadcast 60 s x 15 (sync every 15 min);
    # 5 downlinks 30 min apart; AEM windows every 10 min, a 300 s dump every hour
    step BX1_balanced env BR_SN=$K2 RN_SN= LFN_SNS=$K3 LISTEN_S=60 JOIN_TMO=2700 \
        GAPS="1800 1800 1800 1800 1800" AEM_SN=$K3 AEM_EVERY=600 AEM_WIN=60 \
        AEM_DUMP_EVERY=3600 AEM_DUMP_S=300 "$HW/run_hw_lfn_dl.sh" "$OUT/BX1_balanced"
    flash $K2 "$B/wk-eco/wisun_br.hex" erase
    flash $K3 "$B/wk-eco/wisun_lfn.hex" erase
    # Eco: listen 300 s, LFN broadcast 600 s x 60 (sync every 10 h), listening
    # only in sync slots. The downlink after 11 h of silence, across a sync,
    # is the check of the stack fix of 2026-11-05 (emulator: such a downlink
    # missed every window after the 10 h sync). AEM windows every 15 min, a
    # 310 s dump (one unicast window at least) every 3 h
    step BX2_eco env BR_SN=$K2 RN_SN= LFN_SNS=$K3 LISTEN_S=300 JOIN_TMO=5400 \
        GAPS="$ECO_GAPS" AEM_SN=$K3 AEM_EVERY=900 AEM_WIN=60 \
        AEM_DUMP_EVERY=10800 AEM_DUMP_S=310 "$HW/run_hw_lfn_dl.sh" "$OUT/BX2_eco"
}

pair_Y() {
    local S8="$HW/lfn_blackbox/s8_run.py" dry=""
    [ "$DRY" = 1 ] && dry=--dry-run
    flash $K1 "$B/wk-int-balanced/wisun_br.hex" erase
    flash $K4 "$SL_LFN" erase
    step BY1_p3_balanced python3 "$S8" "$OUT/BY1_p3_balanced" --parent ours --profile balanced \
        --join-tmo 2700 --settle 1800 --duration 5400 --gap 900 --aem-every 600 --aem-win 60 \
        --dump-at 600,3600 --dump-s 300 $dry
    flash $K1 "$B/wk-int-eco/wisun_br.hex" erase
    step BY2_p3_eco python3 "$S8" "$OUT/BY2_p3_eco" --parent ours --profile eco \
        --join-tmo 5400 --settle 0 --duration 7200 --gap 1800 --aem-every 900 --aem-win 60 \
        --dump-at 600,4800 --dump-s 660 $dry
    flash $K1 "$SL_RCP" erase
    # under wsbrd with Eco's 10 h sync: a downlink and an uplink after the
    # join, the next ones after 10.2 h of silence (across the first sync):
    # Silicon Labs' own behaviour in the case of the stack fix of 2026-11-05
    step BY3_p1_eco python3 "$S8" "$OUT/BY3_p1_eco" --parent wsbrd --profile eco \
        --join-tmo 5400 --settle 0 --duration 37800 --gap 36600 --aem-every 900 --aem-win 60 \
        --dump-at 600,18000,36700 --dump-s 660 $dry
}

phase_B() {
    say "=== phase B: pair X (our LFN, Balanced then Eco) and pair Y (Silicon Labs' LFN) at the same time"
    if [ "$DRY" = 1 ]; then
        echo "--- pair X ($K2 border router, $K3 LFN)"; pair_X
        echo "--- pair Y ($K1 parent, $K4 Silicon Labs' LFN)"; pair_Y
    else
        pair_X > "$OUT/pairX.log" 2>&1 & local px=$!
        pair_Y > "$OUT/pairY.log" 2>&1 & local py=$!
        wait $px; wait $py
        cat "$OUT/pairX.log" "$OUT/pairY.log"
        FAILS=$((FAILS + $(cat "$OUT"/B*/out.txt 2>/dev/null | grep -c "^exit [1-9]")))
    fi
    STATE="$K1 wisun_rcp (Silicon Labs), $K4 wisun_soc_lfn_cli (Silicon Labs), $K2 wisun_br wk-eco, $K3 wisun_lfn wk-eco"
}

phase_C() {
    say "=== phase C: our network, long idle with downlinks after silences"
    flash $K1 "$B/wk-net/wisun_br.hex" erase
    flash $K2 "$B/wk-net/wisun_rn.hex" erase
    flash $K3 "$B/wk-lfnr/wisun_lfn.hex" erase
    flash $K4 "$B/wk-lfnr/wisun_lfn.hex" erase
    # 8 silences, 10 min to 2 h (7.8 h), both LFNs under the router; AEM
    # windows of 60 s every 30 min on both LFN kits; a 240 s dump at 100 kHz
    # on $K3 every 3 h
    step C1_idle env BR_SN=$K1 RN_SN=$K2 LFN_SNS="$K3 $K4" LISTEN_S=5 \
        GAPS="600 1800 3600 4000 5400 7200 3600 1800" \
        AEM_SN="$K3 $K4" AEM_EVERY=1800 AEM_WIN=60 AEM_DUMP_SN=$K3 AEM_DUMP_EVERY=10800 AEM_DUMP_S=240 \
        "$HW/run_hw_lfn_dl.sh" "$OUT/C1_idle"
    STATE="$K1 wisun_br wk-net, $K2 wisun_rn wk-net, $K3 and $K4 wisun_lfn wk-lfnr (EU mode 3, keys in flash), network running"
}

# ---------------------------------------------------------------------- main
say "weekend campaign, phases $PHASES, output $OUT$([ "$DRY" = 1 ] && echo ', DRY RUN')"
preflight || { say "preflight failed: nothing done"; exit 2; }
lock
trap 'say "stopped by a signal"; unlock; exit 4' INT TERM
for p in A B C; do
    case $PHASES in *$p*) "phase_$p" ;; esac
done
summary
unlock
say "campaign done; summary $OUT/summary.txt"
[ "$DRY" = 1 ] && { echo "dry run: the summary above is built from printouts, not results"; exit 0; }
cat "$OUT/summary.txt"
[ "$FAILS" = 0 ] && grep -q "^FAIL" "$OUT/summary.txt" 2>/dev/null && FAILS=1
exit $(( FAILS > 0 ))
