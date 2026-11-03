#!/bin/bash
# Three kits: border router - router - LFN (README, "An LFN under a router
# on the kits"). Images: wisun_br (FW_LFN_PARENT), wisun_rn
# (FW_RN_LFN_PARENT), wisun_lfn (FW_TEST_MAC_ALLOW=<router EUI-64>).
# Cold boot: BR, router; when the router holds an LGTK, the LFN. Then BR ->
# LFN pings (one per 6 s) and UDP, N AEM windows on the LFN's kit, and with
# REJOIN=1 a router reset (REJOIN=2: border router and router) and the time
# until the LFN answers pings again.
# Every reset is checked (boot banner) and every search starts at the kit's
# last reset (tests/hw/hwlib.sh); pings are counted as distinct replies.
# Kit J-Link serial numbers: BR_SN, RN_SN, LFN_SN (the consoles are found
# from them); ENV: a script to source for commander (default ../../../.deps/env.sh).
# usage: run_hw_mesh.sh <workdir> [windows (0)] [window s (60)]
# Exit status 0 if every check passed.
set -u
W=$1; N=${2:-0}; WIN=${3:-60}
: "${BR_SN:?}" "${RN_SN:?}" "${LFN_SN:?}"
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
mkdir -p "$W"; cd "$W" || exit 2
source "$HW/hwlib.sh"
console_start br "$BR_SN"
console_start rn "$RN_SN"
console_start lfn "$LFN_SN"
sleep 1
kit_reset br || hw_exit
sleep 3
kit_reset rn || hw_exit
t=$(waitfor rn "if 1: ARM_NWK_BOOTSTRAP_READY" 600) || { fail "router join"; hw_exit; }
echo "router joined ${t}s after its reset"
if t2=$(waitfor rn "LGTK recv\|LGTK set" 300); then echo "router LGTK ${t2}s after its join"; else fail "router LGTK"; fi
kit_reset lfn || hw_exit
t=$(waitfor lfn "if 1: ARM_NWK_BOOTSTRAP_READY" 600) || { fail "LFN join"; hw_exit; }
echo "LFN joined ${t}s after its reset"
since_reset lfn | grep -a -h "LFN: LPA from\|LFN: parent \|rsl_in" | head -5
since_reset rn | grep -a -h "rsl_in" | tail -1
sleep 15   # the status lines, and the router's DAO for the LFN (LFN_STACK.md §9)
LFN_GUA=$(since_reset lfn | grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" | tail -1 | awk '{print $3}')
[ -n "$LFN_GUA" ] || { fail "no LFN address in its log"; hw_exit; }
o=$(logsize br)
send br "nodes"; sleep 2
tail -c +$((o + 1)) br.log | grep -a "node fd12" | tail -3
check "border router lists the LFN" bash -c "tail -c +$((o + 1)) br.log | grep -a -q 'node $LFN_GUA'"
o=$(logsize br)
send br "ping $LFN_GUA 10 32 6000"; sleep 80
send br "udp $LFN_GUA hello-from-br"; sleep 15
tail -c +$((o + 1)) br.log | grep -a -h "ping statistics\|udp rx" | tail -3
np=$(distinct br "bytes from $LFN_GUA: icmp_seq=[0-9]+" "$o")
check "BR -> LFN pings: $np of 10 distinct replies (>= 9)" [ "$np" -ge 9 ]
check "UDP BR -> LFN and its ack" [ "$(distinct br "udp rx from \[$LFN_GUA\].*ack:hello-from-br" "$o")" -ge 1 ]
sleep 10
for i in $(seq 1 "$N"); do
    ua=$(aem_ua "$LFN_SN" "$WIN")
    echo "window $i: ${ua:-none} uA"
    [ -n "$ua" ] || fail "AEM window $i"
done
if [ "${REJOIN:-0}" != 0 ]; then
    # REJOIN=1: reset the router only. REJOIN=2: reset the border router,
    # then the router (new keys; the LFN keeps running)
    o=$(logsize br)
    if [ "$REJOIN" = 2 ]; then
        kit_reset br; sleep 3; o=${HW_OFF[br]}
    fi
    echo "router reset at $(date +%s)"
    kit_reset rn
    # the LFN's traces are at ERROR by then: ping it from the BR
    t0=$(date +%s); back=0
    while :; do
        sleep 20
        send br "ping $LFN_GUA 1 32"
        sleep 14
        if [ "$(distinct br "bytes from $LFN_GUA: icmp_seq=[0-9]+" "$o")" -gt 0 ]; then
            echo "LFN answers again $(( $(date +%s) - t0 ))s after the router reset"; back=1; break; fi
        if [ $(( $(date +%s) - t0 )) -ge 1200 ]; then break; fi
    done
    check "LFN answers again after the router reset (REJOIN=$REJOIN)" [ "$back" = 1 ]
    if t2=$(waitfor rn "LGTK recv\|LGTK set" 5); then :; fi
    since_reset rn | grep -a -m1 "LGTK recv\|LGTK set" | sed 's/^/router: /'
    since_reset lfn | grep -a "LFN: parent .* lost\|ARM_NWK_BOOTSTRAP_READY" | tail -3
fi
send rn "status -v"; send br "status -v"; sleep 3
for k in br rn lfn; do
    check "no fault on $k" [ "$(since_reset $k | grep -a -c FAULT)" -eq 0 ]
done
hw_exit
