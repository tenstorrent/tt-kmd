// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_WORMHOLE_H_INCLUDED
#define TTDRIVER_WORMHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"
#include "eth.h"

struct wormhole_device {
	struct tenstorrent_device tt;
	u8 __iomem *bar2_mapping;
	u8 __iomem *bar4_mapping;

	struct connected_eth_core connected_eth_cores[WH_ETH_CORE_COUNT];
	u32 num_connected_cores;

	struct mutex tlb_mutex;
};

#define tt_dev_to_wh_dev(ttdev) \
	container_of((tt_dev), struct wormhole_device, tt)

/**
 * wh_noc_read32() - Read a 32-bit value from a core on a Wormhole chip.
 * @tt_dev: Handle to the Wormhole device.
 * @x: NOC X coordinate of the core.
 * @y: NOC Y coordinate of the core.
 * @addr: Address within the core to read from.
 *
 * Context: Holds the wh_dev->tlb_mutex throughout its execution to ensure
 * exclusive access to the driver's TLB.
 *
 * Return: The 32-bit value read from the core.
 */
u32 wh_noc_read32(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr);

/**
 * wh_noc_write32() - Write a 32-bit value to a core on a Wormhole chip.
 * Similar to wh_noc_read32(), but writes a value instead of reading.
 * @val: The 32-bit value to write.
 *
 * See wh_noc_read32() for more details on parameters and context.
 */
void wh_noc_write32(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, u32 val);

/**
 * wh_noc_write_block() - Write a block of data to a core on a Wormhole chip.
 * Similar to wh_noc_write32(), but operates on a block of data.
 * @src: Pointer to the data to write.
 * @size: Size of the data to write.
 *
 * See wh_noc_read32() for more details on parameters and context.
 */
void wh_noc_write_block(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, const void *src, size_t size);

#endif
