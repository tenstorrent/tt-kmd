// SPDX-License-Identifier: GPL-2.0-only
/*
 * debugfs: /sys/kernel/debug/tenstorrent/<N>/mappings lists every open fd of
 * the device and the resources it holds. Subsystems add their own rows via
 * tt_<subsys>_debugfs_show() and tt_mappings_row().
 */
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pid.h>
#include <linux/stdarg.h>

#include "device.h"
#include "chardev.h"

struct dentry *tt_debugfs_root;

void tt_mappings_row(struct seq_file *s, struct chardev_private *priv,
		     const char *type, const char *fmt, ...)
{
	va_list args;

	seq_printf(s, "%-8d %-16s %-16s ", pid_vnr(priv->pid), priv->comm, type);
	va_start(args, fmt);
	seq_vprintf(s, fmt, args);
	va_end(args);
	seq_putc(s, '\n');
}

static void tt_debugfs_show_bars(struct seq_file *s, struct chardev_private *priv)
{
	struct tt_bar_mapping *bm;

	spin_lock(&priv->bar_lock);
	list_for_each_entry(bm, &priv->bar_mappings, node)
		tt_mappings_row(s, priv, "BAR",
				"BAR%u %s va 0x%lx offset 0x%llx size 0x%llx vmas %u",
				bm->bar, bm->mmio.wc ? "WC" : "UC", bm->mmio.va, bm->bar_offset,
				bm->mmio.size, kref_read(&bm->mmio.kref));
	spin_unlock(&priv->bar_lock);
}

static int tt_mappings_show(struct seq_file *s, void *unused)
{
	struct tenstorrent_device *dev = s->private;
	struct chardev_private *priv;

	seq_puts(s, "WARNING: This file is for diagnostic purposes only. "
		    "Its format is not stable and may change at any time.\n");
	seq_printf(s, "%-8s %-16s %-16s %s\n", "PID", "Comm", "Type", "Mapping Details");

	mutex_lock(&dev->open_lock);
	list_for_each_entry(priv, &dev->open_fds, open_fd_node) {
		if (!priv->live)
			continue;
		tt_mappings_row(s, priv, "OPEN_FD", "%s%s",
				priv->excl ? "O_EXCL" : "shared",
				priv->o_append ? " O_APPEND" : "");
		tt_debugfs_show_bars(s, priv);
		tt_memory_debugfs_show(s, priv);
		tt_tlb_debugfs_show(s, priv);
	}
	mutex_unlock(&dev->open_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tt_mappings);

void tt_debugfs_device_add(struct tenstorrent_device *dev)
{
	char name[16];

	if (IS_ERR_OR_NULL(tt_debugfs_root))
		return;

	snprintf(name, sizeof(name), "%d", dev->ordinal);
	dev->debugfs_dir = debugfs_create_dir(name, tt_debugfs_root);
	debugfs_create_file("mappings", 0444, dev->debugfs_dir, dev, &tt_mappings_fops);
}

void tt_debugfs_device_remove(struct tenstorrent_device *dev)
{
	/* Waits for readers of the files. */
	debugfs_remove(dev->debugfs_dir);
	dev->debugfs_dir = NULL;
}

void tt_debugfs_init(void)
{
	tt_debugfs_root = debugfs_create_dir(TT_DRIVER_NAME, NULL);
}

void tt_debugfs_exit(void)
{
	debugfs_remove(tt_debugfs_root);
	tt_debugfs_root = NULL;
}
