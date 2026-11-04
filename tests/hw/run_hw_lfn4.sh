#!/bin/bash
# Four kits: border router, two routers (LFN parents), our LFN (README, "LFN
# multicast subscriptions, better parent and weak link on the kits").
# Images: wisun_br (FW_LFN_PARENT), wisun_rn (FW_RN_LFN_PARENT) on both
# routers, wisun_lfn built for the scenario (FW_TEST_MAC_ALLOW,
# FW_LFN_MC_GROUP, FW_LFN_PARENT_CHECK_S, FW_TX_POWER_DDBM). The images are
# flashed beforehand; this script resets the kits and drives the consoles.
#
# Cold boot: BR, then the routers (R2_LATE=1: router 2 switched off (target
# supply) until the LFN has joined, a better parent that appears later); when router 1 holds an LGTK,
# the LFN. TXP_R1, TXP_R2, TXP_BR: TX power (dBm, CLI txpower) set on that
# kit once its console runs. Then ROUNDS rounds, one every PERIOD s: from the
# border router a UDP datagram to GROUP (with GROUP set) and one to the LFN's
# address; the LFN answers each with "ack:<text>" to the border router. The
# script counts the answers, and prints the LFN's parent changes and RSL
# lines (LFN traces stay on: APP_LFN_QUIET_AFTER_S=0 in its build).
#
# Every reset is checked (boot banner) and every search starts at the kit's
# last reset (tests/hw/hwlib.sh); answers are counted as distinct datagrams.
#
# Kit J-Link serial numbers: BR_SN, R1_SN, R2_SN, LFN_SN.
# usage: run_hw_lfn4.sh <workdir>
# Exit status 0 if every check passed: the LFN joined, every unicast
# datagram answered, every group datagram answered (with GROUP), no fault.
# MIN_UC / MIN_MC lower the required counts (default ROUNDS).
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
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
mkdir -p "$W"; cd "$W" || exit 2
source "$HW/hwlib.sh"
console_start br "$BR_SN"
console_start r1 "$R1_SN"
console_start r2 "$R2_SN"
console_start lfn "$LFN_SN"
sleep 1
txp() { [ -n "$2" ] && { sleep 2; send "$1" "txpower $2"; }; }
kit_reset br || hw_exit
txp br "${TXP_BR:-}"
sleep 3
kit_reset r1 || hw_exit
txp r1 "${TXP_R1:-}"
if [ "$R2_LATE" = 1 ]; then kit_off r2; else kit_reset r2; txp r2 "${TXP_R2:-}"; fi
t=$(waitfor r1 "if 1: ARM_NWK_BOOTSTRAP_READY" 900) || { fail "router 1 join"; hw_exit; }
echo "router 1 joined ${t}s after its reset"
if t2=$(waitfor r1 "LGTK recv\|LGTK set" 300); then echo "router 1 LGTK ${t2}s after its join"; else fail "router 1 LGTK"; fi
# the reset of a kit sleeping in EM2 can be missed: kit_reset checks the
# boot banner and retries
kit_reset lfn || hw_exit
t=$(waitfor lfn "if 1: ARM_NWK_BOOTSTRAP_READY" 900) || { fail "LFN join"; hw_exit; }
echo "LFN joined ${t}s after its reset"
since_reset lfn | grep -a -h "LFN: LPA from\|LFN: parent \|EARO multicast\|joining" | head -8
sleep 15   # the router's DAO for the LFN (LFN_STACK.md §9)
LFN_GUA=$(since_reset lfn | grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" | tail -1 | awk '{print $3}')
[ -n "$LFN_GUA" ] || { fail "no LFN address in its log"; hw_exit; }
echo "LFN $LFN_GUA"
if [ "$R2_LATE" = 2 ]; then
    # Router 2 runs from the start at TXP_R2 (weak, like router 1) and
    # becomes a better parent now: TX power R2_TXP_LATER (dBm)
    # (the router the LFN did not choose, if R1_EUI, router 1's EUI-64 as
    # the LFN logs it, is given)
    k=r2
    if [ -n "${R1_EUI:-}" ] && ! since_reset lfn | grep -a "LFN: parent chosen" | tail -1 | grep -q "$R1_EUI"; then k=r1; fi
    send $k "txpower ${R2_TXP_LATER:-10}"
    echo "$k at ${R2_TXP_LATER:-10} dBm at $(date +%s) (appears later)"
elif [ "$R2_LATE" = 1 ]; then
    # R2_ERASE=1: forget its stored keys (CLI nvm erase, which resets it):
    # a full join, as a new router
    # router 2 was off until now (target supply); on, with its stored keys,
    # or R2_ERASE=1: forgets them
    kit_on r2
    if [ "${R2_ERASE:-0}" = 1 ]; then sleep 2; mark_reset r2; send r2 "nvm erase"; expect_boot r2; fi
    txp r2 "${TXP_R2:-}"
    echo "router 2 on at $(date +%s) (appears later)"
    if t3=$(waitfor r2 "LGTK recv\|LGTK set" 600); then echo "router 2 LGTK ${t3}s after it was switched on"; else fail "router 2 LGTK"; fi
fi
o=$(logsize br)
for i in $(seq 1 "$ROUNDS"); do
    [ -n "$GROUP" ] && { send br "udp $GROUP mc$i"; sleep 3; }
    send br "udp $LFN_GUA uc$i"
    sleep "$PERIOD"
done
sleep 20
# distinct answers: a repeated datagram or line counts once
mc=$(distinct br "udp rx from \[$LFN_GUA\][^ ]* \([0-9]+ bytes\): ack:mc[0-9]+$" "$o")
uc=$(distinct br "udp rx from \[$LFN_GUA\][^ ]* \([0-9]+ bytes\): ack:uc[0-9]+$" "$o")
if [ -n "$GROUP" ]; then
    echo "group $GROUP: $mc of $ROUNDS answered by the LFN"
    check "group datagrams answered ($mc of $ROUNDS, >= ${MIN_MC:-$ROUNDS})" [ "$mc" -ge "${MIN_MC:-$ROUNDS}" ]
fi
echo "unicast: $uc of $ROUNDS answered by the LFN"
check "unicast datagrams answered ($uc of $ROUNDS, >= ${MIN_UC:-$ROUNDS})" [ "$uc" -ge "${MIN_UC:-$ROUNDS}" ]
echo "LFN parent changes and checks:"
since_reset lfn | grep -a -h "LFN: better parent\|LFN: parent changed\|LFN: check:\|old parent .* released\|parent .* lost\|LFN: parent .*, PAN"
o=$(logsize br)
send br "nodes"; sleep 2
tail -c +$((o + 1)) br.log | grep -a "node fd12" | tail -4
for k in br r1 r2; do send $k "status -v"; done; sleep 3
for k in br r1 r2 lfn; do
    check "no fault on $k" [ "$(since_reset $k | grep -a -c FAULT)" -eq 0 ]
done
hw_exit
