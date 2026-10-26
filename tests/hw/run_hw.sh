#!/bin/bash
# Two-board hardware test (README procedure, steps 2-7), one cold boot.
# usage: tests/hw/run_hw.sh <workdir> [join timeout s (600)] [settle s (60)]
# Env: BR_SN, BR_TTY, RN_SN, RN_TTY (defaults: the bench kits), COMMANDER.
set -u
W=$1; TMO=${2:-600}; SETTLE=${3:-60}
BR_SN=${BR_SN:-440240046}; BR_TTY=${BR_TTY:-/dev/ttyACM1}
RN_SN=${RN_SN:-440235509}; RN_TTY=${RN_TTY:-/dev/ttyACM0}
COMMANDER=${COMMANDER:-commander}
DEV=EFR32FG25B222F1920IM56
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W"; cd "$W"; rm -f br.log rn.log br.cmd rn.cmd
"$HERE/console.py" "$BR_TTY" br.log br.cmd & BRL=$!
"$HERE/console.py" "$RN_TTY" rn.log rn.cmd & RNL=$!
trap 'kill $BRL $RNL 2>/dev/null' EXIT
sleep 1
# Cold-ish boot: hold the RN in reset (mass erase not needed: no NVM use),
# reset BR, then RN.
$COMMANDER device reset --serialno $BR_SN -d $DEV >/dev/null 2>&1
sleep 3
$COMMANDER device reset --serialno $RN_SN -d $DEV >/dev/null 2>&1
t0=$(date +%s)
echo "waiting up to ${TMO}s for the RN to join..."
while ! grep -a -q "network event on if 1: ARM_NWK_BOOTSTRAP_READY" rn.log; do
    sleep 2
    if [ $(( $(date +%s) - t0 )) -ge "$TMO" ]; then echo "FAIL: RN did not join in ${TMO}s"; exit 1; fi
done
JOIN=$(( $(date +%s) - t0 ))
echo "RN joined after ${JOIN}s (host clock, from RN reset); settling ${SETTLE}s"
# Steady-state timing: reset the radio/port statistics after the join (EAP-TLS
# ECC blocks the stack task for up to ~0.9 s during authentication), and log
# the FHSS drift-compensation trace (DEBUG level, fhss group) on the RN.
echo "radio clear" > br.cmd; echo "radio clear" > rn.cmd
echo "trace 5 fhss" > rn.cmd
sleep "$SETTLE"
echo "trace 4" > rn.cmd
echo "status -v" > br.cmd; echo "status -v" > rn.cmd; sleep 2
BR_GUA=$(grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" br.log | tail -1 | awk '{print $3}')
RN_GUA=$(grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" rn.log | tail -1 | awk '{print $3}')
echo "BR GUA $BR_GUA, RN GUA $RN_GUA"
echo "ping $BR_GUA 10 32" > rn.cmd; sleep 16
echo "ping $RN_GUA 10 32" > br.cmd; sleep 16
echo "udp $BR_GUA hello-from-rn" > rn.cmd; sleep 6
echo "udp $RN_GUA hello-from-br" > br.cmd; sleep 6
echo "nodes" > br.cmd; echo "heap" > br.cmd; echo "heap" > rn.cmd
echo "status -v" > br.cmd; echo "status -v" > rn.cmd; echo "tasks" > rn.cmd; echo "tasks" > br.cmd; sleep 3
res=0
check() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; res=1; fi; }
check "authentication (RN)"   "grep -a -q 'EAP-TLS finished' rn.log && ! grep -a -q 'handshake fatal' rn.log"
check "RPL parent = BR"       "grep -a -q 'parent=fe80::' rn.log"
check "DHCPv6 GUA (RN)"       "[ -n \"$RN_GUA\" ]"
check "BR lists the RN"       "grep -a -q 'node fd12:3456::' br.log"
check "ping RN -> BR"         "grep -a -Eq '10 transmitted, [1-9][0-9]* received' rn.log"
check "ping BR -> RN"         "grep -a -Eq '10 transmitted, [1-9][0-9]* received' br.log"
check "UDP RN -> BR (+ack)"   "grep -a -q 'udp rx .*hello-from-rn' br.log && grep -a -q 'udp rx .*ack:hello-from-rn' rn.log"
check "UDP BR -> RN (+ack)"   "grep -a -q 'udp rx .*hello-from-br' rn.log && grep -a -q 'udp rx .*ack:hello-from-br' br.log"
grep -a -h "ping statistics" rn.log br.log
echo "join ${JOIN}s"
exit $res
