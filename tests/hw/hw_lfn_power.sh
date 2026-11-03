#!/bin/bash
# Idle current of our LFN(s) under our border router (GS8 stage 6; the EM2
# floor). Images flashed beforehand: wisun_br (FW_LFN_PARENT) on BR_SN,
# wisun_lfn on each kit of LFN_SNS (each with a working AEM).
#
# Cold boot of the border router, then each LFN (boot banner checked); once
# every LFN has joined: one ping and one UDP datagram from the border router
# to each LFN, answered (distinct replies counted; LISTEN_S, the LFNs'
# listen interval, sets the pace: the parent sends an LFN one frame per
# interval); after the LFNs' traces go
# quiet (APP_LFN_QUIET_AFTER_S), N AEM windows of WIN s on each LFN kit, in
# turn, and DUMP_S s of 10 kHz samples per kit (aem_<SN>_<i>.csv, DUMPS
# files), for tests/hw/aem_floor.py.
#
# usage: hw_lfn_power.sh <workdir> [windows (5)] [window s (60)]
# Environment: BR_SN (440240046), LFN_SNS (space-separated, "440235509"),
# DUMPS (1), DUMP_S (60), JOIN_TMO (600), LISTEN_S (5), ENV (commander environment,
# default ../../../.deps/env.sh). Exit status 0 if every check passed.
set -u
W=$1; N=${2:-5}; WIN=${3:-60}
BR_SN=${BR_SN:-440240046}; LFN_SNS=${LFN_SNS:-440235509}
DUMPS=${DUMPS:-1}; DUMP_S=${DUMP_S:-60}; JOIN_TMO=${JOIN_TMO:-600}
LISTEN_S=${LISTEN_S:-5}
# 3 requests one listen interval + 5 s apart: each reply waits for the LFN's
# next window, and a reply must come while the series is open (12 s after
# its last request, CLI ping)
PGAP=$(( LISTEN_S + 5 ))
# with long intervals a reply can miss the first window (seen: 105 s at
# 60 s): at least 2 of 3 answered up to 10 s, 1 of 3 above
PMIN=2; [ "$LISTEN_S" -gt 10 ] && PMIN=1
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
mkdir -p "$W"; cd "$W" || exit 2
source "$HW/hwlib.sh"
console_start br "$BR_SN"
lfns=()
for sn in $LFN_SNS; do console_start "lfn$sn" "$sn"; lfns+=("lfn$sn"); done
sleep 1
kit_reset br || hw_exit
waitfor br "BR: interfaces up" 30 >/dev/null || fail "border router not up"
for l in "${lfns[@]}"; do kit_reset "$l"; done
declare -A GUA
for l in "${lfns[@]}"; do
    if t=$(waitfor "$l" "if 1: ARM_NWK_BOOTSTRAP_READY" "$JOIN_TMO"); then
        echo "$l joined, ${t} s after the start of the wait"
        sleep 2
        GUA[$l]=$(since_reset "$l" | grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" | tail -1 | awk '{print $3}')
        [ -n "${GUA[$l]}" ] || fail "$l: no address in its log"
    else
        fail "$l did not join in ${JOIN_TMO} s"
    fi
done
sleep 10
for l in "${lfns[@]}"; do
    a=${GUA[$l]:-}; [ -n "$a" ] || continue
    o=$(logsize br)
    send br "ping $a 3 32 $(( PGAP * 1000 ))"; sleep $(( 3 * PGAP + 15 ))
    send br "udp $a hello-$l"; sleep $(( 2 * LISTEN_S + 15 ))
    np=$(distinct br "bytes from $a: icmp_seq=[0-9]+" "$o")
    nu=$(distinct br "udp rx from \[$a\].*ack:hello-$l" "$o")
    check "$l answers pings ($np of 3 distinct replies, >= $PMIN)" [ "$np" -ge "$PMIN" ]
    check "$l answers UDP" [ "$nu" -ge 1 ]
done
sleep 35   # the LFNs' traces go quiet 30 s after the join
for sn in $LFN_SNS; do
    for i in $(seq 1 "$N"); do
        ua=$(aem_ua "$sn" "$WIN")
        echo "kit $sn window $i: ${ua:-none} uA"
        [ -n "$ua" ] || fail "kit $sn: no AEM reading"
    done
    for i in $(seq 1 "$DUMPS"); do
        commander aem dump -s "$sn" --duration "$DUMP_S" -o "aem_${sn}_$i.csv" >/dev/null 2>&1 ||
            fail "kit $sn: AEM dump $i"
    done
done
for l in "${lfns[@]}"; do
    a=${GUA[$l]:-}; [ -n "$a" ] || continue
    o=$(logsize br)
    send br "ping $a 2 32 $(( PGAP * 1000 ))"; sleep $(( 2 * PGAP + 15 ))
    check "$l still answers after the windows" [ "$(distinct br "bytes from $a: icmp_seq=[0-9]+" "$o")" -ge 1 ]
done
send br "status -v"; sleep 3
for k in br "${lfns[@]}"; do
    check "no fault on $k" [ "$(since_reset "$k" | grep -a -c FAULT)" -eq 0 ]
done
hw_exit
