## Tenstorrent AI Kernel-Mode Driver

## Official Repository

[https://github.com/tenstorrent/tt-kmd](https://github.com/tenstorrent/tt-kmd)

## Supported hardware:
* Grayskull
* Wormhole
* Blackhole

The driver registers device files named `/dev/tenstorrent/%d`, one for each enumerated device.

### To install:

* You must have dkms installed.
    * `apt install dkms` (Debian, Ubuntu)
    * `dnf install dkms` (Fedora)
    * `apk install akms` (Alpine)
    * `dnf install epel-release && dnf install dkms` (Enterprise Linux based)
```
sudo dkms add .
sudo dkms install tenstorrent/2.0.0
sudo modprobe tenstorrent
```
* For Alpine linux
```
doas akms install .
doas modeprobe tenstorrent
```
(or reboot, driver will auto-load next boot)

### To uninstall:
```
sudo modprobe -r tenstorrent
sudo dkms remove tenstorrent/2.0.0 --all
```
* For Alpine linux
```
doas modeprobe -r tenstorrent
doas akms remove tenstorrent
```