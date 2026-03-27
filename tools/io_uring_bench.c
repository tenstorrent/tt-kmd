// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Benchmark NOC_IO via ioctl vs io_uring uring_cmd.
//
// To Compile:
//  gcc -O2 -Wall -o io_uring_bench io_uring_bench.c
//
// To Run:
//  ./io_uring_bench /dev/tenstorrent/0

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/types.h>

#ifndef IORING_SETUP_SQE128
#define IORING_SETUP_SQE128		(1U << 10)
#endif
#ifndef IORING_SETUP_SQPOLL
#define IORING_SETUP_SQPOLL		(1U << 1)
#endif
#ifndef IORING_OP_URING_CMD
#define IORING_OP_URING_CMD		46
#endif
#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS		(1U << 0)
#endif
#ifndef IORING_ENTER_SQ_WAKEUP
#define IORING_ENTER_SQ_WAKEUP		(1U << 1)
#endif
#ifndef IORING_SQ_NEED_WAKEUP
#define IORING_SQ_NEED_WAKEUP		(1U << 0)
#endif
#ifndef IORING_OFF_SQ_RING
#define IORING_OFF_SQ_RING		0ULL
#define IORING_OFF_CQ_RING		0x8000000ULL
#define IORING_OFF_SQES			0x10000000ULL
#endif
#ifndef IORING_REGISTER_FILES
#define IORING_REGISTER_FILES		2
#endif
#ifndef IOSQE_FIXED_FILE
#define IOSQE_FIXED_FILE		(1U << 0)
#endif

struct io_sq_ring_offsets {
	__u32 head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
	__u64 user_addr;
};

struct io_cq_ring_offsets {
	__u32 head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
	__u64 user_addr;
};

struct io_uring_params {
	__u32 sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle;
	__u32 features, wq_fd, resv[3];
	struct io_sq_ring_offsets sq_off;
	struct io_cq_ring_offsets cq_off;
};

struct io_uring_sqe {
	__u8  opcode;
	__u8  flags;
	__u16 ioprio;
	__s32 fd;
	union { __u64 off; struct { __u32 cmd_op; __u32 __pad1; }; };
	__u64 addr;
	__u32 len;
	__u32 rw_flags;
	__u64 user_data;
	__u16 buf_index;
	__u16 personality;
	__s32 splice_fd_in;
	union {
		struct {
			__u64 addr3;
			__u64 __pad2[1];
		};
		__u8 cmd[0];
	};
};

struct io_uring_cqe {
	__u64 user_data;
	__s32 res;
	__u32 flags;
};

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB    _IO(TENSTORRENT_IOCTL_MAGIC, 11)
#define TENSTORRENT_IOCTL_FREE_TLB        _IO(TENSTORRENT_IOCTL_MAGIC, 12)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB   _IO(TENSTORRENT_IOCTL_MAGIC, 13)
#define TENSTORRENT_IOCTL_NOC_IO          _IO(TENSTORRENT_IOCTL_MAGIC, 16)

struct tenstorrent_get_device_info {
	struct { __u32 output_size_bytes; } in;
	struct {
		__u32 output_size_bytes;
		__u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id;
		__u16 bus_dev_fn, max_dma_buf_size_log2, pci_domain;
		__u16 reserved;
	} out;
};

#define TENSTORRENT_NOC_IO_WRITE (1 << 0)
#define TENSTORRENT_NOC_IO_BLOCK (1 << 1)

struct tenstorrent_noc_io {
	__u32 argsz;
	__u32 flags;
	__u8  x;
	__u8  y;
	__u8  reserved0[2];
	__u32 reserved1;
	__u64 addr;
	__u64 data_ptr;
	__u64 data_len;
};

struct tenstorrent_allocate_tlb {
	struct { __u64 size; __u64 reserved; } in;
	struct { __u32 id; __u32 reserved0; __u64 mmap_offset_uc; __u64 mmap_offset_wc; __u64 reserved1; } out;
};

struct tenstorrent_free_tlb {
	struct { __u32 id; } in;
	struct { } out;
};

struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end, y_end, x_start, y_start;
	__u8 noc, mcast, ordering, linked, static_vc;
	__u8 reserved0[3];
	__u32 reserved1[2];
};

struct tenstorrent_configure_tlb {
	struct { __u32 id; __u32 reserved; struct tenstorrent_noc_tlb_config config; } in;
	struct { __u64 reserved; } out;
};

#define PCI_DEVICE_ID_BLACKHOLE 0xb140
#define PCI_DEVICE_ID_WORMHOLE  0x401e

#define TLB_2M_SIZE (1 << 21)
#define TLB_2M_MASK (TLB_2M_SIZE - 1)

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

struct mmio_window {
	int fd;
	uint32_t tlb_id;
	volatile uint32_t *base;
	size_t size;
};

static int mmio_window_open(struct mmio_window *w, int fd, uint8_t x, uint8_t y,
			    uint64_t addr, int use_wc)
{
	struct tenstorrent_allocate_tlb alloc = {};
	struct tenstorrent_configure_tlb conf = {};
	uint64_t offset;
	void *map;

	alloc.in.size = TLB_2M_SIZE;
	if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) < 0)
		return -errno;

	conf.in.id = alloc.out.id;
	conf.in.config.addr = addr & ~(uint64_t)TLB_2M_MASK;
	conf.in.config.x_end = x;
	conf.in.config.y_end = y;
	conf.in.config.ordering = use_wc ? 0 : 1;
	if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &conf) < 0) {
		struct tenstorrent_free_tlb fr = {};
		fr.in.id = alloc.out.id;
		ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &fr);
		return -errno;
	}

	offset = use_wc ? alloc.out.mmap_offset_wc : alloc.out.mmap_offset_uc;
	map = mmap(NULL, TLB_2M_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	if (map == MAP_FAILED) {
		struct tenstorrent_free_tlb fr = {};
		fr.in.id = alloc.out.id;
		ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &fr);
		return -errno;
	}

	w->fd = fd;
	w->tlb_id = alloc.out.id;
	w->base = (volatile uint32_t *)map;
	w->size = TLB_2M_SIZE;
	return 0;
}

static void mmio_window_close(struct mmio_window *w)
{
	struct tenstorrent_free_tlb fr = {};

	munmap((void *)w->base, w->size);
	fr.in.id = w->tlb_id;
	ioctl(w->fd, TENSTORRENT_IOCTL_FREE_TLB, &fr);
}

static void bench_mmio_read32(struct mmio_window *w, uint64_t addr, int iterations)
{
	uint32_t offset_words = (addr & TLB_2M_MASK) / sizeof(uint32_t);
	volatile uint32_t *ptr = w->base + offset_words;
	uint32_t dummy;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++)
		dummy = *ptr;
	(void)dummy;

	start = now_ns();
	for (i = 0; i < iterations; i++)
		dummy = *ptr;
	elapsed = now_ns() - start;

	printf("  mmio read32:       %d ops, %.0f ns/op\n",
	       iterations, (double)elapsed / iterations);
}

static void bench_mmio_write32(struct mmio_window *w, uint64_t addr, int iterations)
{
	uint32_t offset_words = (addr & TLB_2M_MASK) / sizeof(uint32_t);
	volatile uint32_t *ptr = w->base + offset_words;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++)
		*ptr = 0;

	start = now_ns();
	for (i = 0; i < iterations; i++)
		*ptr = (uint32_t)i;
	elapsed = now_ns() - start;

	printf("  mmio write32:      %d ops, %.0f ns/op\n",
	       iterations, (double)elapsed / iterations);
}

static int sys_io_uring_setup(unsigned entries, struct io_uring_params *p)
{
	return syscall(__NR_io_uring_setup, entries, p);
}

static int sys_io_uring_enter(int ring_fd, unsigned to_submit,
			      unsigned min_complete, unsigned flags)
{
	return syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

static int sys_io_uring_register(int ring_fd, unsigned opcode, void *arg, unsigned nr_args)
{
	return syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

struct ring {
	int fd;
	int use_fixed_fd;
	int fixed_fd_idx;
	unsigned sq_mask;
	unsigned cq_mask;
	unsigned sqe_size;
	unsigned *sq_head;
	unsigned *sq_tail;
	unsigned *sq_flags;
	unsigned *sq_array;
	unsigned *cq_head;
	unsigned *cq_tail;
	void *sqes;
	struct io_uring_cqe *cqes;
};

static int ring_init(struct ring *r, unsigned entries, unsigned flags)
{
	struct io_uring_params p = {};
	void *sq_ptr, *cq_ptr;
	size_t sq_sz, cq_sz;

	p.flags = flags | IORING_SETUP_SQE128;

	r->fd = sys_io_uring_setup(entries, &p);
	if (r->fd < 0)
		return -errno;

	r->sq_mask = p.sq_entries - 1;
	r->cq_mask = p.cq_entries - 1;
	r->sqe_size = 128;

	sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
	sq_ptr = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
		      r->fd, IORING_OFF_SQ_RING);
	if (sq_ptr == MAP_FAILED)
		return -errno;

	cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	cq_ptr = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
		      r->fd, IORING_OFF_CQ_RING);
	if (cq_ptr == MAP_FAILED)
		return -errno;

	r->sqes = mmap(NULL, p.sq_entries * r->sqe_size, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
	if (r->sqes == MAP_FAILED)
		return -errno;

	r->sq_head  = sq_ptr + p.sq_off.head;
	r->sq_tail  = sq_ptr + p.sq_off.tail;
	r->sq_flags = sq_ptr + p.sq_off.flags;
	r->sq_array = sq_ptr + p.sq_off.array;
	r->cq_head  = cq_ptr + p.cq_off.head;
	r->cq_tail  = cq_ptr + p.cq_off.tail;
	r->cqes     = cq_ptr + p.cq_off.cqes;

	return 0;
}

static struct io_uring_sqe *ring_get_sqe(struct ring *r)
{
	unsigned tail = *r->sq_tail;
	unsigned idx = tail & r->sq_mask;

	r->sq_array[idx] = idx;
	return (struct io_uring_sqe *)((char *)r->sqes + idx * r->sqe_size);
}

static void ring_submit_one(struct ring *r)
{
	atomic_store_explicit((_Atomic unsigned *)r->sq_tail,
			      *r->sq_tail + 1, memory_order_release);
}

static void ring_wait_cqe(struct ring *r)
{
	while (1) {
		unsigned head = atomic_load_explicit((_Atomic unsigned *)r->cq_head,
						     memory_order_acquire);
		unsigned tail = atomic_load_explicit((_Atomic unsigned *)r->cq_tail,
						     memory_order_acquire);
		if (head != tail)
			return;
	}
}

static struct io_uring_cqe *ring_peek_cqe(struct ring *r)
{
	unsigned head = atomic_load_explicit((_Atomic unsigned *)r->cq_head,
					     memory_order_acquire);
	unsigned tail = atomic_load_explicit((_Atomic unsigned *)r->cq_tail,
					     memory_order_acquire);

	if (head == tail)
		return NULL;

	return &r->cqes[head & r->cq_mask];
}

static void ring_advance_cq(struct ring *r)
{
	atomic_store_explicit((_Atomic unsigned *)r->cq_head,
			      *r->cq_head + 1, memory_order_release);
}

static void ring_sqpoll_wakeup_if_needed(struct ring *r)
{
	unsigned sq_flags = atomic_load_explicit((_Atomic unsigned *)r->sq_flags,
						 memory_order_acquire);
	if (sq_flags & IORING_SQ_NEED_WAKEUP)
		sys_io_uring_enter(r->fd, 0, 0, IORING_ENTER_SQ_WAKEUP);
}

static void prep_noc_io_cmd(struct ring *r, struct io_uring_sqe *sqe, int fd,
			    uint8_t x, uint8_t y, uint64_t addr, uint32_t flags,
			    void *data_ptr, uint32_t data_len)
{
	struct tenstorrent_noc_io *cmd;

	memset(sqe, 0, 128);
	sqe->opcode = IORING_OP_URING_CMD;

	if (r->use_fixed_fd) {
		sqe->fd = r->fixed_fd_idx;
		sqe->flags = IOSQE_FIXED_FILE;
	} else {
		sqe->fd = fd;
	}

	sqe->cmd_op = 0;

	cmd = (struct tenstorrent_noc_io *)sqe->cmd;
	cmd->argsz = sizeof(*cmd);
	cmd->flags = flags;
	cmd->x = x;
	cmd->y = y;
	cmd->addr = addr;
	cmd->data_ptr = (uint64_t)(uintptr_t)data_ptr;
	cmd->data_len = data_len;
}

static void bench_ioctl_read32(int fd, uint8_t x, uint8_t y, uint64_t addr, int iterations)
{
	struct tenstorrent_noc_io io;
	uint32_t val;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++) {
		memset(&io, 0, sizeof(io));
		io.argsz = sizeof(io);
		io.x = x; io.y = y; io.addr = addr;
		io.data_ptr = (uint64_t)(uintptr_t)&val; io.data_len = 4;
		ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io);
	}

	start = now_ns();
	for (i = 0; i < iterations; i++) {
		memset(&io, 0, sizeof(io));
		io.argsz = sizeof(io);
		io.x = x; io.y = y; io.addr = addr;
		io.data_ptr = (uint64_t)(uintptr_t)&val; io.data_len = 4;
		ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io);
	}
	elapsed = now_ns() - start;

	printf("  ioctl read32:      %d ops, %.0f ns/op\n",
	       iterations, (double)elapsed / iterations);
}

static void bench_ioctl_write32(int fd, uint8_t x, uint8_t y, uint64_t addr, int iterations)
{
	struct tenstorrent_noc_io io;
	uint32_t val = 0;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++) {
		memset(&io, 0, sizeof(io));
		io.argsz = sizeof(io);
		io.flags = TENSTORRENT_NOC_IO_WRITE;
		io.x = x; io.y = y; io.addr = addr;
		io.data_ptr = (uint64_t)(uintptr_t)&val; io.data_len = 4;
		ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io);
	}

	start = now_ns();
	for (i = 0; i < iterations; i++) {
		memset(&io, 0, sizeof(io));
		io.argsz = sizeof(io);
		io.flags = TENSTORRENT_NOC_IO_WRITE;
		io.x = x; io.y = y; io.addr = addr;
		io.data_ptr = (uint64_t)(uintptr_t)&val; io.data_len = 4;
		ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io);
	}
	elapsed = now_ns() - start;

	printf("  ioctl write32:     %d ops, %.0f ns/op\n",
	       iterations, (double)elapsed / iterations);
}

static int bench_uring_one_at_a_time(struct ring *r, int dev_fd, uint8_t x, uint8_t y,
				     uint64_t addr, uint32_t noc_flags, int iterations,
				     const char *label)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	uint32_t val = 0;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++) {
		sqe = ring_get_sqe(r);
		prep_noc_io_cmd(r, sqe, dev_fd, x, y, addr, noc_flags, &val, 4);
		ring_submit_one(r);
		sys_io_uring_enter(r->fd, 1, 1, IORING_ENTER_GETEVENTS);
		cqe = ring_peek_cqe(r);
		if (!cqe || cqe->res < 0) {
			fprintf(stderr, "  %s: warmup error: %d\n", label, cqe ? cqe->res : -1);
			if (cqe) ring_advance_cq(r);
			return -1;
		}
		ring_advance_cq(r);
	}

	start = now_ns();
	for (i = 0; i < iterations; i++) {
		sqe = ring_get_sqe(r);
		prep_noc_io_cmd(r, sqe, dev_fd, x, y, addr, noc_flags, &val, 4);
		ring_submit_one(r);
		sys_io_uring_enter(r->fd, 1, 1, IORING_ENTER_GETEVENTS);
		cqe = ring_peek_cqe(r);
		ring_advance_cq(r);
	}
	elapsed = now_ns() - start;

	printf("  %s: %d ops, %.0f ns/op\n", label, iterations, (double)elapsed / iterations);
	return 0;
}

static int bench_sqpoll(struct ring *r, int dev_fd, uint8_t x, uint8_t y,
			uint64_t addr, uint32_t noc_flags, int iterations,
			const char *label)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	uint32_t val = 0;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++) {
		sqe = ring_get_sqe(r);
		prep_noc_io_cmd(r, sqe, dev_fd, x, y, addr, noc_flags, &val, 4);
		ring_submit_one(r);
		ring_sqpoll_wakeup_if_needed(r);
		ring_wait_cqe(r);
		cqe = ring_peek_cqe(r);
		if (!cqe || cqe->res < 0) {
			fprintf(stderr, "  %s: warmup error: %d\n", label, cqe ? cqe->res : -1);
			if (cqe) ring_advance_cq(r);
			return -1;
		}
		ring_advance_cq(r);
	}

	start = now_ns();
	for (i = 0; i < iterations; i++) {
		sqe = ring_get_sqe(r);
		prep_noc_io_cmd(r, sqe, dev_fd, x, y, addr, noc_flags, &val, 4);
		ring_submit_one(r);
		ring_sqpoll_wakeup_if_needed(r);
		ring_wait_cqe(r);
		ring_advance_cq(r);
	}
	elapsed = now_ns() - start;

	printf("  %s: %d ops, %.0f ns/op\n", label, iterations, (double)elapsed / iterations);
	return 0;
}

int main(int argc, char *argv[])
{
	struct tenstorrent_get_device_info info;
	struct ring ring_plain = {};
	struct ring ring_sqpoll = {};
	uint8_t x, y;
	const char *dev_path;
	int fd;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/tenstorrent/N\n", argv[0]);
		return 1;
	}
	dev_path = argv[1];

	fd = open(dev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	memset(&info, 0, sizeof(info));
	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
		fprintf(stderr, "GET_DEVICE_INFO failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	if (info.out.device_id == PCI_DEVICE_ID_BLACKHOLE) {
		x = 1; y = 2;
	} else if (info.out.device_id == PCI_DEVICE_ID_WORMHOLE) {
		x = 1; y = 1;
	} else {
		fprintf(stderr, "Unknown device ID 0x%04x\n", info.out.device_id);
		close(fd);
		return 1;
	}

	printf("Device: %s (ID 0x%04x), target tile (%u, %u)\n\n",
	       dev_path, info.out.device_id, x, y);

	printf("=== direct MMIO baseline (UC, L1 @ 0x4000) ===\n");
	{
		struct mmio_window uc_win = {};
		if (mmio_window_open(&uc_win, fd, x, y, 0x0, 0) == 0) {
			bench_mmio_read32(&uc_win, 0x4000, 10000);
			bench_mmio_write32(&uc_win, 0x4000, 10000);
			mmio_window_close(&uc_win);
		} else {
			printf("  SKIP (TLB alloc failed)\n");
		}
	}

	printf("\n=== ioctl baseline ===\n");
	bench_ioctl_read32(fd, x, y, 0x4000, 10000);
	bench_ioctl_write32(fd, x, y, 0x4000, 10000);

	printf("\n=== io_uring (submit+wait per op, one syscall) ===\n");
	ret = ring_init(&ring_plain, 64, 0);
	if (ret < 0) {
		fprintf(stderr, "ring_init failed: %s\n", strerror(-ret));
		goto skip_uring;
	}
	bench_uring_one_at_a_time(&ring_plain, fd, x, y, 0x4000, 0, 10000, "uring read32 ");
	bench_uring_one_at_a_time(&ring_plain, fd, x, y, 0x4000, TENSTORRENT_NOC_IO_WRITE, 10000, "uring write32");
	close(ring_plain.fd);

	printf("\n=== io_uring + SQPOLL (busy-poll CQ, zero syscalls steady state) ===\n");
	ret = ring_init(&ring_sqpoll, 64, IORING_SETUP_SQPOLL);
	if (ret < 0) {
		fprintf(stderr, "SQPOLL ring_init failed: %s (try running as root)\n", strerror(-ret));
		goto skip_uring;
	}

	ret = sys_io_uring_register(ring_sqpoll.fd, IORING_REGISTER_FILES, &fd, 1);
	if (ret < 0) {
		fprintf(stderr, "SQPOLL REGISTER_FILES failed: %s\n", strerror(errno));
		close(ring_sqpoll.fd);
		goto skip_uring;
	}
	ring_sqpoll.use_fixed_fd = 1;
	ring_sqpoll.fixed_fd_idx = 0;

	bench_sqpoll(&ring_sqpoll, fd, x, y, 0x4000, 0, 10000, "sqpoll read32 ");
	bench_sqpoll(&ring_sqpoll, fd, x, y, 0x4000, TENSTORRENT_NOC_IO_WRITE, 10000, "sqpoll write32");
	close(ring_sqpoll.fd);

skip_uring:
	close(fd);
	return 0;
}
