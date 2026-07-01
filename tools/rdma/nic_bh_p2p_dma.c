// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// nic_bh_p2p_dma - minimal NIC <-> Blackhole peer-to-peer DMA demo.
//
// Demonstrates that an RDMA NIC can DMA directly into and out of Blackhole GDDR
// over the fabric, with both transfers initiated by the NIC and no Blackhole
// CPU involvement on the data path.
//
// The mechanism: the Blackhole node aims a TLB window at a GDDR tile, exports
// that window as a dma-buf (TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF), and registers
// it as an RDMA memory region. The peer node then drives the DMA.
//
// Single binary, two roles (traffic flows over the wire between the two NICs):
//   server (no peer arg): the Blackhole node. Exports a GDDR tile as an MR and
//     is otherwise passive - it never touches the data.
//   client (peer arg = server IP): the NIC that drives the DMA:
//     1. RDMA WRITE a pattern into BH   (NIC -> BH)
//     2. RDMA READ  it back out of BH   (BH -> NIC)
//     3. verify the read-back matches what was written.
//
// The GDDR tile coordinates and transfer size are compile-time constants; the
// device path and RDMA device name are command-line arguments.
//
// To compile:  make
// To run (RoCE example, gid index 3 = RoCEv2 IPv4):
//   # on the Blackhole node:
//   ./nic_bh_p2p_dma -b /dev/tenstorrent/5 -r rocep201s0f0 -g 3
//   # on the peer node:
//   ./nic_bh_p2p_dma -r rocep201s0f0 -g 3 10.32.34.129

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
#include <sys/random.h>
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
#define TENSTORRENT_IOCTL_SET_POWER_STATE	_IO(TENSTORRENT_IOCTL_MAGIC, 15)
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

struct tenstorrent_power_state {
	__u32 argsz;
	__u32 flags;
	__u8 reserved0;
	__u8 validity;
#define TT_POWER_VALIDITY_FLAGS(n)	(((n) & 0xF) << 0)
#define TT_POWER_VALIDITY_SETTINGS(n)	(((n) & 0xF) << 4)
#define TT_POWER_VALIDITY(flags_count, settings_count) \
	(TT_POWER_VALIDITY_FLAGS(flags_count) | TT_POWER_VALIDITY_SETTINGS(settings_count))
	__u16 power_flags;
#define TT_POWER_FLAG_MAX_AI_CLK	(1U << 0) /* 1=Max AI Clock,  0=Min AI Clock */
#define TT_POWER_FLAG_MRISC_PHY_WAKEUP	(1U << 1) /* 1=PHY Wakeup,    0=PHY Powerdown */
#define TT_POWER_FLAG_TENSIX_ENABLE	(1U << 2) /* 1=Enable Tensix, 0=Clock Gate Tensix */
#define TT_POWER_FLAG_L2CPU_ENABLE	(1U << 3) /* 1=Enable L2CPU,  0=Clock Gate L2CPU */
	__u16 power_settings[14];
};

/* ===== hard-coded test parameters ===== */

#define NOC_X		17		// GDDR tile NOC coordinates
#define NOC_Y		12
#define NOC_ADDR	0ULL		// tile base address
#define XFER_SIZE	(256ULL << 20)	// bytes written/read/verified (256 MiB)
#define TLB_WINDOW_SIZE	(4ULL << 30)	// 4 GiB BAR4 window covers the tile
#define TLB_ORDERING	0	// Relaxed

#define RDMA_MTU_MAX	IBV_MTU_4096

#define RDMA_CHUNK	(64ULL << 20)	// 64 MiB per work request
#define OUTSTANDING	4		// in-flight work requests
#define SQ_DEPTH	(OUTSTANDING + 8)
#define MAX_RD_ATOMIC	16		// outstanding RDMA reads the QP allows

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

static void report(const char *label, size_t bytes, double dt)
{
	printf("%-16s %5zu MiB in %8.3f ms  (%6.2f GiB/s)\n",
	       label, bytes >> 20, dt * 1e3,
	       (double)bytes / dt / (double)(1ULL << 30));
}

static void fill_random(void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		ssize_t r = getrandom(p, n, 0);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			DIE("getrandom failed: %s", strerror(errno));
		}
		p += r;
		n -= (size_t)r;
	}
}

/* ===== RDMA plumbing ===== */

struct conn {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	uint8_t port;
	int gid_index;
};

// Wire format for the out-of-band TCP handshake. Both nodes are assumed to share
// host byte order (x86-64 / aarch64 little-endian) for simplicity.
struct exch {
	uint32_t rkey;	// server: bh_mr rkey
	uint32_t qpn;
	uint32_t psn;
	uint64_t addr;	// server: bh_mr iova (0)
	uint8_t gid[16];
};

// Read the GID type string for a given index, e.g. "RoCE v2" or "IB/RoCE v1",
// from sysfs. ibv_query_gid() does not expose the RoCE version, so a port that
// has both v1 and v2 GIDs for the same IPv4 address looks identical without it.
static int gid_is_rocev2(const char *rdma_name, uint8_t port, int index)
{
	char path[256], buf[64];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path),
		 "/sys/class/infiniband/%s/ports/%u/gid_attrs/types/%d",
		 rdma_name, port, index);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return strstr(buf, "RoCE v2") != NULL;
}

// Auto-select a RoCEv2 (IPv4-mapped) GID. We must check both the GID value
// (IPv4-mapped prefix) and the GID type, since many ports expose a RoCE v1 and
// a RoCE v2 GID for the same address - picking the v1 one fails to connect.
static int pick_rocev2_gid(struct ibv_context *ctx, const char *rdma_name,
			   uint8_t port)
{
	static const uint8_t v4_mapped_prefix[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
	union ibv_gid g;
	int i;

	for (i = 0; i < 16; i++) {
		if (ibv_query_gid(ctx, port, i, &g))
			break;
		if (memcmp(g.raw, v4_mapped_prefix, sizeof(v4_mapped_prefix)) != 0)
			continue;
		if (gid_is_rocev2(rdma_name, port, i))
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
			struct exch *local)
{
	struct ibv_qp_init_attr ia = {
		.cap = { .max_send_wr = SQ_DEPTH, .max_recv_wr = 4,
			 .max_send_sge = 1, .max_recv_sge = 1 },
		.qp_type = IBV_QPT_RC,
	};
	union ibv_gid gid;

	c->port = 1;
	c->ctx = open_rdma_device(rdma_name);
	c->pd = ibv_alloc_pd(c->ctx);
	if (!c->pd)
		DIE("ibv_alloc_pd failed");

	c->cq = ibv_create_cq(c->ctx, OUTSTANDING + 16, NULL, NULL, 0);
	if (!c->cq)
		DIE("ibv_create_cq failed");

	if (gid_index < 0) {
		gid_index = pick_rocev2_gid(c->ctx, rdma_name, c->port);
		if (gid_index < 0)
			DIE("no RoCEv2 (IPv4-mapped) GID found; pass -g");
	}
	c->gid_index = gid_index;

	if (ibv_query_gid(c->ctx, c->port, c->gid_index, &gid))
		DIE("ibv_query_gid failed");

	ia.send_cq = c->cq;
	ia.recv_cq = c->cq;
	c->qp = ibv_create_qp(c->pd, &ia);
	if (!c->qp)
		DIE("ibv_create_qp failed: %s", strerror(errno));

	memcpy(local->gid, gid.raw, 16);
	local->qpn = c->qp->qp_num;
	local->psn = lrand48() & 0xffffff;
	printf("local:  qpn=0x%x gid_index=%d\n", local->qpn, c->gid_index);
}

static void connect_qp(struct conn *c, const struct exch *local, const struct exch *remote)
{
	enum ibv_mtu mtu = RDMA_MTU_MAX;
	struct ibv_port_attr pa;
	struct ibv_qp_attr init = {
		.qp_state = IBV_QPS_INIT,
		.pkey_index = 0,
		.port_num = c->port,
		.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
				   IBV_ACCESS_REMOTE_READ,
	};
	struct ibv_qp_attr rtr = {
		.qp_state = IBV_QPS_RTR,
		.dest_qp_num = remote->qpn,
		.rq_psn = remote->psn,
		.max_dest_rd_atomic = MAX_RD_ATOMIC,
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
		.sq_psn = local->psn,
		.max_rd_atomic = MAX_RD_ATOMIC,
	};

	// Clamp the RoCE MTU to what the port negotiated, so we never emit packets
	// larger than the Ethernet link can carry (which would just be dropped ->
	// "transport retry counter exceeded").
	if (!ibv_query_port(c->ctx, c->port, &pa) && pa.active_mtu < mtu)
		mtu = pa.active_mtu;
	rtr.path_mtu = mtu;
	printf("path MTU = %d bytes\n", 128 << mtu);

	memcpy(rtr.ah_attr.grh.dgid.raw, remote->gid, 16);

	if (ibv_modify_qp(c->qp, &init, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
					IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
		DIE("modify INIT failed: %s", strerror(errno));
	if (ibv_modify_qp(c->qp, &rtr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
				       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
				       IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
		DIE("modify RTR failed: %s (gid/route issue?)", strerror(errno));
	if (ibv_modify_qp(c->qp, &rts, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
				       IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
		DIE("modify RTS failed: %s", strerror(errno));
}

// Chunked, pipelined RDMA over [0, total). The remote iova base is 0, so
// remote_addr == byte offset. Up to OUTSTANDING work requests are kept in flight.
static void rdma_xfer(struct conn *c, enum ibv_wr_opcode op, struct ibv_mr *mr,
		      uint8_t *host, size_t total, uint64_t remote_base, uint32_t rkey)
{
	size_t off = 0;
	int inflight = 0;

	while (off < total || inflight) {
		while (off < total && inflight < OUTSTANDING) {
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

			if (ibv_post_send(c->qp, &wr, &bad))
				DIE("ibv_post_send failed at off=%zu: %s", off, strerror(errno));
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

static void raise_power(int fd)
{
	struct tenstorrent_power_state ps = { 0 };

	ps.argsz = sizeof(ps);
	ps.validity = TT_POWER_VALIDITY(4, 0);
	ps.power_flags = TT_POWER_FLAG_MAX_AI_CLK | TT_POWER_FLAG_MRISC_PHY_WAKEUP |
			 TT_POWER_FLAG_TENSIX_ENABLE | TT_POWER_FLAG_L2CPU_ENABLE;
	if (ioctl(fd, TENSTORRENT_IOCTL_SET_POWER_STATE, &ps) != 0)
		DIE("SET_POWER_STATE: %s", strerror(errno));
}

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

	raise_power(fd);
	return fd;
}

static int run_server(const char *bh_path, const char *rdma_name, int gid_index)
{
	struct tenstorrent_allocate_tlb alloc = { 0 };
	struct tenstorrent_configure_tlb cfg = { 0 };
	struct tenstorrent_free_tlb ft = { 0 };
	struct tenstorrent_export_tlb_dmabuf exp = { 0 };
	struct conn c = { 0 };
	struct exch local = { 0 }, remote = { 0 };
	struct ibv_mr *bh_mr;
	int bh_fd, dmabuf_fd, sock;
	uint8_t sync;

	bh_fd = open_blackhole(bh_path);

	// Aim a TLB window at the GDDR tile, then export the window as a dma-buf.
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

	printf("server: BH %s window->(x=%u,y=%u,addr=0x%llx) %zu MiB exported\n",
	       bh_path, NOC_X, NOC_Y, (unsigned long long)NOC_ADDR, (size_t)(XFER_SIZE >> 20));

	setup_verbs(&c, rdma_name, gid_index, &local);

	// Register the exported BH window as an RDMA MR. This is the only place the
	// dma-buf fd is used; the server never maps or touches the data itself.
	bh_mr = ibv_reg_dmabuf_mr(c.pd, 0, XFER_SIZE, 0, dmabuf_fd,
				  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
				  IBV_ACCESS_REMOTE_READ);
	if (!bh_mr)
		DIE("ibv_reg_dmabuf_mr failed: %s", strerror(errno));
	local.rkey = bh_mr->rkey;
	local.addr = 0;	// iova
	printf("server: bh_mr rkey=0x%x\n", bh_mr->rkey);

	sock = tcp_listen_accept();
	rw_all(sock, &remote, sizeof(remote), 0);	// recv client's QP info
	rw_all(sock, &local, sizeof(local), 1);		// send ours (incl rkey/addr)
	connect_qp(&c, &local, &remote);
	printf("server: connected to client (qpn=0x%x)\n", remote.qpn);

	// The client drives all DMA; we just wait for it to report completion.
	rw_all(sock, &sync, 1, 0);
	printf("server: client reported DMA round-trip verified\n");
	printf("\nNIC <-> Blackhole DMA round-trip verified over the fabric.\n");

	close(sock);
	ibv_dereg_mr(bh_mr);
	close(dmabuf_fd);
	ft.in.id = alloc.out.id;
	ioctl(bh_fd, TENSTORRENT_IOCTL_FREE_TLB, &ft);
	close(bh_fd);
	return 0;
}

/* ===== peer side (client only) - drives all DMA ===== */

static int run_client(const char *server_ip, const char *rdma_name, int gid_index)
{
	struct conn c = { 0 };
	struct exch local = { 0 }, remote = { 0 };
	struct ibv_mr *src_mr, *dst_mr;
	uint64_t *src, *dst;
	size_t nwords = XFER_SIZE / sizeof(uint64_t);
	size_t j;
	int sock;
	uint8_t sync = 1;
	double t0, t1;

	setup_verbs(&c, rdma_name, gid_index, &local);

	// Two registered DMA buffers: src is written into Blackhole, dst is filled
	// back from Blackhole. After a correct round-trip they are identical.
	src = aligned_alloc(4096, XFER_SIZE);
	dst = aligned_alloc(4096, XFER_SIZE);
	if (!src || !dst)
		DIE("alloc(%zu) failed", (size_t)XFER_SIZE);
	src_mr = ibv_reg_mr(c.pd, src, XFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
	dst_mr = ibv_reg_mr(c.pd, dst, XFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
	if (!src_mr || !dst_mr)
		DIE("ibv_reg_mr failed: %s", strerror(errno));

	sock = tcp_connect(server_ip);
	rw_all(sock, &local, sizeof(local), 1);		// send our QP info
	rw_all(sock, &remote, sizeof(remote), 0);	// recv server's (incl rkey/addr)
	connect_qp(&c, &local, &remote);
	printf("client: connected to server (qpn=0x%x, bh rkey=0x%x)\n",
	       remote.qpn, remote.rkey);

	// NIC -> BH: DMA a fresh random pattern from src into Blackhole GDDR.
	fill_random(src, XFER_SIZE);
	t0 = now_sec();
	rdma_xfer(&c, IBV_WR_RDMA_WRITE, src_mr, (uint8_t *)src, XFER_SIZE, remote.addr, remote.rkey);
	t1 = now_sec();
	report("DMA write (NIC->BH)", XFER_SIZE, t1 - t0);

	// BH -> NIC: DMA it back out into dst and verify src and dst now match.
	// Zero dst first so a short/silent read can't leave stale data that matches.
	memset(dst, 0, XFER_SIZE);
	t0 = now_sec();
	rdma_xfer(&c, IBV_WR_RDMA_READ, dst_mr, (uint8_t *)dst, XFER_SIZE, remote.addr, remote.rkey);
	t1 = now_sec();
	report("DMA read  (BH->NIC)", XFER_SIZE, t1 - t0);

	for (j = 0; j < nwords; j++)
		if (dst[j] != src[j])
			DIE("verify FAILED at word %zu: got=0x%016llx want=0x%016llx",
			    j, (unsigned long long)dst[j], (unsigned long long)src[j]);
	printf("client: round-trip verified (src and dst identical)\n");

	rw_all(sock, &sync, 1, 1);	// tell server "all verified"

	close(sock);
	ibv_dereg_mr(src_mr);
	ibv_dereg_mr(dst_mr);
	free(src);
	free(dst);
	return 0;
}

int main(int argc, char **argv)
{
	const char *bh_path = NULL;
	const char *rdma_name = NULL;
	int gid_index = -1;
	const char *peer;
	int opt;

	while ((opt = getopt(argc, argv, "b:r:g:h")) != -1) {
		switch (opt) {
		case 'b': bh_path = optarg; break;
		case 'r': rdma_name = optarg; break;
		case 'g': gid_index = atoi(optarg); break;
		default:
			printf("usage:\n"
			       "  server: %s -b /dev/tenstorrent/N -r rdma_dev [-g gid]\n"
			       "  client: %s -r rdma_dev [-g gid] <server_ip>\n",
			       argv[0], argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}

	srand48(getpid());
	peer = (optind < argc) ? argv[optind] : NULL;

	if (!rdma_name)
		DIE("missing -r rdma_dev (e.g. -r rocep201s0f0)");

	if (peer)
		return run_client(peer, rdma_name, gid_index);

	if (!bh_path)
		DIE("missing -b /dev/tenstorrent/N on the Blackhole (server) node");
	return run_server(bh_path, rdma_name, gid_index);
}
