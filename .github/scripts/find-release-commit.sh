#!/bin/bash

# SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

# Find the release commit in the history of HEAD and check it.
#
# Usage: .github/scripts/find-release-commit.sh <version>
#
# A release pull request holds two commits (tools/release makes them):
#
#   1. the release commit: RELEASE and the three version files hold <version>
#   2. the development commit: the version files hold the next X.Y.Z-pre
#
# The release commit is the newest commit in the history of HEAD that changed
# RELEASE. This script checks that it holds <version> in RELEASE, module.h,
# dkms.conf and AKMBUILD, and prints its hash on stdout.
#
# The check fails if the pull request was squash-merged, because then no
# commit holds the release version. Nothing is tagged in that case.
#
# Needs the full history (not a shallow clone).

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <version>" >&2
    exit 2
fi

VERSION="$1"

cd "$(git rev-parse --show-toplevel)"

if [[ "$(git rev-parse --is-shallow-repository)" == "true" ]]; then
    echo "Error: this is a shallow clone; the full history is necessary" >&2
    exit 1
fi

sha="$(git log -1 --format=%H HEAD -- RELEASE)"
if [[ -z "${sha}" ]]; then
    echo "Error: no commit in the history of HEAD changed RELEASE" >&2
    exit 1
fi

# Read the files of that commit without a change to the checkout.
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
git archive "${sha}" RELEASE module.h dkms.conf AKMBUILD tools/current-version | tar -x -C "${tmp}"

release_file="$(tr -d '[:space:]' < "${tmp}/RELEASE")"
module_version="$("${tmp}/tools/current-version")"
dkms_version="$(sed -n -E 's/^PACKAGE_VERSION="([^"]*)"$/\1/p' "${tmp}/dkms.conf")"
akm_version="$(sed -n -E 's/^modver=(.*)$/\1/p' "${tmp}/AKMBUILD")"

{
    echo "Commit that changed RELEASE: ${sha}"
    echo "  RELEASE:   ${release_file}"
    echo "  module.h:  ${module_version}"
    echo "  dkms.conf: ${dkms_version}"
    echo "  AKMBUILD:  ${akm_version}"
} >&2

for found in "${release_file}" "${module_version}" "${dkms_version}" "${akm_version}"; do
    if [[ "${found}" != "${VERSION}" ]]; then
        {
            echo "Error: commit ${sha} does not hold version ${VERSION} in RELEASE, module.h, dkms.conf and AKMBUILD."
            echo "The release commit must hold the release version in all four files."
            echo "A squash merge of the release pull request causes this. Make a new"
            echo "release pull request with tools/release and do not squash it."
        } >&2
        exit 1
    fi
done

echo "${sha}"
