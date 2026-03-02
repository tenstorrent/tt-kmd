#!/bin/bash
# Degrade x1 links to Gen1 via repeated IPMI global resets.
# Usage: ./degrade.sh [min_count]
#   Default: keep going until >= 20 chips are at Gen1.

set -euo pipefail

MIN="${1:-20}"
DISCOVER="$(dirname "$0")/../tools/discover"
MAX_ROUNDS=10

echo "Target: >= $MIN chips at Gen1"

for round in $(seq 1 "$MAX_ROUNDS"); do
    sudo ipmitool raw 0x30 0x8B 0xF 0xFF 0x0 0xFF 2>/dev/null
    sleep 10

    n=$("$DISCOVER" 2>/dev/null | grep -c 'Gen1/Gen5' || true)
    echo "Round $round: $n chips at Gen1"

    if [ "$n" -ge "$MIN" ]; then
        echo "Done. $n >= $MIN"
        "$DISCOVER" 2>/dev/null | grep 'Gen1/Gen5' | sort -t' ' -k2
        exit 0
    fi
done

echo "Gave up after $MAX_ROUNDS rounds (only $n at Gen1)."
exit 1
