#!/bin/bash
# GS11 on the kits: two of our LFNs reset at the same instant must both join
# promptly. Border router BR_SN (wisun_br, FW_LFN_PARENT), router RN_SN
# (wisun_rn, FW_RN_LFN_PARENT; empty: the LFNs under the border router), LFNs
# LFN_A and LFN_B (wisun_lfn). Images flashed beforehand.
#
# Cold boot of the border router and the router (once it holds an LGTK),
# then PAIRS times: both LFNs reset together (hwlib kits_reset_together: two
# commander resets started at once), each LFN's join time from its boot
# banner to ARM_NWK_BOOTSTRAP_READY, the offset between the two boots, and
# one UDP datagram from the border router to each LFN, answered. The first
# pair is a cold join if the kits were erased; the next ones rejoin with the
# keys in flash.
#
# Limit: two commander resets leave the boots tens to hundreds of ms apart,
# while two LFNs collide only if their discovery sweeps start within one
# frame's airtime (about 5 ms). The kits therefore check that both LFNs still
# join promptly with GS11's jitter (no regression), not the colliding case,
# which the emulator covers (SPREAD=0). The boot offsets are reported.
#
# usage: run_hw_gs11.sh <workdir>
# Environment: BR_SN, RN_SN, LFN_A, LFN_B, PAIRS (10), JOIN_TMO (600),
# JOIN_MAX (pass limit per LFN, s; 74: the longest join of one LFN on the
# kits so far, README "An LFN under a router on the kits"), LISTEN_S (5),
# ENV, HW_DRY. Prints one "pair <i>: ..." line per pair and PASS/FAIL lines;
# pairs.txt: <i> <join A s> <join B s> <boot offset ms> <udp A> <udp B>.
# Exit status 0 if every check passed.
set -u
W=$1
: "${BR_SN:?}" "${LFN_A:?}" "${LFN_B:?}"
RN_SN=${RN_SN:-}; PAIRS=${PAIRS:-10}; JOIN_TMO=${JOIN_TMO:-600}; JOIN_MAX=${JOIN_MAX:-74}
LISTEN_S=${LISTEN_S:-5}
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
mkdir -p "$W"; cd "$W" || exit 2
source "$HW/hwlib.sh"
console_start br "$BR_SN"
[ -n "$RN_SN" ] && console_start rn "$RN_SN"
console_start la "$LFN_A"
console_start lb "$LFN_B"
hw_sleep 1
kit_reset br || hw_exit
waitfor br "interfaces up" 30 >/dev/null || fail "border router not up"
if [ -n "$RN_SN" ]; then
    hw_sleep 3
    kit_reset rn || hw_exit
    waitfor rn "if 1: ARM_NWK_BOOTSTRAP_READY" 900 >/dev/null || { fail "router join"; hw_exit; }
    waitfor rn "LGTK set\|LGTK recv" 600 >/dev/null || fail "router LGTK within 600 s of its join"
fi
# join time of an LFN since its last reset: its uptime (ms, second field) at
# ARM_NWK_BOOTSTRAP_READY
join_s() { since_reset "$1" | grep -a -m1 "if 1: ARM_NWK_BOOTSTRAP_READY" | sed -n 's/^[0-9.]* \[\([0-9]*\)\].*/\1/p' | awk '{printf "%.1f", $1 / 1000}'; }
: > pairs.txt
for i in $(seq 1 "$PAIRS"); do
    off=$(kits_reset_together la lb)
    ja=none; jb=none
    waitfor la "if 1: ARM_NWK_BOOTSTRAP_READY" "$JOIN_TMO" >/dev/null && ja=$(join_s la)
    waitfor lb "if 1: ARM_NWK_BOOTSTRAP_READY" "$JOIN_TMO" >/dev/null && jb=$(join_s lb)
    dry && { ja=0; jb=0; }
    hw_sleep 35   # the LFNs' traces go quiet 30 s after the join
    ua=none; ub=none
    for l in la lb; do
        a=$(since_reset "$l" | grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" | tail -1 | awk '{print $3}')
        dry && a="fd12:3456::<$l>"
        [ -n "$a" ] || continue
        o=$(logsize br)
        send br "udp $a gs11-$i-$l"
        if waitfor br "udp rx from \[$a\].*ack:gs11-$i-$l" $(( 2 * LISTEN_S + 15 )) "$o" >/dev/null; then
            [ "$l" = la ] && ua=ok || ub=ok
        fi
    done
    dry && { ua=ok; ub=ok; }
    echo "$i ${ja:-none} ${jb:-none} ${off//[^0-9]/} $ua $ub" >> pairs.txt
    echo "pair $i: LFN $LFN_A joined ${ja:-none} s, LFN $LFN_B ${jb:-none} s after reset; $off; downlinks $ua/$ub"
done
n_ok=$(awk -v m="$JOIN_MAX" '$2 != "none" && $3 != "none" && $2 <= m && $3 <= m' pairs.txt | wc -l)
n_dl=$(awk '$5 == "ok" && $6 == "ok"' pairs.txt | wc -l)
check "GS11: both LFNs joined within $JOIN_MAX s in every pair ($n_ok of $PAIRS)" [ "$n_ok" -eq "$PAIRS" ]
check "GS11: both LFNs answered a downlink after every pair ($n_dl of $PAIRS)" [ "$n_dl" -eq "$PAIRS" ]
for k in br ${RN_SN:+rn} la lb; do
    # the whole log: a fault is printed at the boot after it
    check "no fault on $k" [ "$(cat "$k.log" 2>/dev/null | grep -a -c FAULT)" -eq 0 ]
done
hw_exit
