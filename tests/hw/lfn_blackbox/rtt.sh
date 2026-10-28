#!/bin/bash
# rtt.sh <serial> <logfile> [blockaddress]: log a kit's RTT terminal 0 (Silicon Labs stack traces),
# host-timestamped. Retries the connection (the LFN may be asleep, or a reset may hold the adapter).
# Block addresses (_SEGGER_RTT): wisun_soc_brcli 0x200070c4, wisun_soc_lfn_cli 0x20008488, wisun_rcp 0x200066d4.
D=$(dirname "$0")
case "$1" in
    440240046) B=${3:-0x200070c4} ;;
    440235509) B=${3:-0x20008488} ;;
    *) B=$3 ;;
esac
while true; do
    "${DEPS:-$(dirname "$0")/../../../../.deps}/bin/commander" rtt connect --noreset -s "$1" -d EFR32FG25B222F1920IM56 \
        --blockaddress "$B" 2>&1 | python3 -u "$D/ts.py" "$2"
    sleep 1
done
