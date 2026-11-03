#!/bin/bash
# Four kits: border router, two routers (LFN parents), our LFN (README, "LFN
# multicast subscriptions, better parent and weak link on the kits").
# Images: wisun_br (FW_LFN_PARENT), wisun_rn (FW_RN_LFN_PARENT) on both
# routers, wisun_lfn built for the scenario (FW_TEST_MAC_ALLOW,
# FW_LFN_MC_GROUP, FW_LFN_PARENT_CHECK_S, FW_TX_POWER_DDBM). The images are
# flashed beforehand; this script resets the kits and drives the consoles.
#
# Cold boot: BR, then the routers (R2_LATE=1: router 2 only once the LFN has
# joined, a better parent that appears later); when router 1 holds an LGTK,
# the LFN. TXP_R1, TXP_R2, TXP_BR: TX power (dBm, CLI txpower) set on that
# kit once its console runs. Then ROUNDS rounds, one every PERIOD s: from the
# border router a UDP datagram to GROUP (with GROUP set) and one to the LFN's
# address; the LFN answers each with "ack:<text>" to the border router. The
# script counts the answers, and prints the LFN's parent changes and RSL
# lines (LFN traces stay on: APP_LFN_QUIET_AFTER_S=0 in its build).
#
# Kit J-Link serial numbers: BR_SN, R1_SN, R2_SN, LFN_SN.
# usage: run_hw_lfn4.sh <workdir>
# Environment: ROUNDS (20), PERIOD (15), GROUP (empty: no multicast), R2_LATE
# (0), R2_ERASE (0; 1: router 2 forgets its stored keys when it appears;
# the border router then drops its frames as replays until it restarts: frame
# counters), R2_LATE=2 (router 2 runs from the start at TXP_R2 and turns to
# R2_TXP_LATER dBm once the LFN has joined, a TX power scenario; with
# R1_EUI, the router the LFN did not choose), TXP_BR, TXP_R1, TXP_R2, ENV (commander environment, default
# ../../../.deps/env.sh).
set -u
W=$1
: "${BR_SN:?}" "${R1_SN:?}" "${R2_SN:?}" "${LFN_SN:?}"
ROUNDS=${ROUNDS:-20}; PERIOD=${PERIOD:-15}; GROUP=${GROUP:-}; R2_LATE=${R2_LATE:-0}
DEV=EFR32FG25B222F1920IM56
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
tty_of() { readlink -f /dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_*$1-if00; }
mkdir -p "$W"; cd "$W"
"$HW/console.py" "$(tty_of $BR_SN)" br.log br.cmd & P1=$!
"$HW/console.py" "$(tty_of $R1_SN)" r1.log r1.cmd & P2=$!
"$HW/console.py" "$(tty_of $R2_SN)" r2.log r2.cmd & P3=$!
"$HW/console.py" "$(tty_of $LFN_SN)" lfn.log lfn.cmd & P4=$!
trap 'kill $P1 $P2 $P3 $P4 2>/dev/null' EXIT
sleep 1
waitfor() { # file pattern timeout [from byte] -> seconds waited, or fails
    local t0=$(date +%s)
    while ! tail -c +$(( ${4:-0} + 1 )) "$1" 2>/dev/null | grep -a -q "$2"; do
        sleep 1
        if [ $(( $(date +%s) - t0 )) -ge "$3" ]; then return 1; fi
    done
    echo $(( $(date +%s) - t0 ))
}
reset() { commander device reset --serialno $1 -d $DEV >/dev/null 2>&1; }
txp() { [ -n "$2" ] && { sleep 2; echo "txpower $2" > $1.cmd; }; }
reset $BR_SN; txp br "${TXP_BR:-}"
sleep 3
reset $R1_SN; txp r1 "${TXP_R1:-}"
[ "$R2_LATE" = 1 ] || { reset $R2_SN; txp r2 "${TXP_R2:-}"; }
t=$(waitfor r1.log "if 1: ARM_NWK_BOOTSTRAP_READY" 900) || { echo "FAIL router 1 join"; exit 1; }
echo "router 1 joined ${t}s after its reset"
t2=$(waitfor r1.log "LGTK recv\|LGTK set" 300) && echo "router 1 LGTK ${t2}s after its join" || echo "FAIL router 1 LGTK"
# the reset of a kit sleeping in EM2 can be missed: check the boot banner
for k in 1 2 3; do
    o=$(stat -c %s lfn.log 2>/dev/null || echo 0)
    reset $LFN_SN
    waitfor lfn.log "WS tasklet init" 10 $o >/dev/null && break
done
t=$(waitfor lfn.log "if 1: ARM_NWK_BOOTSTRAP_READY" 900 $o) || { echo "FAIL LFN join"; exit 1; }
echo "LFN joined ${t}s after its reset"
tail -c +$((o + 1)) lfn.log | grep -a -h "LFN: LPA from\|LFN: parent \|EARO multicast\|joining" | head -8
sleep 15   # the router's DAO for the LFN (LFN_STACK.md §9)
LFN_GUA=$(grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" lfn.log | tail -1 | awk '{print $3}')
[ -n "$LFN_GUA" ] || { echo "FAIL no LFN address in its log"; exit 1; }
echo "LFN $LFN_GUA"
if [ "$R2_LATE" = 2 ]; then
    # Router 2 runs from the start at TXP_R2 (weak, like router 1) and
    # becomes a better parent now: TX power R2_TXP_LATER (dBm)
    # (the router the LFN did not choose, if R1_EUI, router 1's EUI-64 as
    # the LFN logs it, is given)
    k=r2
    if [ -n "${R1_EUI:-}" ] && ! tail -c +$((o + 1)) lfn.log | grep -a "LFN: parent chosen" | tail -1 | grep -q "$R1_EUI"; then k=r1; fi
    echo "txpower ${R2_TXP_LATER:-10}" > $k.cmd
    echo "$k at ${R2_TXP_LATER:-10} dBm at $(date +%s) (appears later)"
elif [ "$R2_LATE" = 1 ]; then
    # R2_ERASE=1: forget its stored keys (CLI nvm erase, which resets it):
    # a full join, as a new router
    if [ "${R2_ERASE:-0}" = 1 ]; then echo "nvm erase" > r2.cmd; sleep 3; else reset $R2_SN; fi
    txp r2 "${TXP_R2:-}"
    echo "router 2 reset at $(date +%s) (appears later)"
fi
for i in $(seq 1 "$ROUNDS"); do
    [ -n "$GROUP" ] && { echo "udp $GROUP mc$i" > br.cmd; sleep 3; }
    echo "udp $LFN_GUA uc$i" > br.cmd
    sleep "$PERIOD"
done
sleep 20
mc=$(grep -a -c "udp rx from \[$LFN_GUA\].*ack:mc" br.log)
uc=$(grep -a -c "udp rx from \[$LFN_GUA\].*ack:uc" br.log)
[ -n "$GROUP" ] && echo "group $GROUP: $mc of $ROUNDS answered by the LFN"
echo "unicast: $uc of $ROUNDS answered by the LFN"
echo "LFN parent changes and checks:"
grep -a -h "LFN: better parent\|LFN: parent changed\|LFN: check:\|old parent .* released\|parent .* lost\|LFN: parent .*, PAN" lfn.log
echo "nodes" > br.cmd; sleep 2
grep -a "node fd12" br.log | tail -4
for k in br r1 r2; do echo "status -v" > $k.cmd; done; sleep 3
echo "faults: $(grep -a -c FAULT br.log r1.log r2.log lfn.log | tr '\n' ' ')"
