## Tenstorrent AI Kernel-Mode Driver

## Official Repository

[https://github.com/tenstorrent/tt-kmd](https://github.com/tenstorrent/tt-kmd)

## Supported hardware:
* Wormhole
* Blackhole

The driver registers device files named `/dev/tenstorrent/%d`, one for each enumerated device.

### To install from source:

* You must have dkms installed.
    * `apt install dkms` (Debian, Ubuntu)
    * `dnf install dkms` (Fedora)
    * `apk install akms` (Alpine)
    * `dnf install epel-release && dnf install dkms` (Enterprise Linux based)
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
