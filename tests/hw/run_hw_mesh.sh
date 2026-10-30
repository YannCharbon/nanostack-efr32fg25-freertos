#!/bin/bash
# Three kits: border router - router - LFN (README, "An LFN under a router
# on the kits"). Images: wisun_br (FW_LFN_PARENT), wisun_rn
# (FW_RN_LFN_PARENT), wisun_lfn (FW_TEST_MAC_ALLOW=<router EUI-64>).
# Cold boot: BR, router; when the router holds an LGTK, the LFN. Then BR ->
# LFN pings (one per 6 s) and UDP, N AEM windows on the LFN's kit, and with
# REJOIN=1 a router reset (REJOIN=2: border router and router) and the time
# until the LFN answers pings again.
# Kit J-Link serial numbers: BR_SN, RN_SN, LFN_SN (the consoles are found
# from them); ENV: a script to source for commander (default ../../.deps/env.sh).
# usage: run_hw_mesh.sh <workdir> [windows (0)] [window s (60)]
set -u
W=$1; N=${2:-0}; WIN=${3:-60}
: "${BR_SN:?}" "${RN_SN:?}" "${LFN_SN:?}"
DEV=EFR32FG25B222F1920IM56
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
tty_of() { readlink -f /dev/serial/by-id/usb-Silicon_Labs_J-Link_Pro_OB_*$1-if00; }
mkdir -p "$W"; cd "$W"
"$HW/console.py" "$(tty_of $BR_SN)" br.log br.cmd & P1=$!
"$HW/console.py" "$(tty_of $RN_SN)" rn.log rn.cmd & P2=$!
"$HW/console.py" "$(tty_of $LFN_SN)" lfn.log lfn.cmd & P3=$!
trap 'kill $P1 $P2 $P3 2>/dev/null' EXIT
sleep 1
waitfor() { # file pattern timeout -> seconds waited, or fails
    local t0=$(date +%s)
    while ! grep -a -q "$2" "$1"; do
        sleep 1
        if [ $(( $(date +%s) - t0 )) -ge "$3" ]; then return 1; fi
    done
    echo $(( $(date +%s) - t0 ))
}
commander device reset --serialno $BR_SN -d $DEV >/dev/null 2>&1
sleep 3
commander device reset --serialno $RN_SN -d $DEV >/dev/null 2>&1
t=$(waitfor rn.log "if 1: ARM_NWK_BOOTSTRAP_READY" 600) || { echo "FAIL router join"; exit 1; }
echo "router joined ${t}s after its reset"
t2=$(waitfor rn.log "LGTK recv" 120) && echo "router LGTK ${t2}s after its join" || echo "FAIL router LGTK"
commander device reset --serialno $LFN_SN -d $DEV >/dev/null 2>&1
t=$(waitfor lfn.log "if 1: ARM_NWK_BOOTSTRAP_READY" 600) || { echo "FAIL LFN join"; exit 1; }
echo "LFN joined ${t}s after its reset"
grep -a -h "LFN: LPA from\|LFN: parent \|rsl_in" lfn.log | head -5
grep -a -h "rsl_in" rn.log | tail -1
sleep 15   # the status lines, and the router's DAO for the LFN (LFN_STACK.md §9)
LFN_GUA=$(grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" lfn.log | tail -1 | awk '{print $3}')
[ -n "$LFN_GUA" ] || { echo "FAIL no LFN address in its log"; exit 1; }
echo "nodes" > br.cmd; sleep 2
grep -a "node fd12" br.log | tail -3
echo "ping $LFN_GUA 10 32 6000" > br.cmd; sleep 80
echo "udp $LFN_GUA hello-from-br" > br.cmd; sleep 15
grep -a -h "ping statistics\|udp rx" br.log | tail -3
sleep 10
for i in $(seq 1 "$N"); do
    commander aem measure -s $LFN_SN --windowlength $((WIN * 1000)) > aem_$i.txt 2>&1
    echo "window $i: $(grep -a "Current" aem_$i.txt)"
done
if [ "${REJOIN:-0}" != 0 ]; then
    # REJOIN=1: reset the router only. REJOIN=2: reset the border router,
    # then the router (new keys; the LFN keeps running)
    c0=$(grep -a -c "bytes from $LFN_GUA" br.log)
    if [ "$REJOIN" = 2 ]; then
        commander device reset --serialno $BR_SN -d $DEV >/dev/null 2>&1; sleep 3
    fi
    echo "router reset at $(date +%s)"
    commander device reset --serialno $RN_SN -d $DEV >/dev/null 2>&1
    # the LFN's traces are at ERROR by then: ping it from the BR
    t0=$(date +%s)
    while :; do
        sleep 20
        echo "ping $LFN_GUA 1 32" > br.cmd
        sleep 14
        if [ $(grep -a -c "bytes from $LFN_GUA" br.log) -gt $c0 ]; then
            echo "LFN answers again $(( $(date +%s) - t0 ))s after the router reset"; break; fi
        if [ $(( $(date +%s) - t0 )) -ge 1200 ]; then echo "FAIL no answer 1200 s after router reset"; break; fi
    done
    grep -a "LFN: parent .* lost\|ARM_NWK_BOOTSTRAP_READY" lfn.log | tail -3
fi
echo "status -v" > rn.cmd; echo "status -v" > br.cmd; sleep 3
echo "faults: $(grep -a -c FAULT br.log rn.log lfn.log | tr '\n' ' ')"
