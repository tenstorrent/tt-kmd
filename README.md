## Tenstorrent AI Kernel-Mode Driver

## Official Repository

[https://github.com/tenstorrent/tt-kmd](https://github.com/tenstorrent/tt-kmd)

## Supported hardware:
* Wormhole
* Blackhole

The driver registers device files named `/dev/tenstorrent/%d`, one for each enumerated device.

## Supported kernels:

Linux 5.4 or later. The driver is build-tested against mainline kernel versions from 5.4 through 7.2.

### To install:

Pre-built `.deb` packages for Debian and Ubuntu are available at [https://github.com/tenstorrent/tt-kmd/releases](https://github.com/tenstorrent/tt-kmd/releases)

For other distributions, install from source (see below).

### To install from source:

* You must have dkms installed.
    * `apt install dkms` (Debian, Ubuntu)
    * `dnf install dkms` (Fedora)
    * `apk install akms` (Alpine)
    * `dnf install epel-release && dnf install dkms` (Enterprise Linux based)
    * `emerge sys-kernel/dkms` (Gentoo)
```
make dkms
```
* For Alpine linux
```
make akms
```

#### With NixOS

1. Add this repository as a nix flake input:
```nix
inputs.tt-kmd.url = "github:tenstorrent/tt-kmd";
```

2. Add in the overlay:
```nix
nixpkgs.overlays = [ tt-kmd.overlays.default ];
```

3. Add the package to the kernel modules and udev packages:
```nix
boot.extraModulePackages = [ config.boot.kernelPackages.tt-kmd ];
services.udev.packages = [ config.boot.kernelPackages.tt-kmd ];
```

4. Rebuild: `nixos-rebuild switch`

### To uninstall:
```
make dkms-remove
```
* For Alpine linux
```
make akms-remove
```

## Security Model

A Tenstorrent device is a fully programmable, DMA-capable PCIe accelerator. Opening `/dev/tenstorrent/N` grants complete control, including programming the NOC from the host or device cores. Device-node permissions control access; the driver cannot mediate subsequent device activity.

**Device and host.** Without a translating IOMMU, any device user can read or overwrite arbitrary host memory. With translation, DMA is limited to memory mapped into the device's IOMMU domain. Keep the IOMMU enabled and translating to protect host memory. Disabling it (for example, with `intel_iommu=off`) or enabling passthrough (`iommu=pt`) leaves host memory unprotected from device users. The driver supports these configurations, but all device users must then be trusted with unrestricted host-memory access.

**Users of one device.** All users share the device's cores, memory, and IOMMU domain. Any user can read or overwrite another's device memory and mapped host buffers, including those of privileged processes. Any user can also hang or reset the device. Mutually untrusting users must never use the same device concurrently.

**Assigning devices.** Assign each device to one user or mutually trusting group at a time. Handoff between mutually untrusting users is an orchestration responsibility: revoke the previous user's access and perform a full reset (`tt-smi -r`), which stops device execution and scrubs device memory. Separate-device tenant isolation depends on the platform, not just the driver.

The shipped udev rule uses mode `0666` for trusted development and single-user hosts. Otherwise, restrict access—for example, to a `tenstorrent` group using a later-sorting rule:

```
# /etc/udev/rules.d/60-tenstorrent.rules
SUBSYSTEM=="tenstorrent", MODE="0660", GROUP="tenstorrent"
```

### Reporting security bugs

Access permitted by this model, including unrestricted DMA without a translating IOMMU, is not itself a driver vulnerability. Driver flaws that compromise the host kernel beyond that access, or allow DMA outside the device's translating IOMMU domain, are security bugs. Report suspected security bugs through [GitHub's private vulnerability reporting](https://github.com/tenstorrent/tt-kmd/security/advisories/new).
