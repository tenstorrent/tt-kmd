// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_ETH_H_INCLUDED
#define TTDRIVER_ETH_H_INCLUDED

#include <linux/types.h>

#define WH_ETH_CORE_COUNT 16

struct wormhole_device;

/**
 * struct eth_addr_t - Ethernet address; unique for each chip in a topology.
 */
struct eth_addr_t {
	u8 rack_x;
	u8 rack_y;
	u8 shelf_x;
	u8 shelf_y;
};

/**
 * struct connected_eth_core - State of a connected ethernet core.
 * @eth_channel: Ethernet channel number.  0 <= eth_channel < WH_ETH_CORE_COUNT.
 * @fw_version: Firmware version of the core.
 * @local: Local ethernet address.
 * @remote: Remote ethernet address.
 * @remote_noc_x: NOC X coordinate of the remote ETH core.
 * @remote_noc_y: NOC Y coordinate of the remote ETH core.
 * @local_noc_x: NOC X coordinate of the local ETH core.
 * @local_noc_y: NOC Y coordinate of the local ETH core.
 */
struct connected_eth_core {
	u32 eth_channel;
	u32 fw_version;

	struct eth_addr_t local;
	struct eth_addr_t remote;

	u32 remote_noc_x;
	u32 remote_noc_y;

	u32 local_noc_x;
	u32 local_noc_y;
};

/**
 * wormhole_eth_probe() - Determine which ethernet cores are connected.
 * @wh_dev: Wormhole device.
 *
 * Populates wh_dev->num_connected_cores and wh_dev->connected_eth_cores.
 *
 * Context: Expects wh_dev->tlb_mutex to be held.
 */
void wormhole_eth_probe(struct wormhole_device *wh_dev);

/**
 * wormhole_remote_read32() - Read a 32-bit value from a remote chip.
 * @wh_dev: Wormhole device.
 * @eth_channel: Ethernet channel number.  0 <= eth_channel < WH_ETH_CORE_COUNT.
 * @eth_addr: Ethernet address of the remote chip.
 * @noc_x: NOC X coordinate for the desired core in the remote chip.
 * @noc_y: NOC Y coordinate for the desired core in the remote chip.
 * @addr: Address within the remote core's memory space.
 * @value: Pointer to a variable to store the read value.
 *
 * Context: Expects wh_dev->tlb_mutex to be held.
 * Return: True if the read was successful, false otherwise.
 */
bool wormhole_remote_read32(struct wormhole_device *wh_dev, u32 eth_channel, struct eth_addr_t *eth_addr, u32 noc_x,
			    u32 noc_y, u64 addr, u32 *value);

#endif
