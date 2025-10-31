#!/bin/bash
# SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

# Script to analyze struct padding in ioctl.h using pahole
#
# This script verifies that all padding in ioctl.h structures is explicit.
# Implicit padding in kernel-userspace ABI structures can lead to portability
# and security issues. All padding should be made explicit using reserved fields.
#
# Requirements:
#   - pahole (from dwarves package)
#   - gcc with debug info support
#
# Usage:
#   ./pahole_check.sh           # Check for padding (exit 1 if found)
#   ./pahole_check.sh --verbose # Show all struct layouts
#   ./pahole_check.sh -v        # Same as --verbose
#
# Exit codes:
#   0 - No implicit padding found
#   1 - Implicit padding detected (or other error)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Setup cleanup trap for temporary files
cleanup() {
    rm -f "$SCRIPT_DIR/pahole_dummy.o" "$SCRIPT_DIR/pahole_dummy.c"
}
trap cleanup EXIT

# Check if pahole is available
if ! command -v pahole &> /dev/null; then
    echo "ERROR: pahole not found. Please install dwarves package." >&2
    echo "  Debian/Ubuntu: sudo apt-get install dwarves" >&2
    echo "  Fedora/RHEL:   sudo dnf install dwarves" >&2
    exit 1
fi

# Extract all struct names from ioctl.h
mapfile -t STRUCTS < <(grep '^struct ' "$ROOT_DIR/ioctl.h" | awk '{print $2}' | sort -u)

# Generate pahole_dummy.c with all struct instantiations
echo "Generating pahole_dummy.c..."
cat > "$SCRIPT_DIR/pahole_dummy.c" << 'EOF'
// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Auto-generated file to instantiate all ioctl.h structs for pahole analysis
// This ensures pahole can find type information in the DWARF debug data

#include "../ioctl.h"

EOF

# Add struct instantiations
idx=1
for struct in "${STRUCTS[@]}"; do
    echo "struct $struct v$idx;" >> "$SCRIPT_DIR/pahole_dummy.c"
    idx=$((idx + 1))
done

# Compile pahole_dummy.c with debug info for pahole to analyze
echo "Compiling with debug info..."
if ! gcc -g -c -o "$SCRIPT_DIR/pahole_dummy.o" "$SCRIPT_DIR/pahole_dummy.c" 2>&1; then
    echo "ERROR: Failed to compile pahole_dummy.c" >&2
    exit 1
fi

echo ""
echo "========================================"
echo "Pahole Analysis of ioctl.h Structures"
echo "========================================"
echo ""

FOUND_ISSUES=0

for struct in "${STRUCTS[@]}"; do
    OUTPUT=$(pahole -C "$struct" "$SCRIPT_DIR/pahole_dummy.o" 2>/dev/null || true)

    if [ -z "$OUTPUT" ]; then
        continue
    fi

    # Check if this struct has holes or padding
    if echo "$OUTPUT" | grep -q "XXX.*hole\|padding:"; then
        echo "PADDING DETECTED: struct $struct"
        echo "$OUTPUT"
        echo ""
        FOUND_ISSUES=$((FOUND_ISSUES + 1))
    fi
done

# Also show all structs for reference if requested
if [ "$1" == "--verbose" ] || [ "$1" == "-v" ]; then
    echo ""
    echo "========================================"
    echo "Full Details of All Structures"
    echo "========================================"
    echo ""
    for struct in "${STRUCTS[@]}"; do
        OUTPUT=$(pahole -C "$struct" "$SCRIPT_DIR/pahole_dummy.o" 2>/dev/null || true)
        if [ -n "$OUTPUT" ]; then
            echo "struct $struct:"
            echo "$OUTPUT"
            echo ""
        fi
    done
fi

if [ $FOUND_ISSUES -gt 0 ]; then
    echo "========================================"
    echo "Summary: Found $FOUND_ISSUES structure(s) with implicit padding"
    echo "========================================"
    exit 1
else
    echo "========================================"
    echo "No implicit padding found!"
    echo "========================================"
    exit 0
fi

