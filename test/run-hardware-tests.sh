#!/bin/bash
# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only
#
# Hardware test script - runs locally or in CI

set -e

# Echo command before running it
run() {
    echo "# $@"
    "$@"
}

# Run command with timeout and diagnostics on hang
run_with_timeout() {
    local timeout_secs=$1
    shift
    echo "# timeout ${timeout_secs}s: $@"
    
    # Disable set -e temporarily to capture timeout exit code
    set +e
    timeout "$timeout_secs" "$@"
    local exit_code=$?
    set -e
    
    if [ $exit_code -eq 124 ]; then
        echo ""
        echo "!!! TEST HUNG AFTER ${timeout_secs}s - COLLECTING DIAGNOSTICS !!!"
        echo ""
        echo "=== dmesg (last 200 lines) ==="
        dmesg | tail -200
        echo ""
        echo "=== lscpu ==="
        lscpu
        echo ""
        echo "=== /proc/cmdline ==="
        cat /proc/cmdline
        echo ""
        echo "=== uname -a ==="
        uname -a
        echo ""
        exit 124
    fi
    
    return $exit_code
}

echo "=== Hardware Test ==="

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "This script must be run as root (or with sudo)"
    exit 1
fi

# Figure out where we are and cd to project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$SCRIPT_DIR/../Makefile" ] && [ -f "$SCRIPT_DIR/../dkms.conf" ]; then
    # We're in test/ directory, go up to project root
    cd "$SCRIPT_DIR/.."
elif [ -f "$SCRIPT_DIR/Makefile" ] && [ -f "$SCRIPT_DIR/dkms.conf" ]; then
    # We're already in project root
    cd "$SCRIPT_DIR"
else
    echo "ERROR: Cannot find project root (looking for Makefile and dkms.conf)"
    exit 1
fi

echo "Working directory: $(pwd)"

# Check existing module version
echo ""
echo "=== Check existing driver version ==="
if [ -f /sys/module/tenstorrent/version ]; then
    run cat /sys/module/tenstorrent/version
    echo ""
    echo "=== Remove existing driver ==="
    run rmmod tenstorrent
else
    echo "No existing KMD module loaded"
fi

# Remove all DKMS versions if any exist
for ver in $(dkms status tenstorrent 2>/dev/null | sed -nE 's|^tenstorrent/([^,]+),.*|\1|p' | sort -u); do
    echo ""
    echo "=== Remove DKMS version $ver ==="
    run dkms remove tenstorrent/$ver --all || true
done

# Build driver
echo ""
echo "=== Build driver ==="
run make -j $(nproc)

# Load driver
echo ""
echo "=== Load driver ==="
run insmod tenstorrent.ko

# Verify devices appeared
if [ ! -d /dev/tenstorrent/ ]; then
    echo "ERROR: /dev/tenstorrent/ does not exist - no hardware found"
    exit 1
fi

echo ""
echo "=== Check loaded driver version ==="
run cat /sys/module/tenstorrent/version

echo ""
echo "=== Check devices ==="
DEV_COUNT=$(ls -1 /dev/tenstorrent/ | wc -l)
echo "Found $DEV_COUNT device(s)"
run ls -la /dev/tenstorrent/

# Show hardware info for each device
for dev in /sys/class/tenstorrent/tenstorrent!*; do
    if [ -d "$dev" ]; then
        dev_name=$(basename "$dev")
        echo ""
        echo "=== Hardware info: $dev_name ==="
        (cd "$dev" && grep . tt_* 2>/dev/null | sed 's/^/  /' || echo "  (no info available)")
    fi
done

# Build ttkmd_test
echo ""
echo "=== Build ttkmd_test ==="
run make -C test -j $(nproc)

# Allocate hugepages for testing if needed
echo ""
echo "=== Check hugepages for testing ==="
HUGEPAGES_2MB=/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
CURRENT_HUGEPAGES=$(cat $HUGEPAGES_2MB)
if [ "$CURRENT_HUGEPAGES" -lt 2 ]; then
    echo "Allocating 2x 2MB hugepages (current: $CURRENT_HUGEPAGES)..."
    echo 2 > $HUGEPAGES_2MB
    echo "Allocated 2MB hugepages: $(cat $HUGEPAGES_2MB)"
else
    echo "2MB hugepages already available: $CURRENT_HUGEPAGES"
fi

# Run ttkmd_test with timeout
echo ""
echo "=== Run ttkmd_test ==="
run_with_timeout 120 test/ttkmd_test

echo ""
echo "=== All tests passed! ==="

