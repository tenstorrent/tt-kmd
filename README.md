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
    * `dnf install epel-release && dnf install dkms` (Enterprise Linux based)
```
sudo dkms add .
sudo dkms install tenstorrent/2.0.0
sudo modprobe tenstorrent
```
(or reboot, driver will auto-load next boot)

* (Alpine Linux) You must have akms installed.
    * `apk install akms`
```
doas akms install .
doas modprobe tenstorrent
```

### To uninstall:
```
sudo modprobe -r tenstorrent
sudo dkms remove tenstorrent/2.0.0 --all
```
* Alpine Linux (`akms`)
```
doas modprobe -r tenstorrent
doas akms uninstall tenstorrent
```
