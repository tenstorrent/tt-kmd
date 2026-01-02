# Sysfs Device Attributes

This document describes the sysfs attributes exposed by the Tenstorrent
Kernel-Mode Driver (tt-kmd) for monitoring and interacting with Tenstorrent
hardware.

## Accessing Device Attributes

Tenstorrent devices expose attributes through the sysfs filesystem. The primary
and most straightforward way to access these is via the class device path:

* `/sys/class/tenstorrent/tenstorrent!<N>/`

Where `tenstorrent!<N>` corresponds to the specific logical Tenstorrent device
instance (e.g., `tenstorrent!0` for the device that appears as
`/dev/tenstorrent/0`). The exclamation mark `!` is part of the directory name.

These attributes are also accessible via a path linked to the underlying PCI
device structure:

* `/sys/bus/pci/devices/<YYYY:XX:ZZ.Z>/tenstorrent/tenstorrent!<N>/`

Where `<YYYY:XX:ZZ.Z>` is the PCI domain, bus, slot, and function of the
Tenstorrent device.

**Note on Shell Interaction**: When navigating to these directories in a shell,
the exclamation mark `!` may need to be escaped (e.g. `cd tenstorrent\!0/`) or
quoted (e.g. `cd 'tenstorrent!0/'`) depending on your shell, as `!` is often a
special character for history expansion.

---

## Telemetry Attributes

Telemetry attributes expose data provided by the on-chip ARC firmware. These
attributes are read directly from the device's telemetry region and provide
information about clock speeds, firmware versions, device identity, and health.

**Location**: These attributes are found directly within each Tenstorrent
device's sysfs entry (not in a subdirectory).

**Primary Path Structure**:
`/sys/class/tenstorrent/tenstorrent!<N>/tt_*`

### Availability

Not all attributes are available on all devices or firmware versions. Attributes
will only appear in sysfs if the firmware reports the corresponding telemetry
tag. If an attribute is missing, the firmware on that device does not support
it.

### Available Attributes

| sysfs Filename       | Description                                           | Device Support |
|----------------------|-------------------------------------------------------|----------------|
| `tt_aiclk`           | Current AI clock frequency in MHz                     | BH, WH         |
| `tt_axiclk`          | Current AXI clock frequency in MHz                    | BH, WH         |
| `tt_arcclk`          | Current ARC clock frequency in MHz                    | BH, WH         |
| `tt_serial`          | Board serial number (hex)                             | BH, WH         |
| `tt_card_type`       | Card type identifier (e.g., "p150a", "n150")          | BH, WH         |
| `tt_asic_id`         | ASIC identifier (hex)                                 | BH, WH         |
| `tt_fw_bundle_ver`   | Firmware bundle version                               | BH, WH         |
| `tt_m3app_fw_ver`    | M3 application firmware version                       | BH, WH         |
| `tt_m3bl_fw_ver`     | M3 bootloader firmware version                        | WH             |
| `tt_arc_fw_ver`      | ARC firmware version                                  | WH             |
| `tt_eth_fw_ver`      | Ethernet firmware version                             | WH             |
| `tt_ttflash_ver`     | tt-flash version                                      | WH             |
| `tt_heartbeat`       | Firmware heartbeat counter (changing = alive)         | BH, WH         |
| `tt_therm_trip_count`| ASIC shutdowns due to critical temperature (since power cycle) | BH             |

### Example Usage

To check if firmware is alive by reading the heartbeat counter:

```bash
cat '/sys/class/tenstorrent/tenstorrent!0/tt_heartbeat'
```

To check for thermal events:

```bash
cat '/sys/class/tenstorrent/tenstorrent!0/tt_therm_trip_count'
```

---

## PCIe Performance Counters

A set of read-only performance counters related to PCIe and NOC (Network on
Chip) activity are exposed for each supported device. These counters can be used
to monitor data movement and diagnose performance.

**Location**: These counters are found in the `pcie_perf_counters` subdirectory
within each Tenstorrent device's sysfs entry.

**Primary Path Structure**:
`/sys/class/tenstorrent/tenstorrent!<N>/pcie_perf_counters/`

**Alternative PCI-Specific Path Structure**:
`/sys/bus/pci/devices/<YYYY:XX:ZZ.Z>/tenstorrent/tenstorrent!<N>/pcie_perf_counters/`

**Path Components**:
* `<N>`: The instance number of the Tenstorrent device (e.g., `0`, `1`).
* Exclamation mark `!` is part of the `tenstorrent!<N>` directory name.
* `<YYYY:XX:ZZ.Z>`: The PCI domain, bus, slot, and function of the device.

**Full Path Example:**

`/sys/class/tenstorrent/tenstorrent!0/pcie_perf_counters/`

### General Notes:

* All counters are 32-bit unsigned integers and are cumulative, meaning they
increment from the last hardware reset.
* These counters will wrap around upon reaching their maximum 32-bit value. To
measure rates or activity over an interval, userspace tools should poll the
counters and calculate deltas, accounting for potential wraps.
* There is no software mechanism exposed via sysfs to reset the counters.
* Counters are read directly from a memory-mapped BAR segment. This is a
lightweight operation

### Available Counters:

The following performance counters are available. Each counter:
* Is an **unsigned 32-bit integer**.
* Counts events in units of 32 byte flits.
* Is available for both NOC instances

| sysfs Filename                          | Description                                                                 | Device Support |
|-----------------------------------------|-----------------------------------------------------------------------------|----------------|
| `mst_rd_data_word_received0`            | Master: Read data words received by the PCIe interface (NOC0)          | BH, WH         |
| `mst_rd_data_word_received1`            | Master: Read data words received by the PCIe interface (NOC1)          | BH, WH         |
| `mst_nonposted_wr_data_word_sent0`      | Master: Non-posted write data words sent by the PCIe interface (NOC0)  | BH, WH         |
| `mst_nonposted_wr_data_word_sent1`      | Master: Non-posted write data words sent by the PCIe interface (NOC1)  | BH, WH         |
| `mst_posted_wr_data_word_sent0`         | Master: Posted write data words sent by the PCIe interface (NOC0)      | BH, WH         |
| `mst_posted_wr_data_word_sent1`         | Master: Posted write data words sent by the PCIe interface (NOC1)      | BH, WH         |
| `slv_nonposted_wr_data_word_received0`  | Slave: Non-posted write data words received by the PCIe interface (NOC0) | BH, WH         |
| `slv_nonposted_wr_data_word_received1`  | Slave: Non-posted write data words received by the PCIe interface (NOC1) | BH, WH         |
| `slv_posted_wr_data_word_received0`     | Slave: Posted write data words received by the PCIe interface (NOC0)   | BH, WH         |
| `slv_posted_wr_data_word_received1`     | Slave: Posted write data words received by the PCIe interface (NOC1)   | BH, WH         |
| `slv_rd_data_word_sent0`                | Slave: Read data words sent by the PCIe interface (NOC0)               | BH, WH         |
| `slv_rd_data_word_sent1`                | Slave: Read data words sent by the PCIe interface (NOC1)               | BH, WH         |

### Example Usage:

To read the number of posted write data words sent by the master interface via NOC0 on device instance 0:

```bash
cat '/sys/class/tenstorrent/tenstorrent!0/pcie_perf_counters/mst_posted_wr_data_word_sent0'
```
