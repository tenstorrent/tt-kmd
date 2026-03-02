#!/bin/bash
# Stress test for PCIe link retrain workaround.
#
# Repeatedly IPMI-resets the chips (triggering hotplug re-enumeration),
# waits for devices to come back, then checks that all devices reached
# Gen5. Prints any BDFs that didn't make it.
#
# Usage: ./stress-retrain.sh [iterations]
#   Default: 100 iterations
#
# Requires: ipmitool, discover tool (build with: gcc -o discover discover.c)

set -euo pipefail

ITERATIONS="${1:-100}"
SETTLE_TIME=5
DISCOVER="$(dirname "$0")/discover"

if [ ! -x "$DISCOVER" ]; then
    echo "Error: $DISCOVER not found or not executable."
    echo "Build it: gcc -o tools/discover tools/discover.c"
    exit 1
fi

pass=0
fail=0

echo "Stress testing link retrain, $ITERATIONS iterations."
echo ""

for i in $(seq 1 "$ITERATIONS"); do
    sudo ipmitool raw 0x30 0x8B 0xF 0xFF 0x0 0xFF 2>/dev/null
    sleep "$SETTLE_TIME"

    output=$("$DISCOVER" 2>/dev/null | sort)
    degraded=$(echo "$output" | grep -v 'Gen5/Gen5' || true)

    if [ -z "$degraded" ]; then
        pass=$((pass + 1))
        printf "Iteration %3d/%d: PASS  (pass=%d fail=%d)\n" \
            "$i" "$ITERATIONS" "$pass" "$fail"
    else
        fail=$((fail + 1))
        printf "Iteration %3d/%d: FAIL  (pass=%d fail=%d)\n" \
            "$i" "$ITERATIONS" "$pass" "$fail"
        echo "$degraded" | while read -r line; do
            printf "    %s\n" "$line"
        done
    fi
done

echo ""
echo "=== Summary ==="
echo "Iterations: $ITERATIONS"
echo "Pass: $pass"
echo "Fail: $fail"

[ "$fail" -eq 0 ] || exit 1
