// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "eth.h"
#include "wormhole.h"
#include "tlb.h"

#define ETH_TIMEOUT_MS 250
#define ETH_MIN_FW_VERSION 0x6069000

#define ETH_FW_VERSION_ADDR 0x210
#define ETH_PORT_STATUS_ADDR 0x1200
#define ETH_REMOTE_RACK_ADDR 0x11128
#define ETH_REMOTE_SHELF_ADDR 0x1124
#define ETH_REQ_WR_PTR_ADDR 0x110a0
#define ETH_REQ_RD_PTR_ADDR 0x110b0
#define ETH_REQ_QUEUE_ADDR 0x110c0
#define ETH_RESP_RD_PTR_ADDR 0x11230
#define ETH_RESP_WR_PTR_ADDR 0x11220
#define ETH_RESP_WR_PTR_ADDR 0x11220
#define ETH_RESP_QUEUE_ADDR 0x11240

#define ETH_CMD_WR_REQ (0x1 << 0)
#define ETH_CMD_WR_ACK (0x1 << 1)
#define ETH_CMD_RD_REQ (0x1 << 2)
#define ETH_CMD_RD_DATA (0x1 << 3)

#define ETH_STATUS_UNKNOWN 0
#define ETH_STATUS_NOT_CONNECTED 1

static const u32 WH_ETH_NOC0_X[WH_ETH_CORE_COUNT] = { 9, 1, 8, 2, 7, 3, 6, 4, 9, 1, 8, 2, 7, 3, 6, 4 };
static const u32 WH_ETH_NOC0_Y[WH_ETH_CORE_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 6, 6, 6, 6, 6, 6 };

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

static u32 eth_read_reg(struct wormhole_device *wh_dev, struct tlb_t *tlb, u32 eth_idx, u32 addr)
{
	return wh_read32(wh_dev, tlb, WH_ETH_NOC0_X[eth_idx], WH_ETH_NOC0_Y[eth_idx], addr);
}

static void eth_write_reg(struct wormhole_device *wh_dev, struct tlb_t *tlb, u32 eth_idx, u32 addr, u32 value)
{
	wh_write32(wh_dev, tlb, WH_ETH_NOC0_X[eth_idx], WH_ETH_NOC0_Y[eth_idx], addr, value);
}

static void eth_write_block(struct wormhole_device *wh_dev, struct tlb_t *tlb, u32 eth_idx, u32 addr, const void *src,
			    size_t size)
{
	wh_memcpy_toio(wh_dev, tlb, WH_ETH_NOC0_X[eth_idx], WH_ETH_NOC0_Y[eth_idx], addr, src, size);
}

static bool eth_queue_full(u32 wr, u32 rd)
{
	// The queues are 4 entries deep; valid pointer values are 0..7 inclusive.
	// The queue is full if the write pointer is 4 ahead of the read pointer.
	return (wr != rd) && ((wr & 3) == (rd & 3));
}

void wormhole_eth_probe(struct wormhole_device *wh_dev)
{
	struct tlb_t *tlb;
	u32 i;

	wh_dev->num_connected_cores = 0;
	tlb = tlb_alloc(&wh_dev->tlb_pool);

	if (!tlb)
		return;

	for (i = 0; i < WH_ETH_CORE_COUNT; i++) {
		struct connected_eth_core *core_info = &wh_dev->connected_eth_cores[wh_dev->num_connected_cores];
		u32 fw_version = eth_read_reg(wh_dev, tlb, i, ETH_FW_VERSION_ADDR);
		u32 port_status = eth_read_reg(wh_dev, tlb, i, ETH_PORT_STATUS_ADDR + (i * 4));
		u32 remote_rack = eth_read_reg(wh_dev, tlb, i, ETH_REMOTE_RACK_ADDR);
		u32 remote_shelf = eth_read_reg(wh_dev, tlb, i, ETH_REMOTE_SHELF_ADDR);

		if (fw_version < ETH_MIN_FW_VERSION)
			continue;

		if (port_status == ETH_STATUS_UNKNOWN || port_status == ETH_STATUS_NOT_CONNECTED)
			continue;

		core_info->core_num = i;
		core_info->remote_rack_x = (remote_rack >> 0) & 0xFF;
		core_info->remote_rack_y = (remote_rack >> 8) & 0xFF;
		core_info->remote_shelf_x = (remote_shelf >> 16) & 0x3F;
		core_info->remote_shelf_y = (remote_shelf >> 22) & 0x3F;
		core_info->remote_noc_x = (remote_shelf >> 4) & 0x3F;
		core_info->remote_noc_y = (remote_shelf >> 10) & 0x3F;

		wh_dev->num_connected_cores++;
	}

	tlb_free(&wh_dev->tlb_pool, tlb);
}

bool wormhole_eth_read32(struct wormhole_device *wh_dev, u32 eth_core, u64 sys_addr, u32 rack, u32 *value)
{
	struct eth_cmd_t cmd;
	struct tlb_t *tlb;
	unsigned long timeout;
	u32 req_rd, req_wr, resp_rd, resp_wr; // queue pointers; 0 <= ptr < 8
	u32 req_slot, resp_slot; // queue indices; 0 <= slot < 4
	u32 req_offset;
	u32 resp_offset;
	u32 resp_flags_offset;
	u32 resp_data_offset;
	u32 fw_version;
	u32 resp_flags = 0;

	tlb = tlb_alloc(&wh_dev->tlb_pool);
	if (!tlb)
		return false;

	fw_version = eth_read_reg(wh_dev, tlb, eth_core, ETH_FW_VERSION_ADDR);
	if (fw_version < ETH_MIN_FW_VERSION) {
		pr_err("ETH FW version: %u is too old.\n", fw_version);
		goto wormhole_eth_read32_done;
	}

	// Read the current position of the read and write pointers for both the
	// request and response queues.
	req_wr = eth_read_reg(wh_dev, tlb, eth_core, ETH_REQ_WR_PTR_ADDR);
	req_rd = eth_read_reg(wh_dev, tlb, eth_core, ETH_REQ_RD_PTR_ADDR);
	resp_wr = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_WR_PTR_ADDR);
	resp_rd = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_RD_PTR_ADDR);

	// Encode the command.
	memset(&cmd, 0, sizeof(struct eth_cmd_t));
	cmd.sys_addr = sys_addr;
	cmd.data = sizeof(u32);
	cmd.rack = rack;
	cmd.flags = ETH_CMD_RD_REQ;

	if (eth_queue_full(req_wr, req_rd)) {
		pr_err("ETH queue %u full\n", eth_core);
		goto wormhole_eth_read32_done;
	}

	// Write the request to the slot in the request queue.
	req_slot = req_wr & 3;
	req_offset = req_slot * sizeof(struct eth_cmd_t);
	eth_write_block(wh_dev, tlb, eth_core, ETH_REQ_QUEUE_ADDR + req_offset, &cmd, sizeof(struct eth_cmd_t));

	// Write the request write pointer.
	req_wr = (req_wr + 1) & 0x7;
	eth_write_reg(wh_dev, tlb, eth_core, ETH_REQ_WR_PTR_ADDR, req_wr);

	// UMD says,
	// 	erisc firmware will:
	// 1. clear response flags
	// 2. start operation
	// 3. advance response wrptr
	// 4. complete operation and write data into response or buffer
	// 5. set response flags

	// Busy wait until the response write pointer changes.
	timeout = jiffies + msecs_to_jiffies(ETH_TIMEOUT_MS);
	while (resp_wr == eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_WR_PTR_ADDR)) {
		if (time_after(jiffies, timeout)) {
			pr_err("ETH response timeout\n");
			break;
		}
	}

	// Busy wait until flags are set.
	resp_slot = resp_rd & 3; // 0 <= resp_slot < 4
	resp_offset = resp_slot * sizeof(struct eth_cmd_t);
	resp_flags_offset = resp_offset + offsetof(struct eth_cmd_t, flags);
	timeout = jiffies + msecs_to_jiffies(ETH_TIMEOUT_MS);
	while ((resp_flags = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_QUEUE_ADDR + resp_flags_offset)) == 0) {
		if (time_after(jiffies, timeout)) {
			pr_err("ETH response timeout\n");
			break;
		}
	}

	// Read the response.
	resp_data_offset = resp_offset + offsetof(struct eth_cmd_t, data);
	*value = eth_read_reg(wh_dev, tlb, eth_core, ETH_RESP_QUEUE_ADDR + resp_data_offset);

	// Increment/wrap/update the response read pointer.
	resp_rd = (resp_rd + 1) & 0x7;
	eth_write_reg(wh_dev, tlb, eth_core, ETH_RESP_RD_PTR_ADDR, resp_rd);

wormhole_eth_read32_done:
	tlb_free(&wh_dev->tlb_pool, tlb);

	// Response is only valid if we return true.
	return resp_flags == ETH_CMD_RD_DATA;
}
