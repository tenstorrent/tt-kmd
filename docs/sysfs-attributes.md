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

**Note on Shell Interaction**:
When navigating to these directories in a shell, the exclamation mark `!` may
need to be escaped (e.g. `cd tenstorrent\!0/`) or quoted (e.g. `cd
'tenstorrent!0/'`) depending on your shell, as `!` is often a special character
for history expansion.

### General Notes:

* All counters are 32-bit unsigned integers and are cumulative, meaning they
increment from the last hardware reset.
* These counters will wrap around upon reaching their maximum 32-bit value. To
measure rates or activity over an interval, userspace tools should poll the
counters and calculate deltas, accounting for potential wraps.
* There is no software mechanism exposed via sysfs to reset the counters.

### Device-Specific Notes:

* **Blackhole**: Counters are read directly from a memory-mapped BAR segment.
This is a lightweight operation.
* **Wormhole**: Due to a hardware limitation preventing direct BAR access to
these specific NIU (NOC Interface Unit) registers, counters are read indirectly
by programming a NOC TLB window.
    * **Important**: Each read operation from a Wormhole `pcie_perf_counters`
    sysfs file generates NOC traffic and involves TLB reconfiguration. Frequent
    polling of these counters on Wormhole devices can introduce overhead and
    potentially affect overall system performance. This indirect access is a
    workaround for the hardware issue.

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

