# Version Update Process

The package version is required in multiple files. To update the version:

## Primary Source of Truth
- **`dkms.conf`** - Update `PACKAGE_VERSION="X.Y.Z"`

## Files that need manual updates
1. **`AKMBUILD`** - Update `modver=X.Y.Z`
2. **`debian/changelog`** - Add new entry with updated version
3. **`module.h`** - Update `TENSTORRENT_DRIVER_VERSION_MAJOR`, `MINOR`, and `PATCH` defines as necessary.
4. **`ioctl.h`** - Update `TENSTORRENT_DRIVER_VERSION` (typically just the major version) as necessary.

## Automated Usage
The Makefile automatically extracts the version from `dkms.conf` for DKMS operations, so `make dkms` will always use the correct version.

For scripts that need the current version, use `tools/current-version` which extracts it from `dkms.conf`. 