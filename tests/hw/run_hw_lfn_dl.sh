#!/bin/bash
# Downlinks to our LFN(s) after silences: delivery and latency, optionally a
# router restart in the middle, optionally AEM windows during the silences
# (long idle runs).
#
# Kits: border router BR_SN (wisun_br, FW_LFN_PARENT), optional router RN_SN
# (wisun_rn, FW_RN_LFN_PARENT), LFNs LFN_SNS (wisun_lfn; FW_TEST_MAC_ALLOW
# to force the router as parent). Images flashed beforehand.
#
# Cold boot: border router, router (once it holds an LGTK), each LFN; all
# resets checked by the boot banner. Then for each gap g of GAPS: one UDP
# datagram "dl<i>" from the border router to each LFN, then g s of silence.
# Each LFN answers "ack:dl<i>"; the latency is the border router's console
# time of the ack minus that of the command. With RN_RESET_BEFORE=k, the
# router is reset (keys and frame counters from flash) just before
# datagram k (sent once the router holds an LGTK; the following datagrams
# keep their gaps); the script reports its LGTK, its rejoin and the first
# datagram answered after the reset. MIN_FRAC then allows the datagrams
# sent while the router re-joins. With AEM_SN and
# AEM_EVERY (s), an AEM window of AEM_WIN s on each kit of AEM_SN (one or
# more, measured at the same time) every AEM_EVERY s during the silences
# (aem.txt: <unix time> <kit> <uA>). With AEM_DUMP_EVERY (s), an AEM dump of
# AEM_DUMP_S s on each kit of AEM_DUMP_SN (default AEM_SN) that often during
# the silences, analysed at once (hwlib aem_dump: aem_<kit>_<i>.json,
# .floor.txt, .wakes.txt). HW_DRY=1 prints the sequence without hardware.
#
# usage: run_hw_lfn_dl.sh <workdir>
# Environment: BR_SN, RN_SN (empty: no router), LFN_SNS (space-separated),
# GAPS ("70 70 70 70 70"), LISTEN_S (5: the LFNs' listen interval, sets the
# wait after the last datagram, 2 intervals + 20 s), RN_RESET_BEFORE (0),
# AEM_SN (kits, space-separated), AEM_EVERY (0: none), AEM_WIN (60),
# AEM_DUMP_SN, AEM_DUMP_EVERY (0: none), AEM_DUMP_S (240), JOIN_TMO (900:
# the LFNs' join, s; Balanced and Eco need longer), MIN_FRAC (100: percent of
# the datagrams each LFN must answer), ENV.
# Prints per LFN: answered, latencies, and its parent (border router's
# 'nodes'); latencies.txt has one line per datagram:
#   <lfn> <i> <silence before, s> <sent unix time> <latency s | none>
# Exit status 0 if every check passed.
set -u
W=$1
: "${BR_SN:?}" "${LFN_SNS:?}"
RN_SN=${RN_SN:-}; GAPS=${GAPS:-70 70 70 70 70}; LISTEN_S=${LISTEN_S:-5}
RN_RESET_BEFORE=${RN_RESET_BEFORE:-0}; AEM_SN=${AEM_SN:-}; AEM_EVERY=${AEM_EVERY:-0}
AEM_WIN=${AEM_WIN:-60}; MIN_FRAC=${MIN_FRAC:-100}; JOIN_TMO=${JOIN_TMO:-900}
AEM_DUMP_SN=${AEM_DUMP_SN:-$AEM_SN}; AEM_DUMP_EVERY=${AEM_DUMP_EVERY:-0}; AEM_DUMP_S=${AEM_DUMP_S:-240}
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
mkdir -p "$W"; cd "$W" || exit 2
source "$HW/hwlib.sh"
console_start br "$BR_SN"
[ -n "$RN_SN" ] && console_start rn "$RN_SN"
lfns=()
for sn in $LFN_SNS; do console_start "lfn$sn" "$sn"; lfns+=("lfn$sn"); done
hw_sleep 1
# the router's uptime (ms, the console's second field) at its LGTK
lgtk_ms() { since_reset rn | grep -a -m1 "LGTK set\|LGTK recv" | sed -n 's/^[0-9.]* \[\([0-9]*\)\].*/\1/p'; }
ready_ms() { since_reset rn | grep -a -m1 "if 1: ARM_NWK_BOOTSTRAP_READY" | sed -n 's/^[0-9.]* \[\([0-9]*\)\].*/\1/p'; }
kit_reset br || hw_exit
waitfor br "interfaces up" 30 >/dev/null || fail "border router not up"
if [ -n "$RN_SN" ]; then
    hw_sleep 3
    kit_reset rn || hw_exit
    waitfor rn "if 1: ARM_NWK_BOOTSTRAP_READY" 900 >/dev/null || { fail "router join"; hw_exit; }
    if waitfor rn "LGTK set\|LGTK recv" 600 >/dev/null; then
        echo "router: operational at $(ready_ms) ms, LGTK at $(lgtk_ms) ms after its reset (its uptime)"
    else
        fail "router LGTK within 600 s of its join"
    fi
fi
for l in "${lfns[@]}"; do kit_reset "$l"; done
declare -A GUA
for l in "${lfns[@]}"; do
    if t=$(waitfor "$l" "if 1: ARM_NWK_BOOTSTRAP_READY" "$JOIN_TMO"); then
        hw_sleep 2
        GUA[$l]=$(since_reset "$l" | grep -a -o "ws0 addr fd12:3456:[0-9a-f:]*" | tail -1 | awk '{print $3}')
        dry && GUA[$l]="fd12:3456::<$l>"
        p=$(since_reset "$l" | grep -a -o "LFN: parent chosen: [0-9a-f:]*" | tail -1 | awk '{print $4}')
        echo "$l joined, ${GUA[$l]:-no address}, parent $p, ${t} s after the start of the wait"
        [ -n "${GUA[$l]}" ] || fail "$l: no address in its log"
    else
        fail "$l did not join in $JOIN_TMO s"
    fi
done
hw_sleep 40   # the LFNs' traces go quiet 30 s after the join; the router's DAO
o0=$(logsize br)
send br "nodes"; hw_sleep 2
tail -c +$((o0 + 1)) br.log 2>/dev/null | grep -a "node fd12" | sort -u -k2 | sed 's/^[0-9.]* *//'
: > latencies.txt
i=0; prev=0
next_aem=$(( $(hw_now) + AEM_EVERY ))
next_dump=$(( $(hw_now) + AEM_DUMP_EVERY )); ndump=0
# AEM windows on every kit of AEM_SN at the same time
aem_windows() {
    local sn pids=""
    # wait for these only: a bare wait would also wait for the consoles
    for sn in $AEM_SN; do
        ( ua=$(aem_ua "$sn" "$AEM_WIN"); echo "$(hw_now) $sn ${ua:-none}" >> aem.txt ) & pids="$pids $!"
    done
    wait $pids
}
aem_dumps() {
    local sn pids=""
    ndump=$((ndump + 1))
    for sn in $AEM_DUMP_SN; do aem_dump "$sn" "$AEM_DUMP_S" "aem_${sn}_$ndump" & pids="$pids $!"; done
    wait $pids
}
for g in $GAPS; do
    i=$((i + 1))
    if [ "$i" = "$RN_RESET_BEFORE" ] && [ -n "$RN_SN" ]; then
        echo "router reset before datagram $i, at $(hw_now)"
        T_RESET=$(hw_now)
        kit_reset rn
        if waitfor rn "LGTK set\|LGTK recv" 900 >/dev/null; then
            echo "router after its reset: LGTK at $(lgtk_ms) ms (its uptime); datagrams go on meanwhile"
        else
            fail "router LGTK within 900 s of its reset"
        fi
    fi
    for l in "${lfns[@]}"; do
        a=${GUA[$l]:-}; [ -n "$a" ] || continue
        send br "udp $a dl$i"
        if dry; then echo "$l $i $prev $(hw_now)" >> sent.txt; else echo "$l $i $prev $(date +%s.%N | cut -c1-14)" >> sent.txt; fi
        hw_sleep 1
    done
    end=$(( $(hw_now) + g ))
    while [ "$(hw_now)" -lt "$end" ]; do
        if [ -n "$AEM_DUMP_SN" ] && [ "$AEM_DUMP_EVERY" -gt 0 ] && [ "$(hw_now)" -ge "$next_dump" ] &&
           [ $(( end - $(hw_now) )) -gt $(( AEM_DUMP_S + 10 )) ]; then
            aem_dumps
            dry && hw_sleep "$AEM_DUMP_S"
            next_dump=$(( $(hw_now) + AEM_DUMP_EVERY ))
        elif [ -n "$AEM_SN" ] && [ "$AEM_EVERY" -gt 0 ] && [ "$(hw_now)" -ge "$next_aem" ] &&
           [ $(( end - $(hw_now) )) -gt $(( AEM_WIN + 5 )) ]; then
            aem_windows
            dry && hw_sleep "$AEM_WIN"
            next_aem=$(( $(hw_now) + AEM_EVERY ))
        else
            if dry; then
                # to the next event: an AEM window or dump, or the end of the silence
                nx=$end
                [ -n "$AEM_SN" ] && [ "$AEM_EVERY" -gt 0 ] && [ "$next_aem" -gt "$(hw_now)" ] && [ "$next_aem" -lt "$nx" ] && nx=$next_aem
                [ -n "$AEM_DUMP_SN" ] && [ "$AEM_DUMP_EVERY" -gt 0 ] && [ "$next_dump" -gt "$(hw_now)" ] && [ "$next_dump" -lt "$nx" ] && nx=$next_dump
                hw_sleep $(( nx > $(hw_now) ? nx - $(hw_now) : 1 ))
            else
                sleep 5
            fi
        fi
    done
    prev=$g
done
hw_sleep $(( 2 * LISTEN_S + 20 ))
# latency of each datagram: first ack (distinct by text) after the command
while read -r l n silence t; do
    a=${GUA[$l]}
    ta=$(tail -c +$((o0 + 1)) br.log 2>/dev/null | grep -a -m1 "udp rx from \[$a\][^ ]* ([0-9]* bytes): ack:dl$n\$" | awk '{print $1}')
    if [ -n "$ta" ]; then lat=$(echo "$ta - $t" | bc); else lat=none; fi
    echo "$l $n $silence $t $lat" >> latencies.txt
done < sent.txt
rm -f sent.txt
for l in "${lfns[@]}"; do
    a=${GUA[$l]:-}; [ -n "$a" ] || continue
    n=$(grep -c "^$l " latencies.txt)
    ok=$(grep "^$l " latencies.txt | awk '$5 != "none"' | wc -l)
    dup=$(( $(tail -c +$((o0 + 1)) br.log 2>/dev/null | grep -a -c "udp rx from \[$a\].*ack:dl") - ok ))
    lats=$(grep "^$l " latencies.txt | awk '{printf "%s ", ($5 == "none" ? "none" : sprintf("%.1f", $5))}')
    echo "$l: $ok of $n answered (duplicates $dup); latencies s: $lats"
    check "$l answers >= $MIN_FRAC % of the datagrams" [ $(( ok * 100 )) -ge $(( n * MIN_FRAC )) ]
done
if [ -n "${T_RESET:-}" ]; then
    # the router's own milestones after its reset, and the first datagram
    # answered after it (traffic resumed)
    echo "router after its reset: operational at $(ready_ms) ms (its uptime)"
    first=$(awk -v t="$T_RESET" '$4 > t && $5 != "none" {printf "datagram %d sent %.0f s after the reset, answered %.0f s after it", $2, $4 - t, $4 + $5 - t; exit}' latencies.txt)
    lost=$(awk -v t="$T_RESET" '$4 > t && $5 == "none"' latencies.txt | wc -l)
    echo "after the router reset: ${first:-no datagram answered}; $lost datagram(s) unanswered"
fi
if [ -s aem.txt ]; then
    for sn in $AEM_SN; do
        echo "AEM windows kit $sn (uA): $(awk -v k="$sn" '$2 == k {printf "%s ", $3}' aem.txt)"
    done
fi
for f in aem_*.wakes.txt; do [ -e "$f" ] && echo "AEM dump $(cat "$f")"; done
o=$(logsize br); send br "nodes"; hw_sleep 2
tail -c +$((o + 1)) br.log 2>/dev/null | grep -a "node fd12" | sed 's/^[0-9.]* *//'
for k in br ${RN_SN:+rn} "${lfns[@]}"; do
    check "no fault on $k" [ "$(since_reset "$k" | grep -a -c FAULT)" -eq 0 ]
done
hw_exit
