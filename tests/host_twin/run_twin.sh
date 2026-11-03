#!/bin/bash
# Host twin scenario: border router + one node on the fake medium, then the
# §5.10 checks driven through the real CLI.
#
#   ffn (default): twin_br + twin_rn. The RN joins (EAP-TLS, GTK, RPL
#                  parent, DHCPv6 GUA); ping and UDP both ways.
#   lfn:           twin_br_lfn (FW_LFN_PARENT) + twin_lfn (wisun_lfn): the
#                  LFN joins (EAP-TLS, LGTK, DHCPv6, address registered with
#                  the border router); pings from the border router, one
#                  every 6 s (each answer waits for the LFN's listen
#                  window), UDP both ways. Exercises the driver's LFN paths
#                  (windows opened and closed through the MAC, RX-active
#                  read, backoff with the radio off, scheduled TX from idle).
#
# usage: tests/host_twin/run_twin.sh <host build dir> [join timeout s (600)] [workdir] [settle s (60)] [ffn|lfn]
# Prints PASS/FAIL per check, exit status 0 if all passed; logs in
# <workdir>/{br,rn}.log (fresh at each run).
set -u
B=$(cd "$1" && pwd)
TMO=${2:-600}
W=${3:-$(mktemp -d)}
SETTLE=${4:-60}
MODE=${5:-ffn}
case $MODE in
ffn) BR_BIN=twin_br; RN_BIN=twin_rn; NODE=RN ;;
lfn) BR_BIN=twin_br_lfn; RN_BIN=twin_lfn; NODE=LFN ;;
*) echo "mode: ffn or lfn"; exit 2 ;;
esac
mkdir -p "$W"
cd "$W" || exit 2               # medium sockets are relative: short sun_path
rm -f n*.sock br.in rn.in br.log rn.log
mkfifo br.in rn.in

HOST_TWIN_NODE=0 HOST_TWIN_NODES=2 HOST_TWIN_DIR=. "$B/$BR_BIN" < br.in > br.log 2>&1 &
BR=$!
exec 3>br.in
sleep 1
HOST_TWIN_NODE=1 HOST_TWIN_NODES=2 HOST_TWIN_DIR=. "$B/$RN_BIN" < rn.in > rn.log 2>&1 &
RN=$!
exec 4>rn.in
trap 'kill $BR $RN 2>/dev/null' EXIT

t0=$(date +%s)
echo "mode $MODE, workdir $W; waiting up to ${TMO}s for the $NODE to join..."
while ! grep -q "network event on if 1: ARM_NWK_BOOTSTRAP_READY" rn.log; do
    sleep 2
    if [ $(( $(date +%s) - t0 )) -ge "$TMO" ]; then
        echo "FAIL: $NODE did not reach ARM_NWK_BOOTSTRAP_READY in ${TMO}s"
        exit 1
    fi
    kill -0 $BR $RN 2>/dev/null || { echo "FAIL: a node process died"; tail -5 br.log rn.log; exit 1; }
done
JOIN=$(( $(date +%s) - t0 ))
echo "$NODE joined after ${JOIN}s; settling ${SETTLE}s (PAN advert/config trickle bursts on all 129 channels)"
sleep "$SETTLE"
echo "status -v" >&3; echo "status -v" >&4; sleep 2

BR_GUA=$(grep -o "ws0 addr fd12:3456:[0-9a-f:]*" br.log | tail -1 | awk '{print $3}')
RN_GUA=$(grep -o "ws0 addr fd12:3456:[0-9a-f:]*" rn.log | tail -1 | awk '{print $3}')
echo "BR GUA $BR_GUA, $NODE GUA $RN_GUA"

if [ "$MODE" = ffn ]; then
    echo "ping $BR_GUA 10 32" >&4; sleep 16
    echo "ping $RN_GUA 10 32" >&3; sleep 16
    echo "udp $BR_GUA hello-from-rn" >&4; sleep 5
    echo "udp $RN_GUA hello-from-br" >&3; sleep 5
else
    # an LFN's answers wait for its next listen window (5 s by default);
    # its parent sends it at most one frame per window
    echo "ping $BR_GUA 10 32 6000" >&4; sleep 75
    echo "ping $RN_GUA 10 32 6000" >&3; sleep 75
    echo "udp $BR_GUA hello-from-rn" >&4; sleep 15
    echo "udp $RN_GUA hello-from-br" >&3; sleep 15
fi
echo "nodes" >&3; echo "heap" >&3; echo "heap" >&4; sleep 2

res=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; res=1; fi; }
# distinct echo replies (sequence numbers), so duplicates cannot hide a loss
replies() { grep -o "bytes from $2: icmp_seq=[0-9]*" "$1" | sort -u | wc -l; }
check "authentication ($NODE)"          "grep -q 'EAP-TLS finished\|authentication success\|Authentication success' rn.log && ! grep -q 'handshake fatal' rn.log"
if [ "$MODE" = ffn ]; then
    check "RPL parent = BR (RN status)" "grep -q 'parent=fe80::5eef:fe00:1' rn.log"
else
    check "LGTK installed (LFN)"        "grep -q 'LGTK set\|LGTK recv\|lgtk' rn.log"
fi
check "DHCPv6 GUA in fd12:3456::/64 ($NODE)" "[ -n \"$RN_GUA\" ]"
if [ "$MODE" = ffn ]; then
    check "BR lists the RN (nodes)"     "grep -q 'node fd12:3456::' br.log"
else
    # an LFN is not in the RPL routing table of its parent (no DAO for a
    # direct child); its DHCPv6 exchange with the border router is
    check "BR's DHCPv6 server answered the LFN" "grep -q 'dhcp service socket_sendto fe80::5eef:fe00:2' br.log"
fi
# Ping threshold: >= 5/10 distinct replies. Both nodes sweep asynchronous PAN
# advertisement / configuration frames over all 129 NA/1b channels (~2-3 s
# each at 50 kbit/s, several per minute while the trickle timers are short);
# unicast frames sent during a sweep are lost after the MAC retries. See
# README "Known gaps".
check "ping $NODE -> BR (>= 5/10)"      "[ \$(replies rn.log $BR_GUA) -ge 5 ]"
check "ping BR -> $NODE (>= 5/10)"      "[ \$(replies br.log $RN_GUA) -ge 5 ]"
check "UDP $NODE -> BR (+ ack back)"    "grep -q 'udp rx .*hello-from-rn' br.log && grep -q 'udp rx .*ack:hello-from-rn' rn.log"
check "UDP BR -> $NODE (+ ack back)"    "grep -q 'udp rx .*hello-from-br' rn.log && grep -q 'udp rx .*ack:hello-from-br' br.log"
check "no assert / crash"               "! grep -q 'ASSERT\|FAULT' br.log rn.log"
echo "ping replies: $NODE->BR $(replies rn.log "$BR_GUA")/10, BR->$NODE $(replies br.log "$RN_GUA")/10"
echo "join time ${JOIN}s; logs: $W/br.log $W/rn.log"
exit $res
