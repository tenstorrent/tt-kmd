// SPDX-License-Identifier: GPL-2.0-only
/* procfs: /proc/driver/tenstorrent/<N>/pids, one PID per open fd per line. */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/pid.h>

#include "device.h"

struct proc_dir_entry *tt_procfs_root;

static int tt_pids_show(struct seq_file *s, void *unused)
{
	struct tenstorrent_device *dev = s->private;
	struct chardev_private *priv;

	mutex_lock(&dev->open_lock);
	list_for_each_entry(priv, &dev->open_fds, open_fd_node) {
		pid_t pid = pid_vnr(priv->pid);

		if (priv->live && pid > 0)	/* 0: not visible in the reader's namespace */
			seq_printf(s, "%d\n", pid);
	}
	mutex_unlock(&dev->open_lock);
	return 0;
}

void tt_procfs_device_add(struct tenstorrent_device *dev)
{
	char name[16];

	if (!tt_procfs_root)
		return;

	snprintf(name, sizeof(name), "%d", dev->ordinal);
	dev->procfs_dir = proc_mkdir(name, tt_procfs_root);
	if (!dev->procfs_dir)
		return;
	if (!proc_create_single_data("pids", 0444, dev->procfs_dir, tt_pids_show, dev)) {
		proc_remove(dev->procfs_dir);
		dev->procfs_dir = NULL;
	}
}

void tt_procfs_device_remove(struct tenstorrent_device *dev)
{
	/* Removes the subtree and waits for readers. */
	proc_remove(dev->procfs_dir);
	dev->procfs_dir = NULL;
}

int tt_procfs_init(void)
{
	tt_procfs_root = proc_mkdir("driver/" TT_DRIVER_NAME, NULL);
	return tt_procfs_root ? 0 : -ENOMEM;
}

void tt_procfs_exit(void)
{
	proc_remove(tt_procfs_root);
	tt_procfs_root = NULL;
}
