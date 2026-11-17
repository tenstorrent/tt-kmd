/* SPDX-License-Identifier: GPL-2.0-only */
/* hwmon device fed from firmware telemetry. */
#ifndef TT_HWMON_H
#define TT_HWMON_H

struct tenstorrent_device;

/* Device init (software only): the read cache. */
void tt_hwmon_setup(struct tenstorrent_device *dev);

/* Register the hwmon device under the PCI device if telemetry is available.
 * Failure is logged and not fatal. */
void tt_hwmon_init(struct tenstorrent_device *dev);
void tt_hwmon_fini(struct tenstorrent_device *dev);

#endif
