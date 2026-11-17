/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Core data structures of the Tenstorrent Blackhole driver.
 *
 * Lifetime
 * --------
 * struct tenstorrent_device is reference counted (kref). The PCI probe holds
 * one reference, every open fd (struct chardev_private) holds one. PCI remove
 * marks the device detached, tears the hardware down and drops the probe
 * reference; the struct is freed when the last fd is released.
 *
 * Locking
 * -------
 * - dev->detach_srcu: ioctls, mmap, open and the subsystem release hooks run
 *   inside an SRCU read section (tt_dev_enter()/tt_dev_exit()). PCI remove
 *   sets dev->detached and then waits for all sections (synchronize_srcu)
 *   before tearing anything down. So: if code inside a section saw
 *   !READ_ONCE(dev->detached) (tt_dev_enter() checks it; release hooks must
 *   check it themselves before touching hardware), the BARs and all device
 *   state stay valid until the section ends. SRCU readers never block, so
 *   this nests safely inside mmap_lock and around user copies.
 * - dev->open_lock (mutex): dev->open_fds, dev->open_count, dev->excl_open
 *   and priv->live. Held while debugfs/procfs walk the fd list, while the
 *   subsystem debugfs show hooks run and while remove calls the fd_detach
 *   hooks. Lock order: open_lock -> subsystem per-fd locks -> priv->bar_lock.
 *   Subsystem code must never take open_lock while holding its own locks.
 * - dev->kernel_tlb_mutex: the kernel-reserved TLB window (blackhole.c).
 * - dev->resource_lock (spinlock): the LOCK_CTL locks.
 *
 * Per-fd lifecycle (chardev.c) and the subsystem hooks
 * ----------------------------------------------------
 *  1. kzalloc, then tt_X_fd_init(priv) for every subsystem: non-failing,
 *     software only (init lists/locks), so per-fd state is valid from here on.
 *  2. O_EXCL admission: priv goes on dev->open_fds (still !priv->live).
 *  3. In a detach section: tt_X_fd_open(priv) (may fail; undone with
 *     tt_X_fd_release in reverse), then tt_power_fd_open(priv, o_append).
 *  4. priv->live = true (under open_lock). debugfs/procfs show only live fds.
 *  ... ioctls, mmaps ...
 *  5. release: priv->live = false (under open_lock); then, in a detach
 *     section, tt_X_fd_release(priv) (NOC cleanup first, power last);
 *     priv leaves dev->open_fds last.
 *
 * PCI remove, after setting dev->detached and waiting for every section:
 * zaps all user mappings, then, with open_lock held, calls tt_X_fd_detach(priv)
 * for every fd still on dev->open_fds: tear down everything that uses the
 * device (DMA mappings, iATU, exports) because the PCI device is gone once
 * remove returns. Then tt_X_device_fini(dev). Consequences for subsystems:
 *  - fd_detach can run concurrently with fd_release of the same fd (an fd
 *    stays on the list until its release hooks finished), and fd_release can
 *    run concurrently with device_fini. Protect per-fd state with the
 *    subsystem's own per-fd lock; whichever runs first tears down, the other
 *    finds nothing left.
 *  - fd_release after detach (READ_ONCE(dev->detached)) must not touch the
 *    hardware or the DMA API; it only frees software state.
 */
#ifndef TT_DEVICE_H
#define TT_DEVICE_H

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/srcu.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/pci.h>
#include <linux/sched.h>

#include "ioctl.h"
#include "memory.h"
#include "tlb.h"
#include "noc.h"
#include "smc.h"
#include "power.h"

#define TT_DRIVER_NAME		"tenstorrent"
#define TT_DRIVER_VERSION_MAJOR	2
#define TT_DRIVER_VERSION_MINOR	0
#define TT_DRIVER_VERSION_PATCH	0
#define TT_DRIVER_VERSION_STR	"2.0.0"	/* must match the three numbers above */

#define TT_MAX_DEVICES		64

#define PCI_VENDOR_ID_TENSTORRENT	0x1e52
#define PCI_DEVICE_ID_BLACKHOLE		0xb140

/*
 * mmap offset layout (byte offsets; vm_pgoff << PAGE_SHIFT)
 * ---------------------------------------------------------
 * Everything stays below 2^44 so that offsets fit a 32-bit mmap2 pgoff.
 *
 *   [0x000_0000_0000, 0x010_0000_0000)  BAR0 UC     (RESOURCE0_UC, base 0 is ABI:
 *                                                     test/tlbs.cpp hardcodes it)
 *   [0x010_0000_0000, 0x020_0000_0000)  reserved (BAR0 WC, not exposed: see below)
 *   [0x020_0000_0000, 0x030_0000_0000)  BAR2 UC     (RESOURCE1_UC)
 *   [0x030_0000_0000, 0x100_0000_0000)  reserved for further BAR mappings
 *                                       (BAR2 WC, BAR4 UC/WC: not exposed)
 *   [0x100_0000_0000, 0x200_0000_0000)  DMA buffers (memory.c): buffer i at
 *                                       TT_MMAP_DMA_BUF_BASE + i * 4 GiB
 *   [0x200_0000_0000, 0x400_0000_0000)  TLB windows (tlb.c): window id i UC at
 *                                       TT_MMAP_TLB_BASE + i * 8 GiB, WC at +4 GiB
 *   [0x400_0000_0000, 2^44)             unused, mmap fails with EINVAL
 *
 * A VMA must lie entirely inside one region; tt_cdev_mmap() checks this and
 * dispatches to tt_bar_mmap(), tt_memory_mmap() or tt_tlb_mmap().
 *
 * BAR0 WC is not offered: the kernel keeps UC- ioremaps inside BAR0 (window
 * 201, the register region), so with x86 PAT a whole-BAR0 WC mapping would be
 * UC- anyway, and its memtype reservation would conflict with (or downgrade)
 * the WC reservations of individual TLB windows. PAT note for tlb.c: a WC
 * mapping reserves a WC memtype for its physical range (tt_mmio_mmap); a UC
 * mapping of a page inside a live WC reservation is handed WC by PAT, and a
 * WC reservation that overlaps differing existing types fails with -EBUSY.
 * User BAR0 UC mappings also SIGBUS on window 201 (kernel-owned) and on the
 * TLB config page 0x1FC0_0000; NIU config (0x1FD0_4000) stays readable.
 */
#define TT_MMAP_BAR_REGION_SHIFT	36
#define TT_MMAP_BAR_REGION_SIZE		(1ULL << TT_MMAP_BAR_REGION_SHIFT)
#define TT_MMAP_BAR0_UC			(0ULL * TT_MMAP_BAR_REGION_SIZE)
#define TT_MMAP_BAR0_WC			(1ULL * TT_MMAP_BAR_REGION_SIZE)
#define TT_MMAP_BAR2_UC			(2ULL * TT_MMAP_BAR_REGION_SIZE)
#define TT_MMAP_BAR_END			(0x100ULL << 32)	/* 1 TiB */

#define TT_MMAP_DMA_BUF_BASE		(0x100ULL << 32)
#define TT_MMAP_DMA_BUF_STRIDE		(1ULL << 32)
#define TT_MMAP_DMA_BUF_END		(TT_MMAP_DMA_BUF_BASE + TENSTORRENT_MAX_DMA_BUFS * TT_MMAP_DMA_BUF_STRIDE)

#define TT_MMAP_TLB_BASE		(0x200ULL << 32)
#define TT_MMAP_TLB_STRIDE		(1ULL << 33)
#define TT_MMAP_TLB_WC_OFFSET		(1ULL << 32)
#define TT_MMAP_TLB_END			(TT_MMAP_TLB_BASE + TENSTORRENT_MAX_INBOUND_TLBS * TT_MMAP_TLB_STRIDE)

#define TT_MMAP_LIMIT			(1ULL << 44)

#define TT_MAX_MAPPINGS			2	/* entries reported by QUERY_MAPPINGS */
#define TT_IOCTL_MAX_OUTPUT_SIZE	(64 * 1024)	/* cap on output_size_bytes / mappings */
#define TT_MAX_PEER_MAPPINGS		64	/* MAP_PEER_BAR mappings per fd */

struct tenstorrent_device {
	struct kref kref;
	struct pci_dev *pdev;		/* reference held for the struct's lifetime */
	int ordinal;			/* /dev/tenstorrent/<ordinal>, also the minor */
	dev_t devt;
	struct cdev *cdev;		/* dynamically allocated: may outlive us */
	struct device *chardev;		/* class device "tenstorrent!<ordinal>" */
	struct dentry *debugfs_dir;
	struct proc_dir_entry *procfs_dir;

	/* Lifetime / removal. */
	struct srcu_struct detach_srcu;
	bool detached;			/* READ_ONCE/WRITE_ONCE */

	/* BAR resources (physical) and the kernel's MMIO mappings. */
	resource_size_t bar0_start, bar0_len;
	resource_size_t bar2_start, bar2_len;
	resource_size_t bar4_start, bar4_len;
	void __iomem *bar0_regs;	/* BAR0 [BH_REG_REGION_BAR0_OFFSET, +BH_REG_REGION_SIZE) */
	void __iomem *kernel_tlb;	/* BAR0 window BH_KERNEL_TLB_INDEX (2 MiB) */
	void __iomem *bar2;		/* all of BAR2 */
	unsigned int num_4g_windows;	/* BAR4 length / 4 GiB, at most 8 */

	/* Kernel NOC access through the reserved window (blackhole.c). */
	struct mutex kernel_tlb_mutex;
	bool mmio_gone;			/* set under kernel_tlb_mutex before iounmap */

	/* Hardware state discovered at probe. */
	bool noc_translation_en;
	bool fw_ready;			/* BOOT_STATUS0.msg_queue_ready seen at probe */
	u32 boot_status0;

	/* Interrupts. */
	bool msi_enabled;
	int irq;

	/* PCIe Device Control as found at probe (restored at remove). */
	u16 devctl_orig;
	bool devctl_changed;

	/* Open fds and O_EXCL admission. */
	struct mutex open_lock;
	struct list_head open_fds;	/* of chardev_private.open_fd_node */
	unsigned int open_count;
	bool excl_open;
	wait_queue_head_t open_wq;

	/* LOCK_CTL resource locks. */
	spinlock_t resource_lock;
	struct chardev_private *lock_owner[TENSTORRENT_RESOURCE_LOCK_COUNT];
	wait_queue_head_t lock_wq;

	/* QUERY_MAPPINGS table, built at probe. */
	struct tenstorrent_mapping mappings[TT_MAX_MAPPINGS];
	unsigned int num_mappings;

	/* Subsystem device-wide state. */
	struct tt_memory_dev_state memory;
	struct tt_tlb_dev_state tlb;
	struct tt_noc_dev_state noc;
	struct tt_smc_dev_state smc;
	struct tt_power_dev_state power;
};

/*
 * Lazily faulted MMIO mapping (BARs here, TLB windows in tlb.c); helper in
 * chardev.c, see tt_mmio_mmap() in chardev.h. ->mmap only validates and sets
 * the VMA up; the fault handler inserts PFNs inside a detach section, so no
 * PTE can be created after PCI remove has zapped the mappings (faults then
 * SIGBUS). One object is shared by all VMAs split/duplicated/mremapped from
 * the original mmap (kref, one per VMA); ops->release runs when the last
 * VMA is closed.
 */
struct tt_mmio_mapping;

struct tt_mmio_ops {
	/* Optional: false -> SIGBUS for the page at byte offset @off of the
	 * mapping. Called inside a detach section with mmap_lock held. */
	bool (*page_allowed)(struct tt_mmio_mapping *m, u64 off);
	/* Required: the last VMA is gone; free @m (or its container). Called
	 * from vm_ops->close (mmap_lock may be held: do not take it; do not
	 * take dev->open_lock either). */
	void (*release)(struct tt_mmio_mapping *m);
};

struct tt_mmio_mapping {
	/* Filled by the owner before tt_mmio_mmap(): */
	struct chardev_private *priv;
	const struct tt_mmio_ops *ops;
	phys_addr_t phys;		/* physical address of the mapping's first byte */
	bool wc;			/* write-combining, else uncached */
	bool no_split;			/* refuse VMA splits (partial munmap/mprotect) */
	/* Set by tt_mmio_mmap(): */
	u64 size;			/* bytes: the size of the original VMA */
	unsigned long pgoff;		/* vm_pgoff of the original VMA */
	unsigned long va;		/* vm_start of the original VMA (diagnostics) */
	bool wc_reserved;		/* PAT WC memtype reserved for [phys, +size) */
	struct kref kref;		/* one per VMA */
};

/* A user mmap of a BAR (tracked for debugfs). */
struct tt_bar_mapping {
	struct tt_mmio_mapping mmio;
	struct list_head node;		/* in chardev_private.bar_mappings, under bar_lock */
	unsigned int bar;		/* PCI BAR index */
	u64 bar_offset;
};

struct tt_peer_mapping {
	struct list_head node;
	struct pci_dev *peer;		/* reference held */
	bool dma_mapped;		/* dma_map_resource() (IOMMU) vs bus address */
	dma_addr_t dma_addr;
	size_t size;
};

/* Per open file. */
struct chardev_private {
	struct tenstorrent_device *dev;	/* reference held */
	struct file *filp;
	struct list_head open_fd_node;	/* in dev->open_fds, under dev->open_lock */
	struct pid *pid;		/* opener's thread group (reference held) */
	char comm[TASK_COMM_LEN];
	bool excl;			/* opened with O_EXCL */
	bool o_append;			/* opened with O_APPEND */
	bool live;			/* fully opened, not released; under open_lock */

	spinlock_t bar_lock;
	struct list_head bar_mappings;	/* of struct tt_bar_mapping */

	struct mutex peer_lock;
	struct list_head peer_mappings;	/* of struct tt_peer_mapping */
	unsigned int num_peer_mappings;

	/* Subsystem per-fd state. */
	struct tt_memory_fd_state memory;
	struct tt_tlb_fd_state tlb;
	struct tt_noc_fd_state noc;
	struct tt_smc_fd_state smc;
	struct tt_power_fd_state power;
};

/* module.c */
extern struct class *tt_class;
extern dev_t tt_devt_base;
extern struct dentry *tt_debugfs_root;
extern struct proc_dir_entry *tt_procfs_root;

/* pcie.c */
int tt_pci_register(void);
void tt_pci_unregister(void);
struct tenstorrent_device *tt_device_lookup(unsigned int minor); /* takes a ref */
void tt_device_get(struct tenstorrent_device *dev);
void tt_device_put(struct tenstorrent_device *dev);

/* Enter/leave a section that needs the device attached (see top). */
static inline int tt_dev_enter(struct tenstorrent_device *dev, int *srcu_idx)
{
	*srcu_idx = srcu_read_lock(&dev->detach_srcu);
	if (READ_ONCE(dev->detached)) {
		srcu_read_unlock(&dev->detach_srcu, *srcu_idx);
		return -ENODEV;
	}
	return 0;
}

static inline void tt_dev_exit(struct tenstorrent_device *dev, int srcu_idx)
{
	srcu_read_unlock(&dev->detach_srcu, srcu_idx);
}

/* procfs.c */
int tt_procfs_init(void);
void tt_procfs_exit(void);
void tt_procfs_device_add(struct tenstorrent_device *dev);
void tt_procfs_device_remove(struct tenstorrent_device *dev);

/* debugfs.c */
void tt_debugfs_init(void);
void tt_debugfs_exit(void);
void tt_debugfs_device_add(struct tenstorrent_device *dev);
void tt_debugfs_device_remove(struct tenstorrent_device *dev);

#endif
