#!/bin/bash

# SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

# Configuration
PACKAGE_NAME="tenstorrent-dkms"
MODULE_NAME="tenstorrent"
MAINTAINER="Tenstorrent <releases@tenstorrent.com>"
HOMEPAGE="https://github.com/tenstorrent/tt-kmd"

# Determine project root (parent of tools/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Parse version from module.h
VERSION=$("${PROJECT_ROOT}/tools/current-version")

# Sanity check: verify dkms.conf matches module.h
DKMS_VERSION=$(grep -E '^s*PACKAGE_VERSIONs*=' "${PROJECT_ROOT}/dkms.conf" | sed -E 's/.*"([^"]*)".*/\1/')
if [[ "${VERSION}" != "${DKMS_VERSION}" ]]; then
    echo "Error: Version mismatch!" >&2
    echo "  module.h: ${VERSION}" >&2
    echo "  dkms.conf: ${DKMS_VERSION}" >&2
    echo "" >&2
    echo "These must match. Please update dkms.conf or module.h to match." >&2
    exit 1
fi

echo "Building ${PACKAGE_NAME} version ${VERSION}"

# Create temporary build directory
BUILD_DIR=$(mktemp -d --tmpdir)
trap 'rm -rf "${BUILD_DIR}"' EXIT

PACKAGE_DIR="${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}_all"
DEBIAN_DIR="${PACKAGE_DIR}/DEBIAN"
SRC_DIR="${PACKAGE_DIR}/usr/src/${MODULE_NAME}-${VERSION}"

mkdir -p "${DEBIAN_DIR}"
mkdir -p "${SRC_DIR}"

# Copy source files to package
echo "Copying source files..."
cd "${PROJECT_ROOT}"
tar -c --exclude-from=tools/exclude-from-release -f - . | tar -C "${SRC_DIR}" -xf -

# Make dkms-post-install executable
chmod 755 "${SRC_DIR}/dkms-post-install"

# Create control file
cat > "${DEBIAN_DIR}/control" << EOF
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: misc
Priority: optional
Architecture: all
Depends: dkms, linux-headers-generic | linux-headers
Maintainer: ${MAINTAINER}
Homepage: ${HOMEPAGE}
Description: Tenstorrent kernel mode driver (DKMS)
 This package provides the Tenstorrent kernel module as DKMS source.
 The module will be built automatically by DKMS when installed.
EOF

# Install udev rules
mkdir -p "${PACKAGE_DIR}/lib/udev/rules.d"
cp -v udev-50-tenstorrent.rules "${PACKAGE_DIR}/lib/udev/rules.d/"

# Create postinst script
cat > "${DEBIAN_DIR}/postinst" << 'EOF'
#!/bin/sh
set -e

# Pass arguments to common postinst script
/usr/lib/dkms/common.postinst tenstorrent __VERSION__ /usr/share/tenstorrent "" "$2"

# Try to load the module for the running kernel
modprobe tenstorrent 2>/dev/null || true

# Reload udev rules
udevadm control --reload || true

exit 0
EOF

# Replace version placeholder
sed -i "s/__VERSION__/${VERSION}/g" "${DEBIAN_DIR}/postinst"
chmod 755 "${DEBIAN_DIR}/postinst"

# Create prerm script
cat > "${DEBIAN_DIR}/prerm" << 'EOF'
#!/bin/sh
set -e

case "$1" in
    remove)
        dkms remove -m tenstorrent -v __VERSION__ --all
        ;;
esac

exit 0
EOF

# Replace version placeholder
sed -i "s/__VERSION__/${VERSION}/g" "${DEBIAN_DIR}/prerm"
chmod 755 "${DEBIAN_DIR}/prerm"

# Build the .deb package
echo "Building .deb package..."
dpkg-deb --build "${PACKAGE_DIR}"

# Move .deb to project root or specified output directory
OUTPUT_DIR="${PROJECT_ROOT}"
if [[ -n "${DEB_OUTPUT_DIR:-}" ]]; then
    OUTPUT_DIR="${DEB_OUTPUT_DIR}"
fi

DEB_FILE="${PACKAGE_NAME}_${VERSION}_all.deb"
mv "${BUILD_DIR}/${DEB_FILE}" "${OUTPUT_DIR}/"

echo ""
echo "Successfully built: ${OUTPUT_DIR}/${DEB_FILE}"
echo ""
echo "Package details:"
dpkg-deb --info "${OUTPUT_DIR}/${DEB_FILE}"
echo ""

echo "Package contents:"
dpkg-deb --contents "${OUTPUT_DIR}/${DEB_FILE}"

exit 0

