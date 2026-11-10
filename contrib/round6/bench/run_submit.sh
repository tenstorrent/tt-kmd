#!/bin/bash
# Submit-path experiment: one KMD variant over the comparison points.
# Usage: sudo OUT=dir ./run_submit.sh NAME GUARD_NS GUARD_MODE FAST KEEP_MS [extra bench args...]
set -u
cd "$(dirname "$0")"
NAME=$1 GUARD=$2 MODE=$3 FAST=$4 KEEP=$5; shift 5; EXTRA="$*"
OUT=${OUT:-results_submit}; mkdir -p $OUT
P=/sys/module/tenstorrent/parameters
echo 1 > $P/dma_ring; echo 1 > $P/dma_watermark; echo $GUARD > $P/dma_guard_ns; echo $MODE > $P/dma_guard_mode
echo $FAST > $P/dma_fast_submit; echo $KEEP > $P/dma_keep_ms
STATS="stops watermarks doorbells fresh full window_waits resumes"
snap() { for f in $STATS; do cat $P/dma_stat_$f; done | tr '\n' ' '; }
mark() { dmesg | tail -1 | awk '{ print substr($1, 2) + 0 }' > $OUT/.mark; }
check() { dmesg | grep -E 'DMA write timed out|IO_PAGE_FAULT|AER|tenstorrent.*(error|fail)' | awk -v s="$(cat $OUT/.mark)" '{ t = substr($1, 2) + 0; if (t > s) print }' | sed "s/^/[$NAME] /" | tee -a $OUT/dmesg_issues.log; mark; }
run() {
  echo "### $*" >> $OUT/$NAME.log
  local a=($(snap))
  if ! line=$(taskset -c 4 ./kmd_dma_bench --pf-depth 7 $EXTRA "$@" 2>> $OUT/$NAME.log); then echo "FAILED [$NAME]: $*" | tee -a $OUT/$NAME.log; check; exit 1; fi
  local b=($(snap)) i=0 extra="variant=$NAME"
  for f in $STATS; do extra+=" k_$f=$(( ${b[$i]} - ${a[$i]} ))"; i=$((i+1)); done
  echo "$line $extra" >> $OUT/$NAME.txt
}
mark
V32="--verify-chips 0,5,13,21,29,31"
for e in "16384 64" "4352 60"; do set -- $e; run --chips 5 --elem $1 --descs $2 --inflight 4 --secs 2 --warmup 0.5 --verify-chips 5; done
for e in "16384 64" "16384 16" "8192 128" "4352 60"; do set -- $e; run --chips 0-31 --elem $1 --descs $2 --inflight 4 --secs 3 --warmup 1 $V32; done
run --chips 0-31 --elem 16384 --descs 64 --open 12 --secs 3 --warmup 1 $V32
check
echo "=== $NAME done $(date +%T)"
