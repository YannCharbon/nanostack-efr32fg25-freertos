#!/bin/bash
# HW9 campaign: usage campaign.sh <rounds> <boots per block> <variant>...
# Each round flashes each variant in turn and runs <boots per block> boots of it.
set -u
S=/tmp/claude-1000/-home-yann-mesh-emulator-rework/d4ddb9d0-f90a-45e0-b53f-a1c82161aa85/scratchpad/hw9
FW=/home/yann/mesh-emulator-rework/nanostack-efr32fg25-freertos
export PATH=/home/yann/mesh-emulator-rework/.deps/bin:$PATH
DEV="-d EFR32FG25B222F1920IM56"
BR_SN=440240046; BR_TTY=/dev/ttyACM1; RN_SN=440235509; RN_TTY=/dev/ttyACM0
TMO=${TMO:-420}
ROUNDS=$1; BLK=$2; shift 2
for r in $(seq 1 $ROUNDS); do
  for v in "$@"; do
    img=$S/build-$v
    { commander device masserase --serialno $BR_SN $DEV && commander flash $img/wisun_br.hex --serialno $BR_SN $DEV &&
      commander device masserase --serialno $RN_SN $DEV && commander flash $img/wisun_rn.hex --serialno $RN_SN $DEV; } \
      > $S/flash_last.log 2>&1 || { echo "flash failed $v"; continue; }
    for b in $(seq 1 $BLK); do
      n=$(ls -d $S/runs/$v-* 2>/dev/null | wc -l); n=$((n+1))
      d=$S/runs/$v-$(printf %02d $n); mkdir -p $d; cd $d
      $FW/tests/hw/console.py $BR_TTY br.log br.cmd & BRL=$!
      $FW/tests/hw/console.py $RN_TTY rn.log rn.cmd & RNL=$!
      sleep 1
      commander device reset --serialno $BR_SN $DEV >/dev/null 2>&1
      sleep 3
      commander device reset --serialno $RN_SN $DEV >/dev/null 2>&1
      t0=$(date +%s); res=stall
      while ! grep -a -q "network event on if 1: ARM_NWK_BOOTSTRAP_READY" rn.log; do
        sleep 2
        if [ $(( $(date +%s) - t0 )) -ge "$TMO" ]; then break; fi
      done
      grep -a -q "network event on if 1: ARM_NWK_BOOTSTRAP_READY" rn.log && res=join
      sleep 3
      echo "status -v" > br.cmd; echo "status -v" > rn.cmd; sleep 3
      kill $BRL $RNL 2>/dev/null; wait $BRL $RNL 2>/dev/null
      echo "$v $n $res $(( $(date +%s) - t0 ))s $(date +%T)" | tee -a $S/campaign.log
    done
  done
done
