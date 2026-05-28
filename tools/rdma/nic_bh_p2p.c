// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// nic_bh_p2p - two-node RDMA into Blackhole DRAM (Milestone 2, for real).
//
// Single binary, two roles:
//   server (no peer arg): runs on the Blackhole node. Exports a TLB window
//     aimed at a DRAM tile as a dma-buf, registers it on the local RDMA NIC
//     with ibv_reg_dmabuf_mr(), publishes {qpn,psn,gid,addr,rkey} over TCP,
//     and after the client drives traffic, verifies the DRAM via a CPU
//     mapping of the same window.
//   client (peer arg = server IP): runs on any other RoCE node. Connects,
//     RDMA WRITEs a known pattern into the Blackhole aperture, then RDMA
//     READs it back. The NIC is the bus master; Blackhole is the target.
//
// Only the server needs a Blackhole, the dma-buf-capable rdma-core, and the
// tt-kmd export ioctl. The client just registers host memory.
//
// RoCEv2 is routable, so the two nodes need not share an L2 subnet.
//
// To compile:  make    (links libibverbs)
// To run:
//   # on the Blackhole node (e.g. 10.250.36.78):
//   sudo ./nic_bh_p2p -b /dev/tenstorrent/23 -r bnxt_re0
//   # on a peer RoCE node:
//   ./nic_bh_p2p -r bnxt_re0 10.250.36.78

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ===== sizes / constants ===== */

#define TLB_SIZE_4G	(4ULL << 30)
#define EXPORT_SIZE	(2ULL << 20)
#define MR_LEN		(64 * 1024)
#define TCP_PORT	18515

#define DIE(fmt, ...) \
	do { fprintf(stderr, "error: " fmt "\n", ##__VA_ARGS__); exit(1); } while (0)

// Deterministic patterns both sides agree on (no need to ship the data).
static void pattern_write(uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(0x40 + (i & 0x3f)); }
static void pattern_read(uint8_t *b, size_t n)  { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(0x80 + (i & 0x3f)); }

/* ===== RDMA helpers ===== */

struct conn {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	uint8_t port;
	int gid_index;
};

// Wire format for the out-of-band TCP handshake. Both nodes are x86-64;
// fields are exchanged in host byte order for simplicity.
struct exch {
	uint32_t qpn;
	uint32_t psn;
	uint32_t rkey;	// server: bh_mr rkey (unused by server's own QP)
	uint32_t pad;
	uint64_t addr;	// server: bh_mr iova (0)
	uint8_t gid[16];
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

static void setup_verbs(struct conn *c, const char *rdma_name, int gid_index, struct exch *local)
{
	struct ibv_qp_init_attr ia = {
		.cap = { .max_send_wr = 16, .max_recv_wr = 16,
			 .max_send_sge = 1, .max_recv_sge = 1 },
		.qp_type = IBV_QPT_RC,
	};
	union ibv_gid gid;

	c->port = 1;
	c->ctx = open_rdma_device(rdma_name);
	c->pd = ibv_alloc_pd(c->ctx);
	if (!c->pd)
		DIE("ibv_alloc_pd failed");
	c->cq = ibv_create_cq(c->ctx, 16, NULL, NULL, 0);
	if (!c->cq)
		DIE("ibv_create_cq failed");

	if (gid_index < 0) {
		gid_index = pick_rocev2_gid(c->ctx, c->port);
		if (gid_index < 0)
			DIE("no RoCEv2 (IPv4-mapped) GID found; pass -g");
	}
	c->gid_index = gid_index;

	ia.send_cq = c->cq;
	ia.recv_cq = c->cq;
	c->qp = ibv_create_qp(c->pd, &ia);
	if (!c->qp)
		DIE("ibv_create_qp failed: %s", strerror(errno));

	if (ibv_query_gid(c->ctx, c->port, c->gid_index, &gid))
		DIE("ibv_query_gid failed");

	local->qpn = c->qp->qp_num;
	local->psn = lrand48() & 0xffffff;
	memcpy(local->gid, gid.raw, 16);
	printf("local:  qpn=0x%x psn=0x%x gid_index=%d\n", local->qpn, local->psn, c->gid_index);
}

static void connect_qp(struct conn *c, const struct exch *local, const struct exch *remote)
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
		.path_mtu = IBV_MTU_1024,
		.dest_qp_num = remote->qpn,
		.rq_psn = remote->psn,
		.max_dest_rd_atomic = 1,
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
		.max_rd_atomic = 1,
	};

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

static void rdma_op(struct conn *c, enum ibv_wr_opcode opcode, struct ibv_mr *mr,
		    void *buf, size_t len, uint64_t remote_addr, uint32_t rkey)
{
	struct ibv_sge sge = { .addr = (uintptr_t)buf, .length = len, .lkey = mr->lkey };
	struct ibv_send_wr wr = {
		.wr_id = 1, .sg_list = &sge, .num_sge = 1,
		.opcode = opcode, .send_flags = IBV_SEND_SIGNALED,
		.wr.rdma.remote_addr = remote_addr, .wr.rdma.rkey = rkey,
	};
	struct ibv_send_wr *bad = NULL;
	struct ibv_wc wc;
	int n;

	if (ibv_post_send(c->qp, &wr, &bad))
		DIE("ibv_post_send failed: %s", strerror(errno));
	do { n = ibv_poll_cq(c->cq, 1, &wc); } while (n == 0);
	if (n < 0)
		DIE("ibv_poll_cq failed");
	if (wc.status != IBV_WC_SUCCESS)
		DIE("completion error: %s", ibv_wc_status_str(wc.status));
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

static int run_server(const char *bh_path, const char *rdma_name, int gid_index,
		      uint16_t noc_x, uint16_t noc_y, uint64_t noc_addr)
{
	struct tenstorrent_allocate_tlb alloc = { 0 };
	struct tenstorrent_configure_tlb cfg = { 0 };
	struct tenstorrent_free_tlb ft = { 0 };
	struct tenstorrent_export_tlb_dmabuf exp = { 0 };
	struct conn c = { 0 };
	struct exch local = { 0 }, remote = { 0 };
	struct ibv_mr *bh_mr;
	volatile uint8_t *bh_cpu;
	uint8_t expect[MR_LEN];
	void *mmio;
	int bh_fd, dmabuf_fd, sock;
	uint8_t sync;
	size_t i;

	bh_fd = open_blackhole(bh_path);

	alloc.in.size = TLB_SIZE_4G;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
		DIE("ALLOCATE_TLB(4G): %s", strerror(errno));
	cfg.in.id = alloc.out.id;
	cfg.in.config.addr = noc_addr;
	cfg.in.config.x_end = noc_x;
	cfg.in.config.y_end = noc_y;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cfg) != 0)
		DIE("CONFIGURE_TLB: %s", strerror(errno));

	exp.argsz = sizeof(exp);
	exp.tlb_id = alloc.out.id;
	exp.size = EXPORT_SIZE;
	if (ioctl(bh_fd, TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF, &exp) != 0)
		DIE("EXPORT_TLB_DMABUF: %s", strerror(errno));
	dmabuf_fd = exp.fd;

	mmio = mmap(NULL, EXPORT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
	if (mmio == MAP_FAILED)
		DIE("mmap dma-buf: %s", strerror(errno));
	bh_cpu = mmio;

	printf("server: BH %s window->(x=%u,y=%u,addr=0x%llx) exported\n",
	       bh_path, noc_x, noc_y, (unsigned long long)noc_addr);

	setup_verbs(&c, rdma_name, gid_index, &local);
	bh_mr = ibv_reg_dmabuf_mr(c.pd, 0, MR_LEN, 0, dmabuf_fd,
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
	printf("server: connected to client qpn=0x%x\n", remote.qpn);

	// Phase 1 (NIC writes BH): clear DRAM, wait for the client's WRITE.
	for (i = 0; i < MR_LEN; i++)
		bh_cpu[i] = 0;
	rw_all(sock, &sync, 1, 0);	// client signals "write done"

	pattern_write(expect, MR_LEN);
	for (i = 0; i < MR_LEN; i++)
		if (bh_cpu[i] != expect[i])
			DIE("WRITE verify FAILED at %zu: bh=0x%02x want=0x%02x",
			    i, bh_cpu[i], expect[i]);
	printf("server: RDMA WRITE verified - client NIC wrote %d bytes into BH DRAM\n", MR_LEN);

	// Phase 2 (NIC reads BH): seed DRAM, fence, tell client to read.
	pattern_read((uint8_t *)bh_cpu, MR_LEN);
	(void)bh_cpu[0];	// UC read-back fences the CPU writes into DRAM
	rw_all(sock, &sync, 1, 1);	// "read pattern ready"
	rw_all(sock, &sync, 1, 0);	// client signals "read verified"
	printf("server: client reported RDMA READ verified\n");
	printf("\nNIC <-> Blackhole peer-to-peer RDMA works (over the fabric).\n");

	close(sock);
	ibv_dereg_mr(bh_mr);
	munmap(mmio, EXPORT_SIZE);
	close(dmabuf_fd);
	ft.in.id = alloc.out.id;
	ioctl(bh_fd, TENSTORRENT_IOCTL_FREE_TLB, &ft);
	close(bh_fd);
	return 0;
}

/* ===== peer side (client only) ===== */

static int run_client(const char *server_ip, const char *rdma_name, int gid_index)
{
	struct conn c = { 0 };
	struct exch local = { 0 }, remote = { 0 };
	struct ibv_mr *host_mr;
	uint8_t *host, expect[MR_LEN];
	int sock;
	uint8_t sync = 1;
	size_t i;

	setup_verbs(&c, rdma_name, gid_index, &local);

	host = aligned_alloc(4096, MR_LEN);
	if (!host)
		DIE("aligned_alloc failed");
	host_mr = ibv_reg_mr(c.pd, host, MR_LEN, IBV_ACCESS_LOCAL_WRITE);
	if (!host_mr)
		DIE("ibv_reg_mr failed: %s", strerror(errno));

	sock = tcp_connect(server_ip);
	rw_all(sock, &local, sizeof(local), 1);		// send our QP info
	rw_all(sock, &remote, sizeof(remote), 0);	// recv server's (incl rkey/addr)
	connect_qp(&c, &local, &remote);
	printf("client: connected to server qpn=0x%x (bh rkey=0x%x)\n", remote.qpn, remote.rkey);

	// Phase 1: RDMA WRITE a known pattern into the BH aperture.
	pattern_write(host, MR_LEN);
	rdma_op(&c, IBV_WR_RDMA_WRITE, host_mr, host, MR_LEN, remote.addr, remote.rkey);
	printf("client: RDMA WRITE %d bytes into BH posted+completed\n", MR_LEN);
	rw_all(sock, &sync, 1, 1);	// tell server "write done"

	// Phase 2: wait for server to seed DRAM, then RDMA READ it back.
	rw_all(sock, &sync, 1, 0);	// "read pattern ready"
	memset(host, 0, MR_LEN);
	rdma_op(&c, IBV_WR_RDMA_READ, host_mr, host, MR_LEN, remote.addr, remote.rkey);

	pattern_read(expect, MR_LEN);
	for (i = 0; i < MR_LEN; i++)
		if (host[i] != expect[i])
			DIE("READ verify FAILED at %zu: got=0x%02x want=0x%02x",
			    i, host[i], expect[i]);
	printf("client: RDMA READ verified - NIC read %d bytes from BH DRAM\n", MR_LEN);
	rw_all(sock, &sync, 1, 1);	// tell server "read verified"

	close(sock);
	ibv_dereg_mr(host_mr);
	free(host);
	return 0;
}

int main(int argc, char **argv)
{
	const char *bh_path = "/dev/tenstorrent/0";
	const char *rdma_name = "bnxt_re0";
	int gid_index = -1;
	uint16_t noc_x = 17, noc_y = 12;
	uint64_t noc_addr = 0;
	const char *peer;
	int opt;

	while ((opt = getopt(argc, argv, "b:r:g:x:y:a:h")) != -1) {
		switch (opt) {
		case 'b': bh_path = optarg; break;
		case 'r': rdma_name = optarg; break;
		case 'g': gid_index = atoi(optarg); break;
		case 'x': noc_x = (uint16_t)atoi(optarg); break;
		case 'y': noc_y = (uint16_t)atoi(optarg); break;
		case 'a': noc_addr = strtoull(optarg, NULL, 0); break;
		default:
			printf("usage:\n"
			       "  server: %s -b /dev/tenstorrent/N -r rdma_dev [-g gid] [-x X -y Y -a addr]\n"
			       "  client: %s -r rdma_dev <server_ip>\n", argv[0], argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}

	srand48(getpid());
	peer = (optind < argc) ? argv[optind] : NULL;

	if (peer)
		return run_client(peer, rdma_name, gid_index);
	return run_server(bh_path, rdma_name, gid_index, noc_x, noc_y, noc_addr);
}
