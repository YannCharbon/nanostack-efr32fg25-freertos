#!/bin/bash
# A router's LGTK after a cold start and after a reset while the network
# runs (keys and frame counters in flash, FW_FLASH_FS). Images: wisun_br
# (FW_LFN_PARENT) on BR_SN, wisun_rn (FW_RN_LFN_PARENT) on RN_SN.
#
# 1. Cold start: border router reset, the router 3 s later (both keep their
#    stored keys); 2. after SETTLE s of running network, the router alone is
#    reset. For each: the router's uptime at ARM_NWK_BOOTSTRAP_READY, at its
#    first key request (EAPOL-Key with LGTKL) and at its LGTK, from its own
#    log (ms since its reset), and the refused handshakes ("Incoming KMP
#    rejected").
# usage: run_hw_lgtk.sh <workdir>
# Environment: BR_SN (440240046), RN_SN (440235475), SETTLE (180), MAX_S
# (120: an LGTK later than this after a reset is a FAIL), ENV.
# Exit status 0 if every check passed.
set -u
W=$1
BR_SN=${BR_SN:-440240046}; RN_SN=${RN_SN:-440235475}; SETTLE=${SETTLE:-180}; MAX_S=${MAX_S:-120}
HW=$(cd "$(dirname "$0")" && pwd)
source "${ENV:-$HW/../../../.deps/env.sh}"
mkdir -p "$W"; cd "$W" || exit 2
source "$HW/hwlib.sh"
console_start br "$BR_SN"
console_start rn "$RN_SN"
sleep 1
ms_of() { since_reset rn | grep -a -m1 "$1" | sed -n 's/^[0-9.]* \[\([0-9]*\)\].*/\1/p'; }
report() { # case name
    local r k l
    r=$(ms_of "if 1: ARM_NWK_BOOTSTRAP_READY"); k=$(ms_of "Initial EAPOL-Key send.*LGTKL")
    l=$(ms_of "LGTK set\|LGTK recv")
    echo "$1: operational ${r:-none} ms, key request ${k:-none} ms, LGTK ${l:-none} ms (router uptime);" \
         "refused handshakes: $(since_reset rn | grep -a -c 'Incoming KMP rejected')"
    check "$1: LGTK within $MAX_S s of the router's reset" [ "${l:-999999999}" -le $(( MAX_S * 1000 )) ]
}
kit_reset br || hw_exit
sleep 3
kit_reset rn || hw_exit
waitfor rn "LGTK set\|LGTK recv" 900 >/dev/null
report "cold start"
sleep "$SETTLE"
kit_reset rn || hw_exit
waitfor rn "LGTK set\|LGTK recv" 900 >/dev/null
report "router reset"
for k in br rn; do
    check "no fault on $k" [ "$(since_reset $k | grep -a -c FAULT)" -eq 0 ]
done
hw_exit
