#!/bin/bash
# Stress test: IPMI reset all chips, wait for recovery, verify all 32 are
# at Gen5 and pass NOC + DMA sanity checks. Repeat.
#
# Usage: ./stress.sh [iterations]
#   Default: 100

set -euo pipefail

ITERATIONS="${1:-100}"
TOOLDIR="$(dirname "$0")"
DISCOVER="$TOOLDIR/../tools/discover"
SANITY="$TOOLDIR/../tools/sanity"
EXPECTED=32
SETTLE=15

for tool in "$DISCOVER" "$SANITY"; do
    if [ ! -x "$tool" ]; then
        echo "Error: $tool not found. Build it first."
        exit 1
    fi
done

pass=0
fail=0

echo "Stress test: $ITERATIONS iterations, $EXPECTED devices"
echo ""

for i in $(seq 1 "$ITERATIONS"); do
    sudo ipmitool raw 0x30 0x8B 0xF 0xFF 0x0 0xFF 2>/dev/null
    sleep "$SETTLE"

    # Check link speeds
    degraded=$("$DISCOVER" 2>/dev/null | grep -c 'Gen1/Gen5' || true)
    if [ "$degraded" -gt 0 ]; then
        fail=$((fail + 1))
        printf "Iteration %3d/%d: FAIL  (pass=%d fail=%d)  [%d chips at Gen1]\n" \
            "$i" "$ITERATIONS" "$pass" "$fail" "$degraded"
        "$DISCOVER" 2>/dev/null | grep 'Gen1/Gen5' | while read -r line; do
            printf "    %s\n" "$line"
        done
        continue
    fi

    # All at Gen5 -- run NOC + DMA sanity on all devices
    if ! "$SANITY" -n "$EXPECTED" 2>&1; then
        fail=$((fail + 1))
        printf "Iteration %3d/%d: FAIL  (pass=%d fail=%d)  [sanity check failed]\n" \
            "$i" "$ITERATIONS" "$pass" "$fail"
        continue
    fi

    pass=$((pass + 1))
    printf "Iteration %3d/%d: PASS  (pass=%d fail=%d)\n" \
        "$i" "$ITERATIONS" "$pass" "$fail"
done

echo ""
echo "--- Results: $pass pass, $fail fail out of $ITERATIONS ---"
exit $((fail > 0 ? 1 : 0))
