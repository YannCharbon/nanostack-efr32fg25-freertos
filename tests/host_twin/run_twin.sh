#!/bin/bash
# Host twin scenario: BR + RN processes on the fake medium, then the §5.10
# checks driven through the real CLI: RN joins (EAP-TLS, GTK, RPL parent,
# DHCPv6 GUA), ping and UDP both ways.
#
# usage: tests/host_twin/run_twin.sh <host build dir> [join timeout s (600)] [workdir] [settle s (60)]
# Prints PASS/FAIL per check; logs in <workdir>/{br,rn}.log.
set -u
B=$(cd "$1" && pwd)
TMO=${2:-600}
W=${3:-$(mktemp -d)}
SETTLE=${4:-60}
mkdir -p "$W"
cd "$W"                        # medium sockets are relative: short sun_path
rm -f n*.sock br.in rn.in
mkfifo br.in rn.in

HOST_TWIN_NODE=0 HOST_TWIN_NODES=2 HOST_TWIN_DIR=. "$B/twin_br" < br.in > br.log 2>&1 &
BR=$!
exec 3>br.in
sleep 1
HOST_TWIN_NODE=1 HOST_TWIN_NODES=2 HOST_TWIN_DIR=. "$B/twin_rn" < rn.in > rn.log 2>&1 &
RN=$!
exec 4>rn.in
trap 'kill $BR $RN 2>/dev/null' EXIT

t0=$(date +%s)
echo "workdir $W; waiting up to ${TMO}s for the RN to join..."
while ! grep -q "network event on if 1: ARM_NWK_BOOTSTRAP_READY" rn.log; do
    sleep 2
    if [ $(( $(date +%s) - t0 )) -ge "$TMO" ]; then
        echo "FAIL: RN did not reach ARM_NWK_BOOTSTRAP_READY in ${TMO}s"
        exit 1
    fi
    kill -0 $BR $RN 2>/dev/null || { echo "FAIL: a node process died"; tail -5 br.log rn.log; exit 1; }
done
JOIN=$(( $(date +%s) - t0 ))
echo "RN joined after ${JOIN}s; settling ${SETTLE}s (PAN advert/config trickle bursts on all 129 channels)"
sleep "$SETTLE"
echo "status -v" >&3; echo "status -v" >&4; sleep 2

BR_GUA=$(grep -o "ws0 addr fd12:3456:[0-9a-f:]*" br.log | tail -1 | awk '{print $3}')
RN_GUA=$(grep -o "ws0 addr fd12:3456:[0-9a-f:]*" rn.log | tail -1 | awk '{print $3}')
echo "BR GUA $BR_GUA, RN GUA $RN_GUA"

echo "ping $BR_GUA 10 32" >&4; sleep 16
echo "ping $RN_GUA 10 32" >&3; sleep 16
echo "udp $BR_GUA hello-from-rn" >&4; sleep 5
echo "udp $RN_GUA hello-from-br" >&3; sleep 5
echo "nodes" >&3; echo "heap" >&3; echo "heap" >&4; sleep 2

res=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; res=1; fi; }
check "authentication (RN)"             "grep -q 'EAP-TLS finished\|authentication success\|Authentication success' rn.log && ! grep -q 'handshake fatal' rn.log"
check "RPL parent = BR (RN status)"     "grep -q 'parent=fe80::5eef:fe00:1' rn.log"
check "DHCPv6 GUA in fd12:3456::/64 (RN)" "[ -n \"$RN_GUA\" ]"
check "BR lists the RN (nodes)"         "grep -q 'node fd12:3456::' br.log"
# Ping threshold: >= 5/10. Both nodes sweep asynchronous PAN advertisement /
# configuration frames over all 129 NA/1b channels (~2-3 s each at 50 kbit/s,
# several per minute while the trickle timers are short); unicast frames sent
# during a sweep are lost after the MAC retries. See README "Known gaps".
check "ping RN -> BR (>= 5/10)"         "grep -Eq '10 transmitted, ([5-9]|10) received' rn.log"
check "ping BR -> RN (>= 5/10)"         "grep -Eq '10 transmitted, ([5-9]|10) received' br.log"
check "UDP RN -> BR (+ ack back)"       "grep -q 'udp rx .*hello-from-rn' br.log && grep -q 'udp rx .*ack:hello-from-rn' rn.log"
check "UDP BR -> RN (+ ack back)"       "grep -q 'udp rx .*hello-from-br' rn.log && grep -q 'udp rx .*ack:hello-from-br' br.log"
check "no assert / crash"               "! grep -q 'ASSERT' br.log rn.log"
grep -h "ping statistics" rn.log br.log
echo "join time ${JOIN}s; logs: $W/br.log $W/rn.log"
exit $res
