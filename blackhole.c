// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/types.h>

#include "blackhole.h"
#include "pcie.h"
#include "module.h"

static bool blackhole_init(struct tenstorrent_device *tt_dev) {
	return true;
}

static bool blackhole_init_hardware(struct tenstorrent_device *tt_dev) {
	return true;
}

static bool blackhole_post_hardware_init(struct tenstorrent_device *tt_dev) {
	return true;
}

static void blackhole_cleanup_hardware(struct tenstorrent_device *tt_dev) {
}

static void blackhole_cleanup(struct tenstorrent_device *tt_dev) {
}

struct tenstorrent_device_class blackhole_class = {
	.name = "Blackhole",
	.instance_size = sizeof(struct blackhole_device),
	.init_device = blackhole_init,
	.init_hardware = blackhole_init_hardware,
	.post_hardware_init = blackhole_post_hardware_init,
	.cleanup_hardware = blackhole_cleanup_hardware,
	.cleanup_device = blackhole_cleanup,
};
