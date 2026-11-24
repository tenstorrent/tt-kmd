# Versions and releases

## Where the version is

The driver version is in three files. They must always hold the same version.

1. **`module.h`** - `TENSTORRENT_DRIVER_VERSION_MAJOR`, `_MINOR`, `_PATCH` and `_SUFFIX`
2. **`dkms.conf`** - `PACKAGE_VERSION="X.Y.Z"`
3. **`AKMBUILD`** - `modver=X.Y.Z`

Do not edit them by hand. Use the script:

```bash
tools/set-version 2.13.1-pre
```

`tools/current-version` prints the current version (it reads `module.h`).
The Makefile uses it, so `make dkms` always uses the correct version.

**`ioctl.h`** - `TENSTORRENT_DRIVER_VERSION` is the ioctl API version (usually
only the major version). Update it by hand when necessary.

## Versions on `main`

`main` always holds a development version that ends with `-pre`, for example
`2.13.1-pre`. `main` never holds a release version. A release version is only
in the tagged release commit.

The file **`RELEASE`** holds one line: the version of the last release that
was started, for example `2.13.0` or `2.13.0-rc1`.

## How to make a release

Run the helper. It opens the pull request described below; merge it to start
the release:

```bash
tools/release 2.13.0            # main holds 2.13.1-pre afterwards
tools/release --dry-run 2.13.0  # only show the change
```

It does these steps for you:

1. Make a branch from `main`.
2. Write the version to release in `RELEASE`:

   ```bash
   echo 2.13.0 > RELEASE
   ```

3. Set the next development version in the three version files. This is the
   version that `main` will hold after the release:

   ```bash
   tools/set-version 2.13.1-pre
   ```

   If `main` already holds the correct `-pre` version (for example after a
   release candidate), skip this step.
4. Open a pull request with these changes and merge it after review.

The merge starts the **Create Release** workflow. It needs no other step,
except the approval of the `release` environment if one is required. The
workflow:

1. Checks `RELEASE` and the version on `main`.
2. Makes the release commit: the merged commit plus the release version in
   the three version files. This commit is not on `main` or on any branch.
3. Builds the `.deb` and `.rpm` packages from that source.
4. Pushes the tag `ttkmd-<version>` on the release commit, only after both
   builds pass.
5. Makes a draft GitHub release, uploads the packages, checks them, and then
   publishes the release. A version with a suffix (for example `-rc1`) is
   published as a pre-release.

Notes:

- A tag is never moved or deleted, and a published release is never changed.
  If a release is wrong, release a new version.
- If the workflow fails before it publishes, fix the cause and use
  **Re-run failed jobs** on the failed run. It continues with the same
  release commit, the same tag and the same draft release.
- You can also start **Create Release** from the Actions page (`Run workflow`
  on `main`). It reads `RELEASE` from the newest commit on `main`. If `main`
  changed after the tag was pushed, the tag is on an older commit and the
  workflow stops. Then put a new version in `RELEASE`.
- If the workflow starts for a version that is already published, it stops
  with success and does nothing.
- The workflow does not push to `main`.
