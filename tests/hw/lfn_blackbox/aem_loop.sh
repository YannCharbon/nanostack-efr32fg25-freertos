#!/bin/bash
# aem_loop.sh <dir> <count> [duration_s]: back-to-back AEM dumps (10 kHz) of the LFN kit.
# silink must be closed on that kit (it blocks commander).
d=$1; n=$2; dur=${3:-60}; mkdir -p "$d"
for i in $(seq -w 1 "$n"); do
    echo "$(date +%s.%N) start $i" >> "$d/aem_loop.idx"
    "${DEPS:-$(dirname "$0")/../../../../.deps}/bin/commander" aem dump -s "${AEM_SN:-440235509}" --duration "$dur" -o "$d/aem_$i.csv" > /dev/null 2>&1
done
echo "$(date +%s.%N) done" >> "$d/aem_loop.idx"
