// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_IOCTL_H_INCLUDED
#define TTDRIVER_IOCTL_H_INCLUDED

#include <linux/types.h>
#include <linux/ioctl.h>

#define TENSTORRENT_DRIVER_VERSION 1

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
#define TENSTORRENT_IOCTL_HUGEPAGE_SETUP	_IO(TENSTORRENT_IOCTL_MAGIC, 9)

// For tenstorrent_mapping.mapping_id. These are not array indices.
#define TENSTORRENT_MAPPING_UNUSED		0
#define TENSTORRENT_MAPPING_RESOURCE0_UC	1
#define TENSTORRENT_MAPPING_RESOURCE0_WC	2
#define TENSTORRENT_MAPPING_RESOURCE1_UC	3
#define TENSTORRENT_MAPPING_RESOURCE1_WC	4
#define TENSTORRENT_MAPPING_RESOURCE2_UC	5
#define TENSTORRENT_MAPPING_RESOURCE2_WC	6
#define TENSTORRENT_MAPPING_RESOURCE_DMA	7

#define TENSTORRENT_MAX_DMA_BUFS	256

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
	__s32 numa_node;		// Since 1.28; -1 if not NUMA
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

struct tenstorrent_allocate_dma_buf_in {
	__u32 requested_size;
	__u8  buf_index;	// [0,TENSTORRENT_MAX_DMA_BUFS)
	__u8  reserved0[3];
	__u64 reserved1[2];
};

struct tenstorrent_allocate_dma_buf_out {
	__u64 physical_address;
	__u64 mapping_offset;
	__u32 size;
	__u32 reserved0;
	__u64 reserved1[2];
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
#define TENSTORRENT_PIN_PAGES_CONTIGUOUS 1

struct tenstorrent_pin_pages_in {
	__u32 output_size_bytes;
	__u32 flags;
	__u64 virtual_address;
	__u64 size;
};

struct tenstorrent_pin_pages_out {
	__u64 physical_address;
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

#define TENSTORRENT_MAX_HUGEPAGES_PER_CARD 4
struct tenstorrent_hugepage_setup {
	__u32 num_hugepages;
	__u64 virt_addrs[TENSTORRENT_MAX_HUGEPAGES_PER_CARD];
};

#endif
