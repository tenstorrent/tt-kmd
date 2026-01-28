// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "grendel.h"
#include "module.h"

static bool grendel_init(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Grendel init_device\n");
	return true;
}

static bool grendel_init_hardware(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Grendel init_hardware (stub)\n");
	return true;
}

static bool grendel_init_telemetry(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Grendel init_telemetry (stub)\n");
	return true;
}

static void grendel_save_reset_state(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Grendel save_reset_state (stub)\n");
}

static void grendel_cleanup_hardware(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Grendel cleanup_hardware (stub)\n");
}

static void grendel_cleanup(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Grendel cleanup_device (stub)\n");
}

static int grendel_set_power_state(struct tenstorrent_device *tt_dev,
				   struct tenstorrent_power_state *power_state)
{
	dev_info(&tt_dev->pdev->dev, "Grendel set_power_state (stub)\n");
	return 0;
}

struct tenstorrent_device_class grendel_class = {
	.name = "Grendel",
	.instance_size = sizeof(struct grendel_device),
	.dma_address_bits = 64,
	.init_device = grendel_init,
	.init_hardware = grendel_init_hardware,
	.save_reset_state = grendel_save_reset_state,
	.cleanup_hardware = grendel_cleanup_hardware,
	.cleanup_device = grendel_cleanup,
	.set_power_state = grendel_set_power_state,
	.init_telemetry = grendel_init_telemetry,
};
