// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// nic_bh_loopback - drive a real RDMA NIC into Blackhole DRAM (Milestone 2).
//
// Exports a Blackhole TLB window (aimed at a DRAM tile) as a dma-buf,
// registers it on an RDMA NIC via ibv_reg_dmabuf_mr(), and runs a
// self-looped RC queue pair to:
//   - RDMA WRITE from a host buffer into the BH aperture, then read the
//     BH DRAM back through a CPU TLB mapping and verify (NIC writes BH).
//   - seed the BH DRAM via the CPU mapping, RDMA READ it into a host
//     buffer, and verify (NIC reads BH).
//
// The NIC is the bus master in both directions; the BH is the target.
// This is the same RC-loopback setup as rdma_loopback.c, with the target
// MR swapped from host memory to a Blackhole dma-buf (the PHASE 2 SWAP).
//
// To compile:
//   make            # builds via tools/rdma/Makefile (links libibverbs)
//
// To run (pair a same-socket NIC + Blackhole):
//   sudo ./nic_bh_loopback -b /dev/tenstorrent/23 -r bnxt_re0
//   sudo ./nic_bh_loopback -b /dev/tenstorrent/23 -r bnxt_re0 -g 3 -x 17 -y 12

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>

#include <infiniband/verbs.h>

/* ===== tt-kmd ioctl definitions (subset of ioctl.h) ===== */

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 11)
#define TENSTORRENT_IOCTL_FREE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 12)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 13)
#define TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF	_IO(TENSTORRENT_IOCTL_MAGIC, 16)

#define BLACKHOLE_PCI_DEVICE_ID 0xb140

struct tenstorrent_get_device_info {
	struct { __u32 output_size_bytes; } in;
	struct {
		__u32 output_size_bytes;
		__u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id;
		__u16 bus_dev_fn, max_dma_buf_size_log2, pci_domain;
		__u16 reserved;
	} out;
};

struct tenstorrent_allocate_tlb {
	struct { __u64 size; __u64 reserved; } in;
	struct {
		__u32 id; __u32 reserved0;
		__u64 mmap_offset_uc; __u64 mmap_offset_wc; __u64 reserved1;
	} out;
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
	struct {
		__u32 id;
		__u32 reserved;
		struct tenstorrent_noc_tlb_config config;
	} in;
	struct { __u64 reserved; } out;
};

struct tenstorrent_export_tlb_dmabuf {
	__u32 argsz;
	__u32 flags;
	__u32 tlb_id;
	__s32 fd;
	__u64 offset;
	__u64 size;
};

/* ===== sizes ===== */

#define TLB_SIZE_4G	(4ULL << 30)
#define EXPORT_SIZE	(2ULL << 20)	// dma-buf sub-range of the window
#define MR_LEN		(64 * 1024)	// bytes actually transferred/verified

#define DIE(fmt, ...) \
	do { fprintf(stderr, "error: " fmt "\n", ##__VA_ARGS__); exit(1); } while (0)

/* ===== Blackhole side ===== */

static int open_blackhole(const char *path)
{
	struct tenstorrent_get_device_info info;
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		DIE("open %s: %s", path, strerror(errno));

	memset(&info, 0, sizeof(info));
	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0)
		DIE("GET_DEVICE_INFO: %s", strerror(errno));
	if (info.out.device_id != BLACKHOLE_PCI_DEVICE_ID)
		DIE("%s is not Blackhole (device_id=0x%04x)", path, info.out.device_id);

	return fd;
}

/* ===== NIC / verbs side (same RC-loopback as rdma_loopback.c) ===== */

struct endpoint {
	uint32_t qpn;
	uint32_t psn;
	uint16_t lid;
	union ibv_gid gid;
};

struct conn {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	uint8_t port;
	int gid_index;
};

// Real RoCE NICs require a RoCEv2 GID; the kernel resolves the destination
// MAC from it during RTR. Pick the first IPv4-mapped (::ffff:a.b.c.d) GID.
static int pick_rocev2_gid(struct ibv_context *ctx, uint8_t port)
{
	static const uint8_t v4_mapped_prefix[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
	union ibv_gid g;
	int i;

	for (i = 0; i < 16; i++) {
		if (ibv_query_gid(ctx, port, i, &g))
			break;
		if (memcmp(g.raw, v4_mapped_prefix, sizeof(v4_mapped_prefix)) == 0)
			return i;
	}
	return -1;
}

static struct ibv_context *open_rdma_device(const char *name)
{
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	int n, i;

	list = ibv_get_device_list(&n);
	if (!list || n == 0)
		DIE("no RDMA devices found");

	for (i = 0; i < n; i++) {
		if (!name || strcmp(ibv_get_device_name(list[i]), name) == 0) {
			ctx = ibv_open_device(list[i]);
			if (!ctx)
				DIE("ibv_open_device failed");
			printf("RDMA device: %s\n", ibv_get_device_name(list[i]));
			break;
		}
	}
	ibv_free_device_list(list);
	if (!ctx)
		DIE("RDMA device %s not found", name ? name : "(first)");
	return ctx;
}

static void create_qp(struct conn *c)
{
	struct ibv_qp_init_attr ia = {
		.send_cq = c->cq,
		.recv_cq = c->cq,
		.cap = { .max_send_wr = 16, .max_recv_wr = 16,
			 .max_send_sge = 1, .max_recv_sge = 1 },
		.qp_type = IBV_QPT_RC,
	};

	c->qp = ibv_create_qp(c->pd, &ia);
	if (!c->qp)
		DIE("ibv_create_qp failed: %s", strerror(errno));
}

static void to_init(struct conn *c)
{
	struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_INIT,
		.pkey_index = 0,
		.port_num = c->port,
		.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
				   IBV_ACCESS_REMOTE_READ,
	};

	if (ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
					IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
		DIE("modify INIT failed: %s", strerror(errno));
}

static void to_rtr(struct conn *c, const struct endpoint *remote)
{
	struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_RTR,
		.path_mtu = IBV_MTU_1024,
		.dest_qp_num = remote->qpn,
		.rq_psn = remote->psn,
		.max_dest_rd_atomic = 1,
		.min_rnr_timer = 12,
		.ah_attr = {
			.is_global = 1,
			.port_num = c->port,
			.grh = {
				.dgid = remote->gid,
				.sgid_index = c->gid_index,
				.hop_limit = 64,
			},
		},
	};

	if (ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
					IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
					IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
		DIE("modify RTR failed: %s (try a different -g gid index)", strerror(errno));
}

static void to_rts(struct conn *c, const struct endpoint *local)
{
	struct ibv_qp_attr attr = {
		.qp_state = IBV_QPS_RTS,
		.timeout = 14,
		.retry_cnt = 7,
		.rnr_retry = 7,
		.sq_psn = local->psn,
		.max_rd_atomic = 1,
	};

	if (ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
					IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
		DIE("modify RTS failed: %s", strerror(errno));
}

static void fill_endpoint(struct conn *c, struct endpoint *ep)
{
	struct ibv_port_attr pa;

	if (ibv_query_port(c->ctx, c->port, &pa))
		DIE("ibv_query_port failed");
	if (ibv_query_gid(c->ctx, c->port, c->gid_index, &ep->gid))
		DIE("ibv_query_gid(index=%d) failed", c->gid_index);
	ep->qpn = c->qp->qp_num;
	ep->psn = lrand48() & 0xffffff;
	ep->lid = pa.lid;
}

static void post_and_wait(struct conn *c, enum ibv_wr_opcode opcode,
			  struct ibv_mr *local_mr, void *local_buf, size_t len,
			  uint64_t remote_addr, uint32_t remote_rkey)
{
	struct ibv_sge sge = {
		.addr = (uintptr_t)local_buf,
		.length = len,
		.lkey = local_mr->lkey,
	};
	struct ibv_send_wr wr = {
		.wr_id = 1,
		.sg_list = &sge,
		.num_sge = 1,
		.opcode = opcode,
		.send_flags = IBV_SEND_SIGNALED,
		.wr.rdma.remote_addr = remote_addr,
		.wr.rdma.rkey = remote_rkey,
	};
	struct ibv_send_wr *bad = NULL;
	struct ibv_wc wc;
	int n;

	if (ibv_post_send(c->qp, &wr, &bad))
		DIE("ibv_post_send failed: %s", strerror(errno));

	do {
		n = ibv_poll_cq(c->cq, 1, &wc);
	} while (n == 0);

	if (n < 0)
		DIE("ibv_poll_cq failed");
	if (wc.status != IBV_WC_SUCCESS)
		DIE("completion error: %s", ibv_wc_status_str(wc.status));
}

int main(int argc, char **argv)
{
	const char *bh_path = "/dev/tenstorrent/23";
	const char *rdma_name = "bnxt_re0";
	int gid_index = -1;	// -1 = auto-pick a RoCEv2 GID
	uint16_t noc_x = 17, noc_y = 12;
	uint64_t noc_addr = 0;
	uint8_t port = 1;

	struct tenstorrent_allocate_tlb alloc = { 0 };
	struct tenstorrent_configure_tlb cfg = { 0 };
	struct tenstorrent_free_tlb ft = { 0 };
	struct tenstorrent_export_tlb_dmabuf exp = { 0 };
	struct conn ca, cb;
	struct endpoint ea, eb;
	struct ibv_context *ctx;
	struct ibv_mr *host_mr, *bh_mr;
	uint8_t *host;
	volatile uint8_t *bh_cpu;
	void *mmio;
	int bh_fd, dmabuf_fd, opt;
	size_t i;

	while ((opt = getopt(argc, argv, "b:r:g:x:y:a:h")) != -1) {
		switch (opt) {
		case 'b': bh_path = optarg; break;
		case 'r': rdma_name = optarg; break;
		case 'g': gid_index = atoi(optarg); break;
		case 'x': noc_x = (uint16_t)atoi(optarg); break;
		case 'y': noc_y = (uint16_t)atoi(optarg); break;
		case 'a': noc_addr = strtoull(optarg, NULL, 0); break;
		default:
			printf("usage: %s [-b /dev/tenstorrent/N] [-r rdma_dev] "
			       "[-g gid] [-x X] [-y Y] [-a noc_addr]\n", argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}

	srand48(getpid());

	// --- Blackhole: allocate + aim a 4G window at the DRAM tile, export it ---
	bh_fd = open_blackhole(bh_path);

	alloc.in.size = TLB_SIZE_4G;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
		DIE("ALLOCATE_TLB(4G): %s (BAR4 windows available?)", strerror(errno));

	cfg.in.id = alloc.out.id;
	cfg.in.config.addr = noc_addr;	// 4G-aligned base within the DRAM tile
	cfg.in.config.x_end = noc_x;
	cfg.in.config.y_end = noc_y;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cfg) != 0)
		DIE("CONFIGURE_TLB: %s", strerror(errno));

	exp.argsz = sizeof(exp);
	exp.tlb_id = alloc.out.id;
	exp.offset = 0;
	exp.size = EXPORT_SIZE;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF, &exp) != 0)
		DIE("EXPORT_TLB_DMABUF: %s", strerror(errno));
	dmabuf_fd = exp.fd;

	printf("Blackhole:   %s, window->(x=%u,y=%u,addr=0x%llx), dma-buf fd=%d\n",
	       bh_path, noc_x, noc_y, (unsigned long long)noc_addr, dmabuf_fd);

	// CPU view of the same aperture, for seeding/verifying BH DRAM.
	mmio = mmap(NULL, EXPORT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
	if (mmio == MAP_FAILED)
		DIE("mmap dma-buf fd: %s", strerror(errno));
	bh_cpu = mmio;

	// --- NIC: open verbs, build a self-looped RC connection ---
	ctx = open_rdma_device(rdma_name);
	memset(&ca, 0, sizeof(ca));
	memset(&cb, 0, sizeof(cb));
	ca.ctx = cb.ctx = ctx;
	ca.port = cb.port = port;

	if (gid_index < 0) {
		gid_index = pick_rocev2_gid(ctx, port);
		if (gid_index < 0)
			gid_index = 0;
	}
	ca.gid_index = cb.gid_index = gid_index;
	printf("Using gid index %d\n", gid_index);

	ca.pd = cb.pd = ibv_alloc_pd(ctx);
	if (!ca.pd)
		DIE("ibv_alloc_pd failed");
	ca.cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	cb.cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!ca.cq || !cb.cq)
		DIE("ibv_create_cq failed");

	// Host source/sink buffer.
	host = aligned_alloc(4096, MR_LEN);
	if (!host)
		DIE("aligned_alloc failed");
	host_mr = ibv_reg_mr(ca.pd, host, MR_LEN, IBV_ACCESS_LOCAL_WRITE);
	if (!host_mr)
		DIE("ibv_reg_mr(host) failed: %s", strerror(errno));

	// The Blackhole aperture, registered straight off the dma-buf fd.
	// iova == 0, so remote_addr is just the byte offset into the window.
	bh_mr = ibv_reg_dmabuf_mr(ca.pd, 0, MR_LEN, 0, dmabuf_fd,
				  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
				  IBV_ACCESS_REMOTE_READ);
	if (!bh_mr)
		DIE("ibv_reg_dmabuf_mr failed: %s\n"
		    "(P2P not routed, bnxt_re lacks dmabuf MR support, or gid issue)",
		    strerror(errno));
	printf("Registered:  host_mr rkey=0x%x, bh_mr rkey=0x%x\n",
	       host_mr->rkey, bh_mr->rkey);

	create_qp(&ca);
	create_qp(&cb);
	fill_endpoint(&ca, &ea);
	fill_endpoint(&cb, &eb);
	to_init(&ca); to_init(&cb);
	to_rtr(&ca, &eb); to_rtr(&cb, &ea);
	to_rts(&ca, &ea); to_rts(&cb, &eb);
	printf("Connected:   QP 0x%x <-> 0x%x on %s\n", ea.qpn, eb.qpn, rdma_name);

	// --- Test 1: NIC writes BH. RDMA WRITE host -> BH aperture. ---
	for (i = 0; i < MR_LEN; i++)
		host[i] = (uint8_t)(0x40 + (i & 0x3f));
	for (i = 0; i < MR_LEN; i++)
		bh_cpu[i] = 0;	// clear DRAM via CPU first

	post_and_wait(&ca, IBV_WR_RDMA_WRITE, host_mr, host, MR_LEN, 0, bh_mr->rkey);

	for (i = 0; i < MR_LEN; i++)
		if (bh_cpu[i] != host[i])
			DIE("WRITE verify FAILED at byte %zu: bh=0x%02x host=0x%02x",
			    i, bh_cpu[i], host[i]);
	printf("RDMA WRITE ok: NIC wrote %d bytes into Blackhole DRAM (CPU-verified)\n", MR_LEN);

	// --- Test 2: NIC reads BH. Seed DRAM via CPU, RDMA READ BH -> host. ---
	for (i = 0; i < MR_LEN; i++)
		bh_cpu[i] = (uint8_t)(0x80 + (i & 0x3f));
	(void)bh_cpu[0];	// UC read-back fences the writes above
	memset(host, 0, MR_LEN);

	post_and_wait(&ca, IBV_WR_RDMA_READ, host_mr, host, MR_LEN, 0, bh_mr->rkey);

	for (i = 0; i < MR_LEN; i++)
		if (host[i] != (uint8_t)(0x80 + (i & 0x3f)))
			DIE("READ verify FAILED at byte %zu: host=0x%02x", i, host[i]);
	printf("RDMA READ  ok: NIC read %d bytes from Blackhole DRAM (verified)\n", MR_LEN);

	printf("\nNIC <-> Blackhole peer-to-peer RDMA works.\n");

	ibv_destroy_qp(ca.qp);
	ibv_destroy_qp(cb.qp);
	ibv_dereg_mr(bh_mr);
	ibv_dereg_mr(host_mr);
	ibv_destroy_cq(ca.cq);
	ibv_destroy_cq(cb.cq);
	ibv_dealloc_pd(ca.pd);
	ibv_close_device(ctx);
	free(host);

	munmap(mmio, EXPORT_SIZE);
	close(dmabuf_fd);
	ft.in.id = alloc.out.id;
	ioctl(bh_fd, TENSTORRENT_IOCTL_FREE_TLB, &ft);
	close(bh_fd);
	return 0;
}
