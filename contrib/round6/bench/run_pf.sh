#!/bin/bash
# Prefetch-only experiment: sweeps QOS.PF_DEPTH (= ELEM_PF with the KMD cap raised to 15).
set -u
cd "$(dirname "$0")"
OUT=results_pf; mkdir -p $OUT
mark() { dmesg | tail -1 | awk '{ print substr($1, 2) + 0 }' > $OUT/.mark; }
check() { dmesg | grep -E 'DMA write timed out|IO_PAGE_FAULT|AER|tenstorrent.*(error|fail)' | awk -v s="$(cat $OUT/.mark)" '{ t = substr($1, 2) + 0; if (t > s) print }' | sed "s/^/[$1] /" | tee -a $OUT/dmesg_issues.log; mark; }
run() { local f=$1; shift; echo "### $*" >> $OUT/$f.log
  if ! taskset -c 4 ./kmd_dma_bench "$@" >> $OUT/$f.txt 2>> $OUT/$f.log; then echo "FAILED: $*" | tee -a $OUT/$f.log; check $f; exit 1; fi; }
mark
echo "=== single $(date +%T)"
for c in 5 0; do for e in 4096 8192 16384 65536; do for pf in ${PFS:-1 3 7 11 15}; do
  run single --chips $c --elem $e --descs 64 --inflight 4 --pf-depth $pf --secs 2 --warmup 0.5 --verify-chips $c
  run single --chips $c --elem $e --descs 1024 --inflight 2 --pf-depth $pf --secs 2 --warmup 0.5 --verify-chips $c
done; done; done
check single
echo "=== 32 chips $(date +%T)"
for pf in ${PFS:-1 3 7 11 15}; do for k in 2 4; do
  run c32 --chips 0-31 --elem 8192 --descs 128 --inflight $k --pf-depth $pf --secs 3 --warmup 1 --verify-chips 0,5,13,21,29,31
  run c32 --chips 0-31 --elem 16384 --descs 64 --inflight $k --pf-depth $pf --secs 3 --warmup 1 --verify-chips 0,5,13,21,29,31
done; done
check c32
echo "=== done $(date +%T)"
