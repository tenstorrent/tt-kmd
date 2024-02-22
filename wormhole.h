// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_WORMHOLE_H_INCLUDED
#define TTDRIVER_WORMHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"
#include "eth.h"
#include "tlb.h"

struct wormhole_device {
	struct tenstorrent_device tt;
	u8 __iomem *bar0_mapping;
	u8 __iomem *bar2_mapping;
	u8 __iomem *bar4_mapping;

	struct connected_eth_core connected_eth_cores[WH_ETH_CORE_COUNT];
	u32 num_connected_cores;

	struct tlb_pool tlb_pool;
};

#define tt_dev_to_wh_dev(ttdev) \
	container_of((tt_dev), struct wormhole_device, tt)


bool wormhole_noc_write32(struct tenstorrent_device *tt_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr, u32 val);
bool wormhole_noc_read32(struct tenstorrent_device *tt_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr, u32 *val);
bool wormhole_noc_write(struct tenstorrent_device *tt_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr,
				const void *src, size_t size);

#endif
