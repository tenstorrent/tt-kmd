#!/bin/bash

# SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

# Configuration
PACKAGE_NAME="tenstorrent-dkms"
MODULE_NAME="tenstorrent"

# Determine project root (parent of tools/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Parse version from module.h (e.g., "2.6.1-pre" or "2.6.1")
VERSION=$("${PROJECT_ROOT}/tools/current-version")

# Sanity check: verify dkms.conf matches module.h
DKMS_VERSION=$(grep -E '^\s*PACKAGE_VERSION\s*=' "${PROJECT_ROOT}/dkms.conf" | sed -E 's/.*"([^"]*)".*/\1/')
if [[ "${VERSION}" != "${DKMS_VERSION}" ]]; then
    echo "Error: Version mismatch!" >&2
    echo "  module.h: ${VERSION}" >&2
    echo "  dkms.conf: ${DKMS_VERSION}" >&2
    echo "" >&2
    echo "These must match. Please update dkms.conf or module.h to match." >&2
    exit 1
fi

# RPM doesn't allow hyphens in Version field. Convert:
#   "2.6.1-pre"  -> Version: 2.6.1~pre  (sorts BEFORE 2.6.1)
#   "2.6.1-rc1"  -> Version: 2.6.1~rc1  (sorts BEFORE 2.6.1)
#   "2.6.1"      -> Version: 2.6.1
RPM_VERSION="${VERSION//-/\~}"

echo "Building ${PACKAGE_NAME}"
echo "  Source version: ${VERSION}"
echo "  RPM version:    ${RPM_VERSION}"

# Create temporary build directory
BUILD_DIR=$(mktemp -d --tmpdir)
trap 'rm -rf "${BUILD_DIR}"' EXIT

# Set up RPM build tree
RPMBUILD="${BUILD_DIR}/rpmbuild"
mkdir -p "${RPMBUILD}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Create source tarball
# RPM expects the tarball to extract to a directory named ${MODULE_NAME}-${RPM_VERSION}
SRC_STAGING="${BUILD_DIR}/${MODULE_NAME}-${RPM_VERSION}"
mkdir -p "${SRC_STAGING}"

echo "Creating source tarball..."
cd "${PROJECT_ROOT}"
tar -c --exclude-from=tools/exclude-from-release -f - . | tar -C "${SRC_STAGING}" -xf -

# Create the tarball
tar -C "${BUILD_DIR}" -czf "${RPMBUILD}/SOURCES/${PACKAGE_NAME}-${RPM_VERSION}.tar.gz" \
    "${MODULE_NAME}-${RPM_VERSION}"

# Generate spec file
cat > "${RPMBUILD}/SPECS/${PACKAGE_NAME}.spec" << EOF
Name:           ${PACKAGE_NAME}
Version:        ${RPM_VERSION}
Release:        1%{?dist}
Summary:        Tenstorrent kernel mode driver (DKMS)

License:        GPL-2.0-only
URL:            https://github.com/tenstorrent/tt-kmd
Source0:        %{name}-${RPM_VERSION}.tar.gz

BuildArch:      noarch
Requires:       dkms
Requires:       kernel-devel

%description
This package provides the Tenstorrent kernel module as DKMS source.
The module will be built automatically by DKMS when installed.

%prep
%setup -q -n ${MODULE_NAME}-${RPM_VERSION}

%build
# No build required - DKMS will build the module

%install
mkdir -p %{buildroot}/usr/src/${MODULE_NAME}-${RPM_VERSION}

# Copy all source files
cp -a . %{buildroot}/usr/src/${MODULE_NAME}-${RPM_VERSION}/

# Make dkms-post-install executable
chmod 755 %{buildroot}/usr/src/${MODULE_NAME}-${RPM_VERSION}/dkms-post-install

# Install udev rules
mkdir -p %{buildroot}/lib/udev/rules.d
cp udev-50-tenstorrent.rules %{buildroot}/lib/udev/rules.d/

%post
# Add module to DKMS
dkms add -m ${MODULE_NAME} -v ${RPM_VERSION} || true

# Build and install for all kernels that have headers installed
BUILT_FOR=""
for kernel in /lib/modules/*/build; do
    if [ -d "\$kernel" ]; then
        kernel_ver=\$(basename \$(dirname "\$kernel"))
        if dkms build -m ${MODULE_NAME} -v ${RPM_VERSION} -k "\$kernel_ver" 2>/dev/null; then
            dkms install -m ${MODULE_NAME} -v ${RPM_VERSION} -k "\$kernel_ver" 2>/dev/null || true
            BUILT_FOR="\${BUILT_FOR} \${kernel_ver}"
        fi
    fi
done

# Reload udev rules
udevadm control --reload 2>/dev/null || true

# Try to load the module for the running kernel
RUNNING=\$(uname -r)
if modprobe ${MODULE_NAME} 2>/dev/null; then
    echo "${MODULE_NAME}: loaded successfully"
else
    echo ""
    echo "====================================================================="
    echo "${MODULE_NAME} ${RPM_VERSION} installed"
    echo ""
    echo "Module built for:\${BUILT_FOR:-  (none)}"
    echo "Running kernel:   \${RUNNING}"
    echo ""
    if ls /lib/modules/\${RUNNING}/extra/${MODULE_NAME}.ko* >/dev/null 2>&1; then
        echo "Module is available. Try: sudo modprobe ${MODULE_NAME}"
    else
        echo "Module not built for running kernel. Either:"
        echo "  1. Reboot to use a kernel with the module, or"
        echo "  2. Install headers and rebuild:"
        echo "     sudo dnf install kernel-devel-\${RUNNING}"
        echo "     sudo dkms build -m ${MODULE_NAME} -v ${RPM_VERSION} -k \${RUNNING}"
        echo "     sudo dkms install -m ${MODULE_NAME} -v ${RPM_VERSION} -k \${RUNNING}"
    fi
    echo "====================================================================="
fi

%preun
# Remove from DKMS on both upgrade and uninstall
# (arg1 = 0 for uninstall, arg1 = 1 for upgrade)
dkms remove -m ${MODULE_NAME} -v ${RPM_VERSION} --all 2>/dev/null || true

%files
/usr/src/${MODULE_NAME}-${RPM_VERSION}
/lib/udev/rules.d/udev-50-tenstorrent.rules

%changelog
* $(date '+%a %b %d %Y') Tenstorrent <releases@tenstorrent.com> - ${RPM_VERSION}-1
- Automated build, version ${VERSION}
EOF

# Build the RPM
echo "Building RPM package..."
rpmbuild --define "_topdir ${RPMBUILD}" -bb "${RPMBUILD}/SPECS/${PACKAGE_NAME}.spec"

# Find and move the built RPM
RPM_FILE=$(find "${RPMBUILD}/RPMS" -name "*.rpm" -type f | head -1)
if [[ -z "${RPM_FILE}" ]]; then
    echo "Error: RPM build failed - no .rpm file found" >&2
    exit 1
fi

# Move RPM to project root or specified output directory
OUTPUT_DIR="${PROJECT_ROOT}"
if [[ -n "${RPM_OUTPUT_DIR:-}" ]]; then
    OUTPUT_DIR="${RPM_OUTPUT_DIR}"
fi

OUTPUT_FILE="${OUTPUT_DIR}/$(basename "${RPM_FILE}")"
cp "${RPM_FILE}" "${OUTPUT_FILE}"

echo ""
echo "Successfully built: ${OUTPUT_FILE}"
echo ""
echo "Package details:"
rpm -qip "${OUTPUT_FILE}"
echo ""
echo "Package contents:"
rpm -qlp "${OUTPUT_FILE}"

exit 0
