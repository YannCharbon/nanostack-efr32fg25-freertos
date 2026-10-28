#!/bin/bash
# session.sh start|stop <run dir> [rtt]
# Kit B (parent, embedded BR wisun_soc_brcli): 440240046, 115200 RTS/CTS.
# Kit A (LFN, wisun_soc_lfn_cli, the kit with a working AEM): 440235509, 9600 RTS/CTS.
# Consoles: <dir>/br.log, <dir>/lfn.log (host timestamps), commands via <dir>/br.fifo, <dir>/lfn.fifo.
# With "rtt": also the stack traces of both kits over RTT (<dir>/br_rtt.log, <dir>/lfn_rtt.log).
# RTT keeps a debugger attached, which changes the LFN's sleep current: never with AEM runs.
set -u
R=${R:-$(cd "$(dirname "$0")/../../../.." && pwd)}
T=$(dirname "$(readlink -f "$0")")
CON=$R/nanostack-efr32fg25-freertos/tests/hw/console.py
BR_SN=440240046; LFN_SN=440235509
tty_of() { readlink -f /dev/serial/by-id/*000$1-if00; }
cmd=$1; dir=$2; mkdir -p "$dir"
case $cmd in
start)
    CONSOLE_RTSCTS=1 CONSOLE_BAUD=115200 nohup python3 $CON "$(tty_of $BR_SN)" "$dir/br.log" "$dir/br.fifo" >/dev/null 2>&1 &
    echo $! > "$dir/pids"
    CONSOLE_RTSCTS=1 CONSOLE_BAUD=9600 nohup python3 $CON "$(tty_of $LFN_SN)" "$dir/lfn.log" "$dir/lfn.fifo" >/dev/null 2>&1 &
    echo $! >> "$dir/pids"
    if [ "${3:-}" = rtt ]; then
        nohup $T/rtt.sh $BR_SN "$dir/br_rtt.log" >/dev/null 2>&1 & echo $! >> "$dir/pids"
        nohup $T/rtt.sh $LFN_SN "$dir/lfn_rtt.log" >/dev/null 2>&1 & echo $! >> "$dir/pids"
    fi
    ;;
stop)
    for p in $(cat "$dir/pids"); do pkill -P "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
    pkill -f "commander rtt connect --noreset -s ($BR_SN|$LFN_SN)" 2>/dev/null
    rm -f "$dir/pids"
    ;;
esac
