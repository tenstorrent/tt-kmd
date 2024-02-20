// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>

#include "eth.h"
#include "wormhole.h"

#define ETH_TIMEOUT_MS 250
#define ETH_MIN_FW_VERSION 0x6069000

#define ETH_FW_VERSION_ADDR 0x210
#define ETH_PORT_STATUS_ADDR 0x1200
#define ETH_NODE_INFO_ADDR 0x1100
#define ETH_LOCAL_RACK_SHELF_ADDR 0x1108
#define ETH_LOCAL_NOC0_X_ADDR 0x1110
#define ETH_LOCAL_NOC0_Y_ADDR 0x1118
#define ETH_REMOTE_RACK_ADDR 0x1128
#define ETH_REMOTE_SHELF_ADDR 0x1124
#define ETH_REQ_WR_PTR_ADDR 0x110a0
#define ETH_REQ_RD_PTR_ADDR 0x110b0
#define ETH_REQ_QUEUE_ADDR 0x110c0
#define ETH_RESP_RD_PTR_ADDR 0x11230
#define ETH_RESP_WR_PTR_ADDR 0x11220
#define ETH_RESP_QUEUE_ADDR 0x11240

#define ETH_CMD_WR_REQ (1 << 0)
#define ETH_CMD_WR_ACK (1 << 1)
#define ETH_CMD_RD_REQ (1 << 2)
#define ETH_CMD_RD_DATA (1 << 3)

#define ETH_STATUS_UNKNOWN 0
#define ETH_STATUS_NOT_CONNECTED 1

// NOC coordinates for each of the 16 ethernet cores in a Wormhole chip.
static const u8 WH_ETH_NOC0_X[WH_ETH_CORE_COUNT] = { 9, 1, 8, 2, 7, 3, 6, 4, 9, 1, 8, 2, 7, 3, 6, 4 };
static const u8 WH_ETH_NOC0_Y[WH_ETH_CORE_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 6, 6, 6, 6, 6, 6 };

/**
 * struct eth_cmd_t - Ethernet firmware command structure.
 * @sys_addr: Encodes a remote address.  Includes chip location on within the
 * rack (known as "shelf" or chip coordinates), plus the location of the memory
 * within the chip (NOC X/Y and local offset).
 * @data: Number of bytes to read or write.
 * @flags: Flags indicating the type of operation or status of the response.
 * @rack: Rack coordinates of the remote chip.
 */
struct eth_cmd_t {
	uint64_t sys_addr;
	uint32_t data;
	uint32_t flags;
	uint16_t rack;
	uint16_t src_resp_buf_index;
	uint32_t local_buf_index;
	uint8_t src_resp_q_id;
	uint8_t host_mem_txn_id;
	uint16_t padding;
	uint32_t src_addr_tag;
};

/**
 * struct sys_addr_t - Encode a remote address for an Ethernet command.
 * @offset: Local offset within the remote chip's core.
 * @noc_x: X NOC coordinate of the core on the remote chip.
 * @noc_y: Y NOC coordinate of the core on the remote chip.
 * @chip_x: X coordinate of the chip on the remote rack.
 * @chip_y: Y coordinate of the chip on the remote rack.
 *
 * This structure is Wormhole-specific and does not encode rack X/Y coordinates.
 */
struct sys_addr_t {
	union {
		u64 encoded;
		struct {
			u64 offset : 36;
			u64 noc_x : 6;
			u64 noc_y : 6;
			u64 chip_x : 6;
			u64 chip_y : 6;
			u64 reserved : 4;
		};
	};
};

static bool eth_queue_full(u32 wr, u32 rd)
{
	// The queues are 4 entries deep; valid pointer values are 0..7 inclusive.
	// The queue is full if the write pointer is 4 ahead of the read pointer.
	return (wr != rd) && ((wr & 3) == (rd & 3));
}

void wormhole_eth_probe(struct wormhole_device *wh_dev)
{
	struct tenstorrent_device *tt = &wh_dev->tt;
	u32 eth_channel;

	wh_dev->num_connected_cores = 0;

	for (eth_channel = 0; eth_channel < WH_ETH_CORE_COUNT; eth_channel++) {
		struct connected_eth_core *core_info = &wh_dev->connected_eth_cores[wh_dev->num_connected_cores];
		u32 x = WH_ETH_NOC0_X[eth_channel];
		u32 y = WH_ETH_NOC0_Y[eth_channel];
		u32 fw_version = wh_noc_read32(tt, x, y, ETH_FW_VERSION_ADDR);
		u32 port_status = wh_noc_read32(tt, x, y, ETH_PORT_STATUS_ADDR + (eth_channel * 4));
		u32 remote_rack = wh_noc_read32(tt, x, y, ETH_REMOTE_RACK_ADDR);
		u32 remote_shelf = wh_noc_read32(tt, x, y, ETH_REMOTE_SHELF_ADDR);
		u32 rack_shelf = wh_noc_read32(tt, x, y, ETH_LOCAL_RACK_SHELF_ADDR);

		if (fw_version < ETH_MIN_FW_VERSION) {
			dev_info(&tt->pdev->dev, "ETH FW version: %u is too old.\n", fw_version);
			// Make the assumption that all ETH cores are running the same FW.
			return;
		}

		if (port_status == ETH_STATUS_UNKNOWN || port_status == ETH_STATUS_NOT_CONNECTED)
			continue;

		core_info->eth_channel = eth_channel;
		core_info->fw_version = fw_version;

		core_info->local.rack_x = (rack_shelf >> 0) & 0xFF;
		core_info->local.rack_y = (rack_shelf >> 8) & 0xFF;
		core_info->local.shelf_x = (rack_shelf >> 16) & 0xFF;
		core_info->local.shelf_y = (rack_shelf >> 24) & 0xFF;
		core_info->local_noc_x = x;
		core_info->local_noc_y = y;

		core_info->remote.rack_x = (remote_rack >> 0) & 0xFF;
		core_info->remote.rack_y = (remote_rack >> 8) & 0xFF;
		core_info->remote.shelf_x = (remote_shelf >> 16) & 0x3F;
		core_info->remote.shelf_y = (remote_shelf >> 22) & 0x3F;
		core_info->remote_noc_x = (remote_shelf >> 4) & 0x3F;
		core_info->remote_noc_y = (remote_shelf >> 10) & 0x3F;

		wh_dev->num_connected_cores++;
	}
}

bool wormhole_remote_read32(struct wormhole_device *wh_dev, u32 eth_channel, struct eth_addr_t *eth_addr, u32 noc_x,
			    u32 noc_y, u64 addr, u32 *value)
{
	struct tenstorrent_device *tt = &wh_dev->tt;
	struct sys_addr_t sys_addr = { 0 };
	struct eth_cmd_t cmd;
	unsigned long timeout;
	u32 x = WH_ETH_NOC0_X[eth_channel];
	u32 y = WH_ETH_NOC0_Y[eth_channel];
	u32 req_rd, req_wr, resp_rd, resp_wr; // queue pointers; 0 <= ptr < 8
	u32 req_slot, resp_slot; // queue indices; 0 <= slot < 4
	u32 req_offset, resp_offset;
	u32 resp_flags_offset, resp_data_offset;
	u32 resp_flags = 0;

	// Read the current position of the read and write pointers for both the
	// request and response queues.
	req_wr = wh_noc_read32(tt, x, y, ETH_REQ_WR_PTR_ADDR);
	req_rd = wh_noc_read32(tt, x, y, ETH_REQ_RD_PTR_ADDR);
	resp_wr = wh_noc_read32(tt, x, y, ETH_RESP_WR_PTR_ADDR);
	resp_rd = wh_noc_read32(tt, x, y, ETH_RESP_RD_PTR_ADDR);

	// Encode the command.
	memset(&cmd, 0, sizeof(struct eth_cmd_t));
	sys_addr.chip_x = eth_addr->shelf_x;
	sys_addr.chip_y = eth_addr->shelf_y;
	sys_addr.noc_x = noc_x;
	sys_addr.noc_y = noc_y;
	sys_addr.offset = addr;
	cmd.sys_addr = sys_addr.encoded;
	cmd.data = sizeof(u32);
	cmd.rack = ((u16)eth_addr->rack_y << 8) | eth_addr->rack_x;
	cmd.flags = ETH_CMD_RD_REQ;

	if (eth_queue_full(req_wr, req_rd)) {
		dev_err_ratelimited(&tt->pdev->dev, "ETH queue %u full\n", eth_channel);
		goto wormhole_eth_read32_done;
	}

	// Write the request to the slot in the request queue.
	req_slot = req_wr & 3;
	req_offset = req_slot * sizeof(struct eth_cmd_t);
	wh_noc_write_block(tt, x, y, ETH_REQ_QUEUE_ADDR + req_offset, &cmd, sizeof(struct eth_cmd_t));

	// Write the request write pointer.
	req_wr = (req_wr + 1) & 0x7;
	wh_noc_write32(tt, x, y, ETH_REQ_WR_PTR_ADDR, req_wr);

	// UMD says,
	// 	erisc firmware will:
	// 1. clear response flags
	// 2. start operation
	// 3. advance response wrptr
	// 4. complete operation and write data into response or buffer
	// 5. set response flags
	// Wait until the response write pointer changes.
	timeout = jiffies + msecs_to_jiffies(ETH_TIMEOUT_MS);
	while (resp_wr == wh_noc_read32(tt, x, y, ETH_RESP_WR_PTR_ADDR)) {
		usleep_range(1, 2);
		if (time_after(jiffies, timeout)) {
			dev_err(&tt->pdev->dev, "ETH response timeout\n");
			break;
		}
	}

	// Busy wait until flags are set.
	resp_slot = resp_rd & 3; // 0 <= resp_slot < 4
	resp_offset = resp_slot * sizeof(struct eth_cmd_t);
	resp_flags_offset = resp_offset + offsetof(struct eth_cmd_t, flags);
	timeout = jiffies + msecs_to_jiffies(ETH_TIMEOUT_MS);
	while ((resp_flags = wh_noc_read32(tt, x, y, ETH_RESP_QUEUE_ADDR + resp_flags_offset)) == 0) {
		// Short sleep to yield the CPU.  Based on empirical measurements, the
		// operation takes ~16 to ~24 usec to complete.
		usleep_range(1, 8);

		if (time_after(jiffies, timeout)) {
			dev_err(&tt->pdev->dev, "ETH response timeout\n");
			break;
		}
	}

	// Read the response.
	resp_data_offset = resp_offset + offsetof(struct eth_cmd_t, data);
	*value = wh_noc_read32(tt, x, y, ETH_RESP_QUEUE_ADDR + resp_data_offset);

	// Increment/wrap/update the response read pointer.
	resp_rd = (resp_rd + 1) & 0x7;
	wh_noc_write32(tt, x, y, ETH_RESP_RD_PTR_ADDR, resp_rd);

wormhole_eth_read32_done:

	// Response is only valid if we return true.
	return resp_flags == ETH_CMD_RD_DATA;
}
