// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// nic_bh_p2p_dma - DMA-centric two-node RDMA round-trip into Blackhole GDDR.
//
// The trick: MMIO *reads* of Blackhole are slow, but MMIO *writes* are fast and
// the NIC can DMA in and out of a GDDR tile over the fabric. So instead of
// CPU-reading the aperture to verify (the slow path in nic_bh_p2p), we read the
// data back out via DMA and compare "did I get out what I put in".
//
// Single binary, two roles (traffic flows over the wire between the NICs):
//   server (no peer arg): the Blackhole node. Aims a TLB window at a GDDR tile
//     at (X,Y,0x0), exports it as a dma-buf, registers it on the local NIC, and
//     then does exactly ONE fast MMIO write to seed a known pattern. After that
//     it is passive - no further MMIO.
//   client (peer arg = server IP): any other RoCE node. Drives all DMA:
//     Phase A: RDMA READ the seed back out of BH and verify (proves the CPU's
//              MMIO write is visible to the NIC's DMA reads through the NOC).
//     Phase B: RDMA WRITE a new pattern into BH, RDMA READ it back, verify
//              (a pure DMA round-trip, no MMIO at all).
//
// X, Y and the transfer size are compile-time constants for now (see below);
// command-line parsing for them can come later. The GDDR tile base is 0x0.
//
// To compile:  make    (links libibverbs)
// To run:
//   # on the Blackhole node:
//   sudo ./nic_bh_p2p_dma -b /dev/tenstorrent/0 -r mlx5_0
//   # on a peer RoCE node:
//   ./nic_bh_p2p_dma -r mlx5_1 192.168.99.1

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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
	__u32 argsz; __u32 flags; __u32 tlb_id; __s32 fd;
	__u64 offset; __u64 size;
};

/* ===== hard-coded test parameters (CLI parsing can come later) ===== */

#define NOC_X		17		// GDDR tile NOC coordinates
#define NOC_Y		12
#define NOC_ADDR	0ULL		// tile base; always 0x0 for this demo
#define XFER_SIZE	(2ULL << 30)	// bytes written/read/verified (2 GiB)
#define TLB_WINDOW_SIZE	(4ULL << 30)	// 4 GiB BAR4 window covers the tile

// TLB NOC ordering for the data window. The field is 2 bits; the kmd uses 1
// ("strict") for register access. Per Tenstorrent's NOC model 2 == Posted
// (fire-and-forget), the high-throughput mode for bulk writes.
// WARNING: with Posted, a read issued right after a write can race the write
// into GDDR. Phase B's immediate DMA read-back is only guaranteed correct for
// the non-posted modes (0 = relaxed, 1 = strict); drop to 1 if it gets flaky.
#define TLB_ORDERING	0
// Desired RoCE MTU ceiling. The actual value is clamped to the port's
// active_mtu at connect time: RoCEv2 rides Ethernet, so the RDMA MTU must fit
// inside the link MTU (a default 1500-byte link caps this at 1024). Raise the
// interface MTU (jumbo frames) on both ports to let a larger value take hold.
#define RDMA_MTU_MAX	IBV_MTU_4096

// DMA is issued in chunks with a sliding window of outstanding work requests:
// keeps each message well under the 2 GiB single-message cap and pipelines for
// bandwidth. XFER_SIZE need not be a multiple of RDMA_CHUNK.
#define RDMA_CHUNK	(64ULL << 20)	// 64 MiB per work request
#define MAX_QPS		16		// upper bound on parallel QPs (-q)
#define OUTSTANDING_PER_QP 4		// in-flight work requests kept per QP
#define SQ_DEPTH	(OUTSTANDING_PER_QP + 8)  // send queue depth per QP
#define DEFAULT_QPS	1		// -q overrides; 1 reproduces the old path

// Distinct salts for the two patterns so Phase B can't pass on Phase A data.
#define SALT_SEED	0xA5A5A5A5A5A5A5A5ULL	// MMIO-seeded (Phase A)
#define SALT_DMA	0x5C5C5C5C5C5C5C5CULL	// DMA-written  (Phase B)

#define TCP_PORT	18515

#define DIE(fmt, ...) \
	do { fprintf(stderr, "error: " fmt "\n", ##__VA_ARGS__); exit(1); } while (0)

/* ===== helpers ===== */

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void report(const char *who, const char *label, size_t bytes, double dt)
{
	printf("%s: %-18s %5zu MiB in %8.3f ms  (%6.2f GiB/s)\n",
	       who, label, bytes >> 20, dt * 1e3,
	       (double)bytes / dt / (double)(1ULL << 30));
}

// Push CPU write-combining buffers toward the device. WC stores may linger in a
// core buffer and reorder, so a barrier must precede the non-posted read-back
// that fences the seed through to GDDR.
static inline void wc_flush(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
	__asm__ volatile("sfence" ::: "memory");
#else
	__sync_synchronize();
#endif
}

// Position-dependent pattern word: a misrouted/duplicated/dropped chunk changes
// the value at that offset, so the verify catches it. Both nodes agree on this.
static inline uint64_t patword(uint64_t word_index, uint64_t salt)
{
	return (word_index * 0x9E3779B97F4A7C15ULL) ^ salt;
}

/* ===== RDMA plumbing ===== */

struct conn {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;		// one shared CQ across all QPs
	struct ibv_qp *qp[MAX_QPS];
	int n_qp;
	uint8_t port;
	int gid_index;
};

// Wire format for the out-of-band TCP handshake. Both nodes are assumed to share
// host byte order (x86-64 / aarch64 little-endian) for simplicity.
struct exch {
	uint32_t n_qp;
	uint32_t rkey;	// server: bh_mr rkey
	uint64_t addr;	// server: bh_mr iova (0)
	uint8_t gid[16];
	uint32_t qpn[MAX_QPS];
	uint32_t psn[MAX_QPS];
};

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
			break;
		}
	}
	ibv_free_device_list(list);
	if (!ctx)
		DIE("RDMA device %s not found/openable", name ? name : "(first)");
	return ctx;
}

static void setup_verbs(struct conn *c, const char *rdma_name, int gid_index,
			int n_qp, struct exch *local)
{
	union ibv_gid gid;
	int cqe, i;

	c->n_qp = n_qp;
	c->port = 1;
	c->ctx = open_rdma_device(rdma_name);
	c->pd = ibv_alloc_pd(c->ctx);
	if (!c->pd)
		DIE("ibv_alloc_pd failed");

	// One shared CQ sized for all QPs' outstanding work plus slack.
	cqe = OUTSTANDING_PER_QP * n_qp + n_qp + 16;
	c->cq = ibv_create_cq(c->ctx, cqe, NULL, NULL, 0);
	if (!c->cq)
		DIE("ibv_create_cq failed");

	if (gid_index < 0) {
		gid_index = pick_rocev2_gid(c->ctx, c->port);
		if (gid_index < 0)
			DIE("no RoCEv2 (IPv4-mapped) GID found; pass -g");
	}
	c->gid_index = gid_index;

	if (ibv_query_gid(c->ctx, c->port, c->gid_index, &gid))
		DIE("ibv_query_gid failed");

	local->n_qp = n_qp;
	memcpy(local->gid, gid.raw, 16);
	for (i = 0; i < n_qp; i++) {
		struct ibv_qp_init_attr ia = {
			.send_cq = c->cq, .recv_cq = c->cq,
			.cap = { .max_send_wr = SQ_DEPTH, .max_recv_wr = 4,
				 .max_send_sge = 1, .max_recv_sge = 1 },
			.qp_type = IBV_QPT_RC,
		};

		c->qp[i] = ibv_create_qp(c->pd, &ia);
		if (!c->qp[i])
			DIE("ibv_create_qp[%d] failed: %s", i, strerror(errno));
		local->qpn[i] = c->qp[i]->qp_num;
		local->psn[i] = lrand48() & 0xffffff;
	}
	printf("local:  %d qp(s), qpn[0]=0x%x gid_index=%d\n",
	       n_qp, local->qpn[0], c->gid_index);
}

static void connect_one_qp(struct conn *c, int i, const struct exch *local,
			   const struct exch *remote, enum ibv_mtu mtu)
{
	struct ibv_qp_attr init = {
		.qp_state = IBV_QPS_INIT,
		.pkey_index = 0,
		.port_num = c->port,
		.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
				   IBV_ACCESS_REMOTE_READ,
	};
	struct ibv_qp_attr rtr = {
		.qp_state = IBV_QPS_RTR,
		.path_mtu = mtu,
		.dest_qp_num = remote->qpn[i],
		.rq_psn = remote->psn[i],
		.max_dest_rd_atomic = 16,
		.min_rnr_timer = 12,
		.ah_attr = {
			.is_global = 1,
			.port_num = c->port,
			.grh = { .sgid_index = c->gid_index, .hop_limit = 64 },
		},
	};
	struct ibv_qp_attr rts = {
		.qp_state = IBV_QPS_RTS,
		.timeout = 14,
		.retry_cnt = 7,
		.rnr_retry = 7,
		.sq_psn = local->psn[i],
		.max_rd_atomic = 16,
	};

	memcpy(rtr.ah_attr.grh.dgid.raw, remote->gid, 16);

	if (ibv_modify_qp(c->qp[i], &init, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
					   IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
		DIE("modify INIT[%d] failed: %s", i, strerror(errno));
	if (ibv_modify_qp(c->qp[i], &rtr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
					  IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
					  IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
		DIE("modify RTR[%d] failed: %s (gid/route issue?)", i, strerror(errno));
	if (ibv_modify_qp(c->qp[i], &rts, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
					  IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
		DIE("modify RTS[%d] failed: %s", i, strerror(errno));
}

static void connect_qp(struct conn *c, const struct exch *local, const struct exch *remote)
{
	enum ibv_mtu mtu = RDMA_MTU_MAX;
	struct ibv_port_attr pa;
	int i;

	// Clamp the RoCE MTU to what the port actually negotiated, so we never
	// emit packets larger than the Ethernet link can carry (which would just
	// be dropped -> "transport retry counter exceeded").
	if (!ibv_query_port(c->ctx, c->port, &pa) && pa.active_mtu < mtu)
		mtu = pa.active_mtu;
	printf("path MTU = %d bytes\n", 128 << mtu);

	for (i = 0; i < c->n_qp; i++)
		connect_one_qp(c, i, local, remote, mtu);
}

// Chunked, pipelined RDMA over [0, total). Chunks are round-robined across all
// QPs (remote iova base is 0, so remote_addr == byte offset), keeping up to
// OUTSTANDING_PER_QP * n_qp work requests in flight across the shared CQ. The
// per-QP parallelism is the whole point: it tests whether the P2P-into-BH limit
// is per-stream serialization (scales with QPs) or aggregate fabric bandwidth.
static void rdma_xfer(struct conn *c, enum ibv_wr_opcode op, struct ibv_mr *mr,
		      uint8_t *host, size_t total, uint64_t remote_base, uint32_t rkey)
{
	int window = OUTSTANDING_PER_QP * c->n_qp;
	size_t off = 0;
	int inflight = 0;
	int next = 0;

	while (off < total || inflight) {
		while (off < total && inflight < window) {
			size_t len = (total - off < RDMA_CHUNK) ? total - off : RDMA_CHUNK;
			struct ibv_sge sge = {
				.addr = (uintptr_t)(host + off),
				.length = (uint32_t)len,
				.lkey = mr->lkey,
			};
			struct ibv_send_wr wr = {
				.wr_id = off, .sg_list = &sge, .num_sge = 1,
				.opcode = op, .send_flags = IBV_SEND_SIGNALED,
				.wr.rdma.remote_addr = remote_base + off,
				.wr.rdma.rkey = rkey,
			};
			struct ibv_send_wr *bad = NULL;

			if (ibv_post_send(c->qp[next], &wr, &bad))
				DIE("ibv_post_send failed at off=%zu: %s", off, strerror(errno));
			next = (next + 1) % c->n_qp;
			off += len;
			inflight++;
		}

		struct ibv_wc wc;
		int n = ibv_poll_cq(c->cq, 1, &wc);

		if (n < 0)
			DIE("ibv_poll_cq failed");
		if (n > 0) {
			if (wc.status != IBV_WC_SUCCESS)
				DIE("completion error: %s (wr off=%llu)",
				    ibv_wc_status_str(wc.status),
				    (unsigned long long)wc.wr_id);
			inflight--;
		}
	}
}

/* ===== TCP out-of-band exchange ===== */

static void rw_all(int fd, void *buf, size_t n, int writing)
{
	uint8_t *p = buf;
	while (n) {
		ssize_t r = writing ? write(fd, p, n) : read(fd, p, n);
		if (r <= 0)
			DIE("tcp %s failed: %s", writing ? "write" : "read", strerror(errno));
		p += r;
		n -= (size_t)r;
	}
}

static int tcp_listen_accept(void)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT),
				    .sin_addr.s_addr = htonl(INADDR_ANY) };
	int lfd, fd, yes = 1;

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0)
		DIE("socket: %s", strerror(errno));
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)))
		DIE("bind: %s", strerror(errno));
	if (listen(lfd, 1))
		DIE("listen: %s", strerror(errno));
	printf("waiting for client on tcp/%d ...\n", TCP_PORT);
	fd = accept(lfd, NULL, NULL);
	if (fd < 0)
		DIE("accept: %s", strerror(errno));
	close(lfd);
	return fd;
}

static int tcp_connect(const char *host)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT) };
	int fd;

	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
		DIE("bad server IP %s (use a dotted-quad)", host);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		DIE("socket: %s", strerror(errno));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
		DIE("connect %s:%d: %s", host, TCP_PORT, strerror(errno));
	return fd;
}

/* ===== Blackhole side (server only) ===== */

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

/* ===== Blackhole PCIe perf counters (sysfs, server side) ===== */

struct bh_counters {
	uint64_t posted_wr;	// slv posted write data words received (NIC->BH)
	uint64_t nonposted_wr;	// slv non-posted write data words received
	uint64_t rd_sent;	// slv read data words sent (BH->NIC)
};

static uint64_t read_u64(const char *path)
{
	uint64_t v = 0;
	FILE *f = fopen(path, "r");

	if (!f)
		return 0;
	if (fscanf(f, "%" SCNu64, &v) != 1)
		v = 0;
	fclose(f);
	return v;
}

// Each counter is split across two link-half files (suffix 0/1); sum them.
static uint64_t read_pair(const char *dir, const char *base)
{
	char p[512];
	uint64_t sum = 0;
	int half;

	for (half = 0; half < 2; half++) {
		snprintf(p, sizeof(p), "%s/%s%d", dir, base, half);
		sum += read_u64(p);
	}
	return sum;
}

static void counters_sample(const char *dir, struct bh_counters *c)
{
	c->posted_wr    = read_pair(dir, "slv_posted_wr_data_word_received");
	c->nonposted_wr = read_pair(dir, "slv_nonposted_wr_data_word_received");
	c->rd_sent      = read_pair(dir, "slv_rd_data_word_sent");
}

// sysfs escapes the '/' in the "tenstorrent/N" device name as '!'.
static void counters_dir(const char *bh_path, char *out, size_t outsz)
{
	const char *slash = strrchr(bh_path, '/');
	int idx = slash ? atoi(slash + 1) : 0;

	snprintf(out, outsz, "/sys/class/tenstorrent/tenstorrent!%d/pcie_perf_counters", idx);
}

// Bracket one client-driven DMA phase with counter snapshots. The client sends
// a byte just before and just after its transfer, so the delta tightly
// encloses it. This is how we see posted-vs-non-posted inbound writes.
static void counters_phase(int sock, const char *dir, const char *label)
{
	struct bh_counters a, b;
	uint8_t s;

	rw_all(sock, &s, 1, 0);		// "phase begin"
	counters_sample(dir, &a);
	rw_all(sock, &s, 1, 0);		// "phase end"
	counters_sample(dir, &b);

	printf("server: %-13s  slv_wr words: posted=%" PRIu64 " nonposted=%" PRIu64
	       "  slv_rd words sent=%" PRIu64 "\n",
	       label,
	       b.posted_wr - a.posted_wr,
	       b.nonposted_wr - a.nonposted_wr,
	       b.rd_sent - a.rd_sent);
}

static int run_server(const char *bh_path, const char *rdma_name, int gid_index, int n_qp)
{
	struct tenstorrent_allocate_tlb alloc = { 0 };
	struct tenstorrent_configure_tlb cfg = { 0 };
	struct tenstorrent_free_tlb ft = { 0 };
	struct tenstorrent_export_tlb_dmabuf exp = { 0 };
	struct conn c = { 0 };
	struct exch local = { 0 }, remote = { 0 };
	struct ibv_mr *bh_mr;
	volatile uint64_t *bh_words;
	size_t nwords = XFER_SIZE / sizeof(uint64_t);
	size_t j;
	void *mmio;
	int bh_fd, dmabuf_fd, sock;
	uint8_t sync;
	double t0, t1;
	char cdir[256];

	bh_fd = open_blackhole(bh_path);
	counters_dir(bh_path, cdir, sizeof(cdir));

	alloc.in.size = TLB_WINDOW_SIZE;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
		DIE("ALLOCATE_TLB(4G): %s", strerror(errno));
	cfg.in.id = alloc.out.id;
	cfg.in.config.addr = NOC_ADDR;
	cfg.in.config.x_end = NOC_X;
	cfg.in.config.y_end = NOC_Y;
	cfg.in.config.ordering = TLB_ORDERING;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cfg) != 0)
		DIE("CONFIGURE_TLB: %s", strerror(errno));

	exp.argsz = sizeof(exp);
	exp.tlb_id = alloc.out.id;
	exp.size = XFER_SIZE;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF, &exp) != 0)
		DIE("EXPORT_TLB_DMABUF: %s", strerror(errno));
	dmabuf_fd = exp.fd;

	// Write-combining CPU view, for the fast MMIO seed only. The dma-buf fd
	// is used solely to register the RDMA MR below.
	mmio = mmap(NULL, XFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		    bh_fd, alloc.out.mmap_offset_wc);
	if (mmio == MAP_FAILED)
		DIE("mmap TLB window (WC): %s", strerror(errno));
	bh_words = mmio;

	printf("server: BH %s window->(x=%u,y=%u,addr=0x%llx) %zu MiB exported\n",
	       bh_path, NOC_X, NOC_Y, (unsigned long long)NOC_ADDR, (size_t)(XFER_SIZE >> 20));

	setup_verbs(&c, rdma_name, gid_index, n_qp, &local);
	// RELAXED_ORDERING lets the responder's P2P writes into BH be reordered
	// across the PCIe/mesh hops instead of strictly serialized - this is the
	// one knob ib_write_bw has on (and we didn't) that can lift write BW.
	bh_mr = ibv_reg_dmabuf_mr(c.pd, 0, XFER_SIZE, 0, dmabuf_fd,
				  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
				  IBV_ACCESS_REMOTE_READ | IBV_ACCESS_RELAXED_ORDERING);
	if (!bh_mr)
		DIE("ibv_reg_dmabuf_mr failed: %s", strerror(errno));
	local.rkey = bh_mr->rkey;
	local.addr = 0;	// iova
	printf("server: bh_mr rkey=0x%x\n", bh_mr->rkey);

	sock = tcp_listen_accept();
	rw_all(sock, &remote, sizeof(remote), 0);	// recv client's QP info
	rw_all(sock, &local, sizeof(local), 1);		// send ours (incl rkey/addr)
	if (remote.n_qp != local.n_qp)
		DIE("QP count mismatch: local %d vs remote %d (use the same -q)",
		    local.n_qp, remote.n_qp);
	connect_qp(&c, &local, &remote);
	printf("server: connected to client (%d qp(s), qpn[0]=0x%x)\n", remote.n_qp, remote.qpn[0]);

	// The one and only MMIO touch: seed the tile with the SEED pattern using
	// wide write-combining stores, then fence it into GDDR (barrier + a single
	// non-posted read-back) so the client's DMA read observes it.
	t0 = now_sec();
	for (j = 0; j < nwords; j++)
		bh_words[j] = patword(j, SALT_SEED);
	wc_flush();
	(void)bh_words[nwords - 1];	// non-posted read-back: seed now in GDDR
	t1 = now_sec();
	report("server", "MMIO seed write", XFER_SIZE, t1 - t0);

	rw_all(sock, &sync, 1, 1);	// "seeded, you may DMA"

	// Sample BH PCIe counters around each client-driven DMA phase: shows
	// whether inbound writes arrive posted vs non-posted (the throughput
	// asymmetry) and the read words BH returns.
	counters_phase(sock, cdir, "DMA read  (A)");
	counters_phase(sock, cdir, "DMA write (B)");
	counters_phase(sock, cdir, "DMA read  (B)");

	rw_all(sock, &sync, 1, 0);	// wait for client "all verified"
	printf("server: client reported DMA round-trip verified\n");
	printf("\nNIC <-> Blackhole DMA round-trip verified over the fabric.\n");

	close(sock);
	ibv_dereg_mr(bh_mr);
	munmap(mmio, XFER_SIZE);
	close(dmabuf_fd);
	ft.in.id = alloc.out.id;
	ioctl(bh_fd, TENSTORRENT_IOCTL_FREE_TLB, &ft);
	close(bh_fd);
	return 0;
}

/* ===== peer side (client only) - drives all DMA ===== */

static int run_client(const char *server_ip, const char *rdma_name, int gid_index, int n_qp)
{
	struct conn c = { 0 };
	struct exch local = { 0 }, remote = { 0 };
	struct ibv_mr *host_mr;
	uint64_t *host;
	size_t nwords = XFER_SIZE / sizeof(uint64_t);
	size_t j;
	int sock;
	uint8_t sync = 1;
	double t0, t1;

	setup_verbs(&c, rdma_name, gid_index, n_qp, &local);

	host = aligned_alloc(4096, XFER_SIZE);
	if (!host)
		DIE("aligned_alloc(%zu) failed", (size_t)XFER_SIZE);
	host_mr = ibv_reg_mr(c.pd, host, XFER_SIZE,
			     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_RELAXED_ORDERING);
	if (!host_mr)
		DIE("ibv_reg_mr failed: %s", strerror(errno));

	sock = tcp_connect(server_ip);
	rw_all(sock, &local, sizeof(local), 1);		// send our QP info
	rw_all(sock, &remote, sizeof(remote), 0);	// recv server's (incl rkey/addr)
	if (remote.n_qp != local.n_qp)
		DIE("QP count mismatch: local %d vs remote %d (use the same -q)",
		    local.n_qp, remote.n_qp);
	connect_qp(&c, &local, &remote);
	printf("client: connected to server (%d qp(s), qpn[0]=0x%x, bh rkey=0x%x)\n",
	       remote.n_qp, remote.qpn[0], remote.rkey);

	rw_all(sock, &sync, 1, 0);	// wait for "seeded, you may DMA"

	// Phase A: DMA the MMIO-seeded pattern back out and verify. This proves
	// the server's CPU MMIO write is visible to the NIC's DMA reads.
	memset(host, 0, XFER_SIZE);
	rw_all(sock, &sync, 1, 1);	// phase begin: server snapshots counters
	t0 = now_sec();
	rdma_xfer(&c, IBV_WR_RDMA_READ, host_mr, (uint8_t *)host, XFER_SIZE, remote.addr, remote.rkey);
	t1 = now_sec();
	rw_all(sock, &sync, 1, 1);	// phase end: server snapshots counters
	report("client", "DMA read  (A)", XFER_SIZE, t1 - t0);
	for (j = 0; j < nwords; j++)
		if (host[j] != patword(j, SALT_SEED))
			DIE("phase A verify FAILED at word %zu: got=0x%016llx want=0x%016llx",
			    j, (unsigned long long)host[j],
			    (unsigned long long)patword(j, SALT_SEED));
	printf("client: phase A OK - MMIO-seeded pattern read back via DMA\n");

	// Phase B: DMA a new pattern in, DMA it back out, verify. No MMIO at all.
	// The WRITE and READ ride the same RC QP, so the responder's posted write
	// into BH is ordered ahead of the read that pulls it back.
	for (j = 0; j < nwords; j++)
		host[j] = patword(j, SALT_DMA);
	rw_all(sock, &sync, 1, 1);	// phase begin
	t0 = now_sec();
	rdma_xfer(&c, IBV_WR_RDMA_WRITE, host_mr, (uint8_t *)host, XFER_SIZE, remote.addr, remote.rkey);
	t1 = now_sec();
	rw_all(sock, &sync, 1, 1);	// phase end
	report("client", "DMA write (B)", XFER_SIZE, t1 - t0);

	memset(host, 0, XFER_SIZE);
	rw_all(sock, &sync, 1, 1);	// phase begin
	t0 = now_sec();
	rdma_xfer(&c, IBV_WR_RDMA_READ, host_mr, (uint8_t *)host, XFER_SIZE, remote.addr, remote.rkey);
	t1 = now_sec();
	rw_all(sock, &sync, 1, 1);	// phase end
	report("client", "DMA read  (B)", XFER_SIZE, t1 - t0);
	for (j = 0; j < nwords; j++)
		if (host[j] != patword(j, SALT_DMA))
			DIE("phase B verify FAILED at word %zu: got=0x%016llx want=0x%016llx",
			    j, (unsigned long long)host[j],
			    (unsigned long long)patword(j, SALT_DMA));
	printf("client: phase B OK - DMA write then DMA read round-trip verified\n");

	rw_all(sock, &sync, 1, 1);	// tell server "all verified"

	close(sock);
	ibv_dereg_mr(host_mr);
	free(host);
	return 0;
}

int main(int argc, char **argv)
{
	const char *bh_path = "/dev/tenstorrent/0";
	const char *rdma_name = "mlx5_0";
	int gid_index = -1;
	int n_qp = DEFAULT_QPS;
	const char *peer;
	int opt;

	while ((opt = getopt(argc, argv, "b:r:g:q:h")) != -1) {
		switch (opt) {
		case 'b': bh_path = optarg; break;
		case 'r': rdma_name = optarg; break;
		case 'g': gid_index = atoi(optarg); break;
		case 'q': n_qp = atoi(optarg); break;
		default:
			printf("usage:\n"
			       "  server: %s -b /dev/tenstorrent/N -r rdma_dev [-g gid] [-q nqp]\n"
			       "  client: %s -r rdma_dev [-q nqp] <server_ip>\n"
			       "  -q: parallel QPs (default %d, max %d); both ends must match\n",
			       argv[0], argv[0], DEFAULT_QPS, MAX_QPS);
			return opt == 'h' ? 0 : 2;
		}
	}

	if (n_qp < 1 || n_qp > MAX_QPS)
		DIE("-q must be 1..%d", MAX_QPS);

	srand48(getpid());
	peer = (optind < argc) ? argv[optind] : NULL;

	if (peer)
		return run_client(peer, rdma_name, gid_index, n_qp);
	return run_server(bh_path, rdma_name, gid_index, n_qp);
}
