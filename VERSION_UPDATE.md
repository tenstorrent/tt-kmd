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

`main` holds a development version that ends with `-pre`, for example
`2.13.1-pre`. A release version is only in the release commit, which is in the
history of `main` and has the tag. The next commit sets the development
version again, so a checkout of `main` never reports a release version.

```
main:  A ─────────────── M     (merge of the release pull request)
        \               /
         R ─────────── B
         TTKMD v2.13.0  Bump version to 2.13.1-pre for development
         ↑ tag ttkmd-2.13.0
```

The file **`RELEASE`** holds one line: the version of the last release that
was started, for example `2.13.0` or `2.13.0-rc1`.

## How to make a release

1. Run the helper. It opens the release pull request:

   ```bash
   tools/release 2.13.0            # main holds 2.13.1-pre afterwards
   tools/release --dry-run 2.13.0  # only show the change
   ```

   The pull request holds two commits:

   - the release commit: `RELEASE` and the three version files hold `2.13.0`
   - the development commit: the three version files hold `2.13.1-pre`
     (use `--next` for a different development version)

2. Merge the pull request with **Create a merge commit** or **Rebase and
   merge**. **Do not squash.** A squash removes the release commit. The
   workflow then stops before it makes a tag. To recover:

   1. Revert the squashed commit on `main` with a pull request. (Its merge
      starts the workflow for the release before; that run ends with success
      and does nothing.)
   2. Delete the branch `release/<version>` on GitHub if it still exists.
   3. Run `tools/release <version>` again and merge without a squash.

The merge starts the **Create Release** workflow. It needs no other step,
except the approval of the `release` environment if one is required. The
workflow:

1. Reads `RELEASE` and checks that `main` holds a `-pre` version.
2. Finds the release commit: the newest commit in the history of `main` that
   changed `RELEASE`. It must hold the release version in `RELEASE` and in the
   three version files.
3. Builds the `.deb` and `.rpm` packages from the release commit.
4. Pushes the tag `ttkmd-<version>` on the release commit, only after both
   builds pass.
5. Makes a draft GitHub release, uploads the packages, checks them, and then
   publishes the release. A version with a suffix (for example `-rc1`) is
   published as a pre-release.

Notes:

- A tag is never moved or deleted, and a published release is never changed.
  If a release is wrong, release a new version.
- If the workflow fails before it publishes, fix the cause and use
  **Re-run failed jobs** on the failed run, or start **Create Release** from
  the Actions page (`Run workflow` on `main`). Both find the same release
  commit, also if `main` has new commits.
- If the workflow starts for a version that is already published, it stops
  with success and does nothing.
- The workflow does not push to `main` or to any branch. It does not make
  commits.
