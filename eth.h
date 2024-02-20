// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_ETH_H_INCLUDED
#define TTDRIVER_ETH_H_INCLUDED

#include <linux/types.h>

#define WH_ETH_CORE_COUNT 16

struct wormhole_device;

struct connected_eth_core {
    u32 core_num;
    u32 fw_version;
    u32 remote_rack_x;
    u32 remote_rack_y;
    u32 remote_shelf_x;
    u32 remote_shelf_y;
    u32 remote_noc_x;
    u32 remote_noc_y;
};

void wormhole_eth_probe(struct wormhole_device *wh_dev);
bool wormhole_eth_read32(struct wormhole_device *wh_dev, u32 eth_core, u64 sys_addr, u32 rack, u32 *value);

#endif
