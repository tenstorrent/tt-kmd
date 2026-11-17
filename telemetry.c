// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware telemetry: the table at SCRATCH_RAM[13] lists {tag, offset}
 * pairs; the value of a tag is the u32 at SCRATCH_RAM[12] + 4 * offset.
 * The tag list only changes with a firmware update + chip reset, so it is
 * read once, by the SMC late-init work once the firmware has booted.
 */
#include <linux/errno.h>
#include <linux/slab.h>

#include "device.h"
#include "blackhole.h"
#include "telemetry.h"

#define TELEM_DATA_PTR		BH_ARC_SCRATCH_RAM(12)
#define TELEM_TABLE_PTR		BH_ARC_SCRATCH_RAM(13)
#define TELEM_CSM_START		0x10000000u
#define TELEM_CSM_END		0x10080000u
#define TELEM_MAX_ENTRIES	1024
#define TELEM_VERSION_MAJOR(v)	(((v) >> 16) & 0xff)

static bool telem_csm_ok(u32 addr, u32 len)
{
	return !(addr & 3) && addr >= TELEM_CSM_START && addr < TELEM_CSM_END &&
	       len <= TELEM_CSM_END - addr;
}

static int telem_read32(struct tenstorrent_device *dev, u64 addr, u32 *val)
{
	return tt_noc_read32(dev, 0, BH_ARC_X, BH_ARC_Y, addr, val);
}

void tt_telemetry_init(struct tenstorrent_device *dev)
{
	struct tt_telemetry_state *t = &dev->smc.telemetry;
	u32 table, data, hdr[2], count, i, max_offset = 0;
	u32 *entries = NULL;
	int ret;

	t->valid = false;
	for (i = 0; i < TT_TELEMETRY_MAX_TAG; i++)
		t->offset[i] = TT_TELEMETRY_NO_OFFSET;

	ret = telem_read32(dev, TELEM_TABLE_PTR, &table);
	if (!ret)
		ret = telem_read32(dev, TELEM_DATA_PTR, &data);
	if (ret)
		return;
	if (!telem_csm_ok(table, sizeof(hdr)) || !telem_csm_ok(data, 4))
		goto unavailable;

	ret = tt_noc_read_block(dev, 0, BH_ARC_X, BH_ARC_Y, table, hdr, sizeof(hdr));
	if (ret)
		return;
	count = hdr[1];
	/* Only major version 0 is known (0x00000100 = v0.1.0 everywhere). */
	if (TELEM_VERSION_MAJOR(hdr[0]) != 0 || !count || count > TELEM_MAX_ENTRIES ||
	    !telem_csm_ok(table + 8, count * 4))
		goto unavailable;

	entries = kmalloc_array(count, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return;
	ret = tt_noc_read_block(dev, 0, BH_ARC_X, BH_ARC_Y, table + 8, entries, count * 4);
	if (ret)
		goto out;

	for (i = 0; i < count; i++) {
		u16 tag = entries[i] & 0xffff;
		u16 offset = entries[i] >> 16;

		/* Unfilled slots are {0, 0}; telemetry[0] is never written. */
		if (!tag || tag >= TT_TELEMETRY_MAX_TAG)
			continue;
		t->offset[tag] = offset;
		max_offset = max_t(u32, max_offset, offset);
	}
	if (!telem_csm_ok(data, (max_offset + 1) * 4))
		goto unavailable;

	t->data_addr = data;
	t->valid = true;
out:
	kfree(entries);
	return;

unavailable:
	kfree(entries);
	dev_info(&dev->pdev->dev, "no usable telemetry table\n");
}

bool tt_telemetry_has(struct tenstorrent_device *dev, unsigned int tag)
{
	const struct tt_telemetry_state *t = &dev->smc.telemetry;

	return t->valid && tag < TT_TELEMETRY_MAX_TAG && t->offset[tag] != TT_TELEMETRY_NO_OFFSET;
}

int tt_telemetry_read(struct tenstorrent_device *dev, unsigned int tag, u32 *val)
{
	const struct tt_telemetry_state *t = &dev->smc.telemetry;

	if (!t->valid)
		return -ENODEV;
	if (!tt_telemetry_has(dev, tag))
		return -ENOENT;
	return telem_read32(dev, t->data_addr + 4ULL * t->offset[tag], val);
}
