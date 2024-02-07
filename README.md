## Tenstorrent AI Kernel-Mode Driver

## Official Repository

[https://github.com/tenstorrent/tt-kmd](https://github.com/tenstorrent/tt-kmd)

## Supported hardware:
* Grayskull
* Wormhole

The driver registers device files named `/dev/tenstorrent/%d`, one for each enumerated device.

### To install:

* You must have dkms installed.
    * `apt install dkms` (Debian, Ubuntu)
    * `dnf install dkms` (Fedora)
    * `dnf install epel-release && dnf install dkms` (Enterprise Linux based)
```
sudo dkms add .
sudo dkms install tenstorrent/1.26
sudo modprobe tenstorrent
```
(or reboot, driver will auto-load next boot)


For NixOS users, add:
```nix
"${builtins.fetchTarball "https://github.com/tenstorrent/tt-kmd/archive/main.tar.gz"}/tt-kmd.nix"
```
to your `configuration.nix`. The module will load on the next boot. 

### To uninstall:
```
sudo modprobe -r tenstorrent
sudo dkms remove tenstorrent/1.26 --all
```

