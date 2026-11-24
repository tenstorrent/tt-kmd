#!/bin/bash

# SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

# Make the release commit on top of the current HEAD.
#
# Usage: .github/scripts/release-commit.sh <version>
#
# The commit sets the three version files to <version>. Its parent is HEAD.
# Author, committer and dates are fixed (dates come from HEAD), so the same
# HEAD and the same version always give the same commit hash. The workflow
# uses this to make the commit again in a later job or in a rerun.
#
# The commit is made on a detached HEAD. It is not pushed to a branch.
# Prints the hash of the new commit on stdout.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <version>" >&2
    exit 2
fi

VERSION="$1"

cd "$(git rev-parse --show-toplevel)"

if ! git diff --quiet HEAD -- module.h dkms.conf AKMBUILD; then
    echo "Error: module.h, dkms.conf or AKMBUILD has local changes" >&2
    exit 1
fi

BASE_DATE="$(git show -s --format=%cI HEAD)"

./tools/set-version "${VERSION}" >&2

if git diff --quiet HEAD -- module.h dkms.conf AKMBUILD; then
    echo "Error: the version files already hold ${VERSION}; there is nothing to commit" >&2
    exit 1
fi

export GIT_AUTHOR_NAME="Tenstorrent Releases"
export GIT_AUTHOR_EMAIL="releases@tenstorrent.com"
export GIT_AUTHOR_DATE="${BASE_DATE}"
export GIT_COMMITTER_NAME="${GIT_AUTHOR_NAME}"
export GIT_COMMITTER_EMAIL="${GIT_AUTHOR_EMAIL}"
export GIT_COMMITTER_DATE="${BASE_DATE}"

git checkout --quiet --detach
git add module.h dkms.conf AKMBUILD
git -c commit.gpgsign=false commit --quiet --no-verify -m "TTKMD v${VERSION}" >&2

git rev-parse HEAD
