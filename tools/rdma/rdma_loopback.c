// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// rdma_loopback - a teaching harness for RDMA verbs (Phase 0a).
//
// Connects two RC queue pairs to each other on a single port (loopback)
// and uses one-sided RDMA WRITE and READ to move bytes between two
// locally-registered memory regions.  No second machine, no fabric
// config: develop it against Soft-RoCE (rdma_rxe) on any box.
//
//   sudo modprobe rdma_rxe
//   sudo rdma link add rxe0 type rxe netdev <ethdev>
//   ibv_devinfo                 # confirm rxe0 shows up
//
// The point of this tool is to make the verbs lifecycle legible:
//   open device -> alloc PD -> create CQ -> register MRs ->
//   create QPs -> INIT -> RTR -> RTS -> post_send(WRITE/READ) -> poll.
//
// Why it matters for tt-kmd: in Phase 2 the "TARGET" memory region
// below is the only thing that changes.  Instead of a host buffer
// registered with ibv_reg_mr(), you register a Blackhole aperture with
// ibv_reg_dmabuf_mr(pd, 0, len, iova, dmabuf_fd, access).  Everything
// else in this file - QP setup, the WRITE/READ work requests, the
// completion polling - stays identical.  So learn it here, cheaply.
// The TARGET region is deliberately factored out and marked.
//
// To compile:
//   make            # or: gcc -O2 -Wall -Wextra -o rdma_loopback rdma_loopback.c -libverbs
//
// To run:
//   ./rdma_loopback              # first device, gid index 0
//   ./rdma_loopback -d rxe0 -g 1 # pick device and gid index

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#define BUF_SIZE 4096
#define WR_ID_WRITE 1
#define WR_ID_READ 2

#define DIE(fmt, ...) \
	do { fprintf(stderr, "error: " fmt "\n", ##__VA_ARGS__); exit(1); } while (0)

// Everything we need to tell the other side of an RC connection about
// "us" so it can target its work requests and address handle at us.
// In real life this is swapped over a socket; here both QPs are local,
// so we just pass the struct around in-process.
struct endpoint {
	uint32_t qpn;	// queue pair number
	uint32_t psn;	// initial packet sequence number
	uint16_t lid;	// local id (0 on RoCE/Ethernet)
	union ibv_gid gid;	// RoCE address
};

struct conn {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	uint8_t port;
	int gid_index;
};

static struct ibv_context *open_device(const char *name)
{
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	int n, i;

	list = ibv_get_device_list(&n);
	if (!list || n == 0)
		DIE("no RDMA devices found (did you `modprobe rdma_rxe` and `rdma link add`?)");

	for (i = 0; i < n; i++) {
		const char *dev_name = ibv_get_device_name(list[i]);
		if (!name || strcmp(dev_name, name) == 0) {
			ctx = ibv_open_device(list[i]);
			if (!ctx)
				DIE("ibv_open_device(%s) failed", dev_name);
			printf("Using device: %s\n", dev_name);
			break;
		}
	}

	ibv_free_device_list(list);
	if (!ctx)
		DIE("device %s not found", name ? name : "(first)");
	return ctx;
}

// Print the gid table so the user learns where RoCEv2 entries live;
// the right gid index is the #1 footgun on first contact.
static void dump_port(struct conn *c)
{
	struct ibv_port_attr pa;
	int i;

	if (ibv_query_port(c->ctx, c->port, &pa))
		DIE("ibv_query_port failed");

	printf("Port %u: state=%d link_layer=%s lid=%u\n", c->port, pa.state,
	       pa.link_layer == IBV_LINK_LAYER_ETHERNET ? "Ethernet(RoCE)" :
	       pa.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" : "?",
	       pa.lid);

	for (i = 0; i < 4; i++) {
		union ibv_gid g;
		if (ibv_query_gid(c->ctx, c->port, i, &g) == 0) {
			uint8_t *r = g.raw;
			printf("  gid[%d] = %02x%02x:%02x%02x:%02x%02x:%02x%02x:"
			       "%02x%02x:%02x%02x:%02x%02x:%02x%02x%s\n", i,
			       r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
			       r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15],
			       i == c->gid_index ? "  <- using" : "");
		}
	}
}

static void create_qp(struct conn *c)
{
	struct ibv_qp_init_attr ia = {
		.send_cq = c->cq,
		.recv_cq = c->cq,
		.cap = {
			.max_send_wr = 16,
			.max_recv_wr = 16,
			.max_send_sge = 1,
			.max_recv_sge = 1,
		},
		.qp_type = IBV_QPT_RC,
		.sq_sig_all = 0,
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
		// Permit remote peers to WRITE/READ our MRs over this QP.
		.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
				   IBV_ACCESS_REMOTE_WRITE |
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
			.is_global = 1,	// RoCE always uses the global routing header
			.dlid = remote->lid,
			.sl = 0,
			.src_path_bits = 0,
			.port_num = c->port,
			.grh = {
				.dgid = remote->gid,
				.sgid_index = c->gid_index,
				.hop_limit = 1,
			},
		},
	};

	if (ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_AV |
					IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
					IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
					IBV_QP_MIN_RNR_TIMER))
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

	if (ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
					IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
					IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
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

// One-sided RDMA op: move `len` bytes between a local MR and a remote MR.
// For WRITE: local sge is the source, remote {addr,rkey} is the sink.
// For READ:  local sge is the sink,   remote {addr,rkey} is the source.
static void post_and_wait(struct conn *c, enum ibv_wr_opcode opcode, uint64_t wr_id,
			  struct ibv_mr *local_mr, void *local_buf, size_t len,
			  uint64_t remote_addr, uint32_t remote_rkey)
{
	struct ibv_sge sge = {
		.addr = (uintptr_t)local_buf,
		.length = len,
		.lkey = local_mr->lkey,
	};
	struct ibv_send_wr wr = {
		.wr_id = wr_id,
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
		DIE("completion error: %s (wr_id=%llu)",
		    ibv_wc_status_str(wc.status), (unsigned long long)wc.wr_id);
}

int main(int argc, char **argv)
{
	const char *dev_name = NULL;
	struct conn ca = { .port = 1, .gid_index = 0 };
	struct conn cb = { .port = 1, .gid_index = 0 };
	struct endpoint ea, eb;
	struct ibv_context *ctx;
	struct ibv_mr *src_mr, *tgt_mr;
	char *src, *tgt;
	size_t len = BUF_SIZE;
	int opt;
	int gid_index = 0;
	uint8_t port = 1;

	while ((opt = getopt(argc, argv, "d:g:p:s:h")) != -1) {
		switch (opt) {
		case 'd': dev_name = optarg; break;
		case 'g': gid_index = atoi(optarg); break;
		case 'p': port = (uint8_t)atoi(optarg); break;
		case 's': len = strtoul(optarg, NULL, 0); break;
		default:
			printf("usage: %s [-d device] [-g gid_index] [-p port] [-s size]\n", argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}

	srand48(getpid());

	ctx = open_device(dev_name);
	ca.ctx = cb.ctx = ctx;
	ca.port = cb.port = port;
	ca.gid_index = cb.gid_index = gid_index;

	dump_port(&ca);

	// Protection domain: the bucket that ties our MRs and QPs together.
	// A QP can only touch MRs registered in the same PD.
	ca.pd = cb.pd = ibv_alloc_pd(ctx);
	if (!ca.pd)
		DIE("ibv_alloc_pd failed");

	ca.cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	cb.cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!ca.cq || !cb.cq)
		DIE("ibv_create_cq failed");

	// Two host buffers + MRs.  `src` is the source of the WRITE / sink of
	// the READ.  `tgt` is the TARGET.
	//
	// >>> PHASE 2 SWAP POINT <<<
	// In the real integration, `tgt`/`tgt_mr` becomes the Blackhole
	// aperture: replace the malloc + ibv_reg_mr below with
	//   int fd = <tt-kmd export ioctl>;
	//   tgt_mr = ibv_reg_dmabuf_mr(ca.pd, 0, len, /*iova*/ 0, fd, access);
	// and set remote_addr = iova (0).  Nothing else changes.
	src = aligned_alloc(4096, len);
	tgt = aligned_alloc(4096, len);
	if (!src || !tgt)
		DIE("aligned_alloc failed");

	src_mr = ibv_reg_mr(ca.pd, src, len, IBV_ACCESS_LOCAL_WRITE);
	tgt_mr = ibv_reg_mr(ca.pd, tgt, len, IBV_ACCESS_LOCAL_WRITE |
						IBV_ACCESS_REMOTE_WRITE |
						IBV_ACCESS_REMOTE_READ);
	if (!src_mr || !tgt_mr)
		DIE("ibv_reg_mr failed: %s", strerror(errno));

	create_qp(&ca);
	create_qp(&cb);

	// Each QP needs the other's qpn/psn/gid to connect.  Same process,
	// so we just read them out directly instead of socket exchange.
	fill_endpoint(&ca, &ea);
	fill_endpoint(&cb, &eb);

	to_init(&ca);
	to_init(&cb);
	to_rtr(&ca, &eb);	// A points at B
	to_rtr(&cb, &ea);	// B points at A
	to_rts(&ca, &ea);
	to_rts(&cb, &eb);

	printf("Connected QPs: A=0x%x <-> B=0x%x\n", ea.qpn, eb.qpn);

	// --- Test 1: RDMA WRITE (the "NIC writes the target" direction) ---
	memset(src, 0xAB, len);
	memset(tgt, 0x00, len);

	post_and_wait(&ca, IBV_WR_RDMA_WRITE, WR_ID_WRITE, src_mr, src, len,
		      (uintptr_t)tgt, tgt_mr->rkey);

	if (memcmp(src, tgt, len) != 0)
		DIE("WRITE verify FAILED: target does not match source");
	printf("RDMA WRITE ok: %zu bytes src -> target verified\n", len);

	// --- Test 2: RDMA READ (the "NIC reads the target" direction) ---
	memset(tgt, 0xCD, len);
	memset(src, 0x00, len);

	post_and_wait(&ca, IBV_WR_RDMA_READ, WR_ID_READ, src_mr, src, len,
		      (uintptr_t)tgt, tgt_mr->rkey);

	if (memcmp(src, tgt, len) != 0)
		DIE("READ verify FAILED: local does not match target");
	printf("RDMA READ  ok: %zu bytes target -> src verified\n", len);

	printf("\nAll good. This is the exact flow you'll point at a Blackhole\n"
	       "aperture in Phase 2 (see the PHASE 2 SWAP POINT comment).\n");

	ibv_destroy_qp(ca.qp);
	ibv_destroy_qp(cb.qp);
	ibv_dereg_mr(src_mr);
	ibv_dereg_mr(tgt_mr);
	ibv_destroy_cq(ca.cq);
	ibv_destroy_cq(cb.cq);
	ibv_dealloc_pd(ca.pd);
	ibv_close_device(ctx);
	free(src);
	free(tgt);
	return 0;
}
