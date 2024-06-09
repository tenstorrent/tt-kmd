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
to the imports list in your `configuration.nix`. The module will load on the next boot.

Make sure to add the following to your `configuration.nix` as well:

``` nix
boot.kernelParams = ["hugepagesz=1GB"
                     "hugepages=1"
                     "iommu=pt"
                     "nr_hugepages=1" ];

  systemd.mounts = [
      {
        where = "/dev/hugepages";
        enable = false;
      }
      { where = "/dev/hugepages/hugepages-1G";
          enable  = true;
          what  = "hugetlbfs";
          type  = "hugetlbfs";
          options = "pagesize=1G";
          requiredBy  = [ "basic.target" ];
      }
    ];
```

This sets up IOMMU passthrough mode and hugepages.

### To uninstall:
```
sudo modprobe -r tenstorrent
sudo dkms remove tenstorrent/1.26 --all
```

