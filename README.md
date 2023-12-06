## Tenstorrent AI Kernel-Mode Driver

Supported hardware:
* Grayskull
* Wormhole

The driver registers device files named `/dev/tenstorrent/%d`, one for each enumerated device.

### To install:

* You must have dkms installed.
    * `apt install dkms` (Debian, Ubuntu)
    * `dnf install dkms` (Fedora, Enterprise Linux based)
```
sudo dkms add .
sudo dkms install tenstorrent/1.26
sudo modprobe tenstorrent
```
(or reboot, driver will auto-load next boot)

### To uninstall:
```
sudo modprobe -r tenstorrent
sudo dkms remove tenstorrent/1.26 --all
```

