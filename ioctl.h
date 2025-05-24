// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note

#ifndef TTDRIVER_IOCTL_H_INCLUDED
#define TTDRIVER_IOCTL_H_INCLUDED

#include <linux/types.h>
#include <linux/ioctl.h>

#define TENSTORRENT_DRIVER_VERSION 2

#define TENSTORRENT_IOCTL_MAGIC 0xFA

#define TENSTORRENT_IOCTL_GET_DEVICE_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_GET_HARVESTING	_IO(TENSTORRENT_IOCTL_MAGIC, 1)
#define TENSTORRENT_IOCTL_QUERY_MAPPINGS	_IO(TENSTORRENT_IOCTL_MAGIC, 2)
#define TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF	_IO(TENSTORRENT_IOCTL_MAGIC, 3)
#define TENSTORRENT_IOCTL_FREE_DMA_BUF		_IO(TENSTORRENT_IOCTL_MAGIC, 4)
#define TENSTORRENT_IOCTL_GET_DRIVER_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 5)
#define TENSTORRENT_IOCTL_RESET_DEVICE		_IO(TENSTORRENT_IOCTL_MAGIC, 6)
#define TENSTORRENT_IOCTL_PIN_PAGES		_IO(TENSTORRENT_IOCTL_MAGIC, 7)
#define TENSTORRENT_IOCTL_LOCK_CTL		_IO(TENSTORRENT_IOCTL_MAGIC, 8)
#define TENSTORRENT_IOCTL_MAP_PEER_BAR		_IO(TENSTORRENT_IOCTL_MAGIC, 9)
#define TENSTORRENT_IOCTL_UNPIN_PAGES		_IO(TENSTORRENT_IOCTL_MAGIC, 10)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 11)
#define TENSTORRENT_IOCTL_FREE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 12)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 13)

// For tenstorrent_mapping.mapping_id. These are not array indices.
#define TENSTORRENT_MAPPING_UNUSED		0
#define TENSTORRENT_MAPPING_RESOURCE0_UC	1
#define TENSTORRENT_MAPPING_RESOURCE0_WC	2
#define TENSTORRENT_MAPPING_RESOURCE1_UC	3
#define TENSTORRENT_MAPPING_RESOURCE1_WC	4
#define TENSTORRENT_MAPPING_RESOURCE2_UC	5
#define TENSTORRENT_MAPPING_RESOURCE2_WC	6

#define TENSTORRENT_MAX_DMA_BUFS	256
#define TENSTORRENT_MAX_INBOUND_TLBS	256

#define TENSTORRENT_RESOURCE_LOCK_COUNT 64

struct tenstorrent_get_device_info_in {
	__u32 output_size_bytes;
};

struct tenstorrent_get_device_info_out {
	__u32 output_size_bytes;
	__u16 vendor_id;
	__u16 device_id;
	__u16 subsystem_vendor_id;
	__u16 subsystem_id;
	__u16 bus_dev_fn;	// [0:2] function, [3:7] device, [8:15] bus
	__u16 max_dma_buf_size_log2;	// Since 1.0
	__u16 pci_domain;		// Since 1.23
};

struct tenstorrent_get_device_info {
	struct tenstorrent_get_device_info_in in;
	struct tenstorrent_get_device_info_out out;
};

struct tenstorrent_query_mappings_in {
	__u32 output_mapping_count;
	__u32 reserved;
};

struct tenstorrent_mapping {
	__u32 mapping_id;
	__u32 reserved;
	__u64 mapping_base;
	__u64 mapping_size;
};

struct tenstorrent_query_mappings_out {
	struct tenstorrent_mapping mappings[0];
};

struct tenstorrent_query_mappings {
	struct tenstorrent_query_mappings_in in;
	struct tenstorrent_query_mappings_out out;
};

// tenstorrent_allocate_dma_buf_in.flags
#define TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA 2

struct tenstorrent_allocate_dma_buf_in {
	__u32 requested_size;
	__u8  buf_index;	// [0,TENSTORRENT_MAX_DMA_BUFS)
	__u8  flags;
	__u8  reserved0[2];
	__u64 reserved1[2];
};

struct tenstorrent_allocate_dma_buf_out {
	__u64 physical_address;	// or IOVA
	__u64 mapping_offset;
	__u32 size;
	__u32 reserved0;
	__u64 noc_address;	// valid if TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA is set
	__u64 reserved1;
};

struct tenstorrent_allocate_dma_buf {
	struct tenstorrent_allocate_dma_buf_in in;
	struct tenstorrent_allocate_dma_buf_out out;
};

struct tenstorrent_free_dma_buf_in {
};

struct tenstorrent_free_dma_buf_out {
};

struct tenstorrent_free_dma_buf {
	struct tenstorrent_free_dma_buf_in in;
	struct tenstorrent_free_dma_buf_out out;
};

struct tenstorrent_get_driver_info_in {
	__u32 output_size_bytes;
};

struct tenstorrent_get_driver_info_out {
	__u32 output_size_bytes;
	__u32 driver_version;
};

struct tenstorrent_get_driver_info {
	struct tenstorrent_get_driver_info_in in;
	struct tenstorrent_get_driver_info_out out;
};

// tenstorrent_reset_device_in.flags
#define TENSTORRENT_RESET_DEVICE_RESTORE_STATE 0
#define TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK 1
#define TENSTORRENT_RESET_DEVICE_CONFIG_WRITE 2

struct tenstorrent_reset_device_in {
	__u32 output_size_bytes;
	__u32 flags;
};

struct tenstorrent_reset_device_out {
	__u32 output_size_bytes;
	__u32 result;
};

struct tenstorrent_reset_device {
	struct tenstorrent_reset_device_in in;
	struct tenstorrent_reset_device_out out;
};

// tenstorrent_pin_pages_in.flags
#define TENSTORRENT_PIN_PAGES_CONTIGUOUS 1	// app attests that the pages are physically contiguous
#define TENSTORRENT_PIN_PAGES_NOC_DMA 2		// app wants to use the pages for NOC DMA
#define TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN 4	// NOC DMA will be allocated top-down (default is bottom-up)

struct tenstorrent_pin_pages_in {
	__u32 output_size_bytes;
	__u32 flags;
	__u64 virtual_address;
	__u64 size;
};

struct tenstorrent_pin_pages_out {
	__u64 physical_address;	// or IOVA
};

struct tenstorrent_pin_pages_out_extended {
	__u64 physical_address;	// or IOVA
	__u64 noc_address;
};

// unpinning subset of a pinned buffer is not supported
struct tenstorrent_unpin_pages_in {
	__u64 virtual_address;	// original VA used to pin, not current VA if remapped
	__u64 size;
	__u64 reserved;
};

struct tenstorrent_unpin_pages_out {
};

struct tenstorrent_unpin_pages {
	struct tenstorrent_unpin_pages_in in;
	struct tenstorrent_unpin_pages_out out;
};

struct tenstorrent_pin_pages {
	struct tenstorrent_pin_pages_in in;
	struct tenstorrent_pin_pages_out out;
};

// tenstorrent_lock_ctl_in.flags
#define TENSTORRENT_LOCK_CTL_ACQUIRE 0
#define TENSTORRENT_LOCK_CTL_RELEASE 1
#define TENSTORRENT_LOCK_CTL_TEST 2

struct tenstorrent_lock_ctl_in {
	__u32 output_size_bytes;
	__u32 flags;
	__u8  index;
};

struct tenstorrent_lock_ctl_out {
	__u8 value;
};

struct tenstorrent_lock_ctl {
	struct tenstorrent_lock_ctl_in in;
	struct tenstorrent_lock_ctl_out out;
};

struct tenstorrent_map_peer_bar_in {
	__u32 peer_fd;
	__u32 peer_bar_index;
	__u32 peer_bar_offset;
	__u32 peer_bar_length;
	__u32 flags;
};

struct tenstorrent_map_peer_bar_out {
	__u64 dma_address;
	__u64 reserved;
};

struct tenstorrent_map_peer_bar {
	struct tenstorrent_map_peer_bar_in in;
	struct tenstorrent_map_peer_bar_out out;
};

struct tenstorrent_allocate_tlb_in {
	__u64 size;
	__u64 reserved;
};

struct tenstorrent_allocate_tlb_out {
	__u32 id;
	__u32 reserved0;
	__u64 mmap_offset_uc;
	__u64 mmap_offset_wc;
	__u64 reserved1;
};

struct tenstorrent_allocate_tlb {
	struct tenstorrent_allocate_tlb_in in;
	struct tenstorrent_allocate_tlb_out out;
};

struct tenstorrent_free_tlb_in {
	__u32 id;
};

struct tenstorrent_free_tlb_out {
};

struct tenstorrent_free_tlb {
	struct tenstorrent_free_tlb_in in;
	struct tenstorrent_free_tlb_out out;
};

struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end;
	__u16 y_end;
	__u16 x_start;
	__u16 y_start;
	__u8 noc;
	__u8 mcast;
	__u8 ordering;
	__u8 linked;
	__u8 static_vc;
	__u8 reserved0[3];
	__u32 reserved1[2];
};

struct tenstorrent_configure_tlb_in {
	__u32 id;
	struct tenstorrent_noc_tlb_config config;
};

struct tenstorrent_configure_tlb_out {
	__u64 reserved;
};

struct tenstorrent_configure_tlb {
	struct tenstorrent_configure_tlb_in in;
	struct tenstorrent_configure_tlb_out out;
};

#endif
