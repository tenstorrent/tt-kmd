/**
 * SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "tt_kmd_lib.h"

#include <errno.h>
#include <linux/mman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#ifdef PROFILE_API_CALLS
#define OK(expr) do { \
    struct timespec start, end; \
    clock_gettime(CLOCK_MONOTONIC, &start); \
    int rc = (expr); \
    clock_gettime(CLOCK_MONOTONIC, &end); \
    long long duration_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec); \
    printf("[PROFILE] %-70s took %12lld ns\n", #expr, duration_ns); \
    if (rc != 0) { FATAL("API call failed: " #expr); } \
} while (0)
#else
#define OK(expr) do { if ((expr) != 0) { FATAL("API call failed: " #expr); } } while (0)
#endif
#define FATAL(fmt, ...) do { fprintf(stderr, "%s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); exit(1); } while (0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX_DEVICES 32
#define UNUSED(x) (void)(x)

#define WH_SIZE_X 10
#define WH_SIZE_Y 12
#define WH_PCIE_X 0
#define WH_PCIE_Y 3
#define WH_DDR_X 0
#define WH_DDR_Y 0
#define WH_ARC_X 0
#define WH_ARC_Y 10
#define WH_ARC_NOC_NODE_ID 0xFFFB2002CULL
#define WH_TENSIX_NOC_NODE_ID 0xFFB2002CULL

#define BH_SIZE_X 17
#define BH_SIZE_Y 12
#define BH_PCIE_X 19
#define BH_PCIE_Y 24
#define BH_DDR_X 17
#define BH_DDR_Y 12
#define BH_NOC_NODE_ID_LOGICAL 0xFFB20148ULL

static bool is_wormhole(tt_device_t* dev)
{
    uint64_t arch;
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_CHIP_ARCH, &arch));
    return arch == TT_DEVICE_ARCH_WORMHOLE;
}

static bool is_blackhole(tt_device_t* dev)
{
    uint64_t arch;
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_CHIP_ARCH, &arch));
    return arch == TT_DEVICE_ARCH_BLACKHOLE;
}

static void* allocate_dma_buffer(size_t len)
{
    void* addr = MAP_FAILED;

    if ((len % (1UL << 30)) == 0) {
        addr = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0);
    }

    if (addr == MAP_FAILED && (len % (1UL << 21)) == 0) {
        addr = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
    }

    if (addr == MAP_FAILED) {
        addr = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }

    /* TODO: use madvise instead? */

    return addr;
}

static void query_attributes(tt_device_t* dev)
{
    const char* arch;
    uint64_t vendor_id, device_id;
    uint64_t pci_domain, pci_bus, pci_device, pci_function;
    uint64_t num_1m_tlbs, num_2m_tlbs, num_16m_tlbs, num_4g_tlbs;
    uint64_t semver_major, semver_minor, semver_patch;
    uint64_t api_version;

    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_PCI_VENDOR_ID, &vendor_id));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_PCI_DEVICE_ID, &device_id));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_PCI_DOMAIN, &pci_domain));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_PCI_BUS, &pci_bus));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_PCI_DEVICE, &pci_device));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_PCI_FUNCTION, &pci_function));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_NUM_1M_TLBS, &num_1m_tlbs));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_NUM_2M_TLBS, &num_2m_tlbs));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_NUM_16M_TLBS, &num_16m_tlbs));
    OK(tt_device_get_attr(dev, TT_DEVICE_ATTR_NUM_4G_TLBS, &num_4g_tlbs));

    OK(tt_driver_get_attr(dev, TT_DRIVER_SEMVER_MAJOR, &semver_major));
    OK(tt_driver_get_attr(dev, TT_DRIVER_SEMVER_MINOR, &semver_minor));
    OK(tt_driver_get_attr(dev, TT_DRIVER_SEMVER_PATCH, &semver_patch));
    OK(tt_driver_get_attr(NULL, TT_DRIVER_API_VERSION, &api_version));  /* OK to call with NULL device. */

    arch = is_wormhole(dev) ? "Wormhole" :
           is_blackhole(dev) ? "Blackhole" : "Unknown";

    printf("\t Driver: %lu.%lu.%lu (API %lu)\n", semver_major, semver_minor, semver_patch, api_version);
    printf("\t %04lx:%02lx:%02lx.%lx %04lx:%04lx", pci_domain, pci_bus, pci_device, pci_function, vendor_id, device_id);
    printf(" (%s)\n", arch);

    if (num_1m_tlbs > 0) {
        printf("\t %lu 1M TLBs\n", num_1m_tlbs);
    }
    if (num_2m_tlbs > 0) {
        printf("\t %lu 2M TLBs\n", num_2m_tlbs);
    }
    if (num_16m_tlbs > 0) {
        printf("\t %lu 16M TLBs\n", num_16m_tlbs);
    }
    if (num_4g_tlbs > 0) {
        printf("\t %lu 4G TLBs\n", num_4g_tlbs);
    }
}

static uint32_t seed;
static void my_srand(uint32_t new_seed)
{
    seed = new_seed;
}

static inline uint32_t my_rand(void)
{
    seed = seed * 1103515245 + 12345;
    return (uint32_t)(seed / 65536) % 32768;
}

static void noc_dma_test(tt_device_t* dev, size_t len)
{
    /* Allocate a DMA buffer. */
    void* buffer = allocate_dma_buffer(len);
    if (buffer == MAP_FAILED) {
        FATAL("Failed to allocate DMA buffer: %s", strerror(errno));
    }
    memset(buffer, 0, len);

    /* Map the DMA buffer. */
    tt_dma_t* dma_handle;
    OK(tt_dma_map(dev, buffer, len, TT_DMA_FLAG_NOC, &dma_handle));

    /* Allocate a TLB window */
    tt_tlb_t *tlb;
    size_t tlb_size = TT_TLB_SIZE_2M;
    void* mmio;
    OK(tt_tlb_alloc(dev, tlb_size, TT_MMIO_CACHE_MODE_WC, &tlb));
    OK(tt_tlb_get_mmio(tlb, &mmio));

    /* Prepare the NOC address (x, y, address) that targets the buffer. */
    uint16_t pcie_x = is_wormhole(dev) ? WH_PCIE_X : is_blackhole(dev) ? BH_PCIE_X : -1;
    uint16_t pcie_y = is_wormhole(dev) ? WH_PCIE_Y : is_blackhole(dev) ? BH_PCIE_Y : -1;
    uint64_t noc_addr;
    OK(tt_dma_get_noc_addr(dma_handle, &noc_addr));

    /* Write a pattern through the window. */
    uint64_t bytes_remaining = len;
    uint64_t seed = 17;
    my_srand(seed);
    while (bytes_remaining > 0) {
        uint64_t aligned_addr = noc_addr & ~(tlb_size - 1);
        uint64_t offset = noc_addr & (tlb_size - 1);
        size_t chunk_size = MIN(bytes_remaining, tlb_size - offset);
        uint32_t* dst_ptr = (uint32_t*)((uint8_t*)mmio + offset);

        /* Map the TLB window for this chunk. */
        if (tt_tlb_map_unicast(dev, tlb, pcie_x, pcie_y, aligned_addr) != 0) {
            FATAL("Failed to configure TLB for write");
        }

        for (size_t i = 0; i < chunk_size; i += 4) {
            uint32_t value = my_rand();
            dst_ptr[i / 4] = value;
        }

        bytes_remaining -= chunk_size;
        noc_addr += chunk_size;
    }

    /* Release the TLB window. */
    OK(tt_tlb_free(dev, tlb));

    /* Unmap the DMA buffer. */
    OK(tt_dma_unmap(dev, dma_handle));

    /* Verify the written data by re-generating the pattern and comparing. */
    my_srand(seed);
    for (size_t i = 0; i < len / sizeof(uint32_t); i++) {
        uint32_t expected_value = my_rand();
        uint32_t actual_value = ((uint32_t*)buffer)[i];

        if (expected_value != actual_value) {
            FATAL("Data mismatch at index %zu: expected %u, got %u", i, expected_value, actual_value);
        }
    }

    /* Deallocate the buffer. */
    munmap(buffer, len);

    printf("NOC DMA (size=0x%lx) test PASSED\n", len);
}

static bool is_tensix_wh(uint32_t x, uint32_t y)
{
    return (((y != 6) && (y >= 1) && (y <= 11)) && // valid Y
            ((x != 5) && (x >= 1) && (x <= 9)));   // valid X
}

static void node_id_test_wh(tt_device_t* dev)
{
    uint32_t node_id;
    uint32_t node_id_x;
    uint32_t node_id_y;

    if (!is_wormhole(dev)) {
        return;
    }

    OK(tt_noc_read32(dev, WH_ARC_X, WH_ARC_Y, WH_ARC_NOC_NODE_ID, &node_id));

    node_id_x = (node_id >> 0x0) & 0x3f;
    node_id_y = (node_id >> 0x6) & 0x3f;

    if (node_id_x != WH_ARC_X || node_id_y != WH_ARC_Y) {
        FATAL("ARC ID mismatch, expected (%u, %u), got (%u, %u)", WH_ARC_X, WH_ARC_Y, node_id_x, node_id_y);
    }

    for (uint32_t x = 0; x < WH_SIZE_X; ++x) {
        for (uint32_t y = 0; y < WH_SIZE_Y; ++y) {
            if (!is_tensix_wh(x, y)) {
                continue;
            }

            OK(tt_noc_read32(dev, x, y, WH_TENSIX_NOC_NODE_ID, &node_id));

            node_id_x = (node_id >> 0x0) & 0x3f;
            node_id_y = (node_id >> 0x6) & 0x3f;

            if (node_id_x != x || node_id_y != y) {
                FATAL("Tensix ID mismatch, expected (%u, %u), got (%u, %u)", x, y, node_id_x, node_id_y);
            }
        }
    }

    printf("NOC node id test PASSED\n");
}

static bool is_tensix_bh(uint32_t x, uint32_t y)
{
    return (y >= 2 && y <= 11) &&   // Valid y range
        ((x >= 1 && x <= 7) ||      // Left block
        (x >= 10 && x <= 16));      // Right block
}

static void node_id_test_bh(tt_device_t* dev)
{
    uint32_t node_id;
    uint32_t node_id_x;
    uint32_t node_id_y;

    if (!is_blackhole(dev)) {
        return;
    }

    for (uint32_t x = 0; x < BH_SIZE_X; ++x) {
        for (uint32_t y = 0; y < BH_SIZE_Y; ++y) {
            if (!is_tensix_bh(x, y)) {
                continue;
            }

            OK(tt_noc_read32(dev, x, y, BH_NOC_NODE_ID_LOGICAL, &node_id));

            node_id_x = (node_id >> 0x0) & 0x3f;
            node_id_y = (node_id >> 0x6) & 0x3f;

            if (node_id_x != x || node_id_y != y) {
                FATAL("Tensix ID mismatch, expected (%u, %u), got (%u, %u)", x, y, node_id_x, node_id_y);
            }
        }
    }

    printf("NOC node id test PASSED\n");
}

void block_io_test(tt_device_t* dev)
{
    uint16_t ddr_x = is_wormhole(dev) ? WH_DDR_X : is_blackhole(dev) ? BH_DDR_X : -1;
    uint16_t ddr_y = is_wormhole(dev) ? WH_DDR_Y : is_blackhole(dev) ? BH_DDR_Y : -1;

    /* Allocate buffer. */
    void* data;
    size_t len = 0x380000; /* 3.5 MiB */
    data = malloc(len);
    if (!data) {
        FATAL("Failed to allocate memory for data: %s", strerror(errno));
    }

    /* Fill buffer with pseudorandom numbers. */
    my_srand(42);
    for (size_t i = 0; i < len / sizeof(uint32_t); i++) {
        ((uint32_t*)data)[i] = my_rand();
    }

    /* Write the buffer and read it back in a few different places */
    uint64_t addresses[] = { 0x000000, 0xF00008, 0x50000C };
    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        uint64_t addr = addresses[i];

        /* Write data to the NOC. */
        OK(tt_noc_write(dev, ddr_x, ddr_y, addr, data, len));

        /* Read it back into a new buffer. */
        void* read_data = malloc(len);
        if (!read_data) {
            FATAL("Failed to allocate memory for read data: %s", strerror(errno));
        }
        OK(tt_noc_read(dev, ddr_x, ddr_y, addr, read_data, len));

        /* Verify that the data matches. */
        if (memcmp(data, read_data, len) != 0) {
            FATAL("Data mismatch at address 0x%lx", addr);
        }

        free(read_data);
    }

    printf("Block I/O test PASSED\n");
}

int main(int argc, char** argv)
{
    uint64_t api_version;
    OK(tt_driver_get_attr(NULL, TT_DRIVER_API_VERSION, &api_version));
    printf("Tenstorrent Driver API Version: %lu\n", api_version);

    for (int i = 0; i < MAX_DEVICES; ++i) {
        char chardev_path[32];
        snprintf(chardev_path, sizeof(chardev_path), "/dev/tenstorrent/%d", i);

        tt_device_t* dev;
        if (tt_device_open(chardev_path, &dev) != 0)
            continue;

        printf("Running tests on %s\n", chardev_path);

        query_attributes(dev);
        node_id_test_wh(dev);
        node_id_test_bh(dev);
        block_io_test(dev);
        noc_dma_test(dev, 0x1000);
        noc_dma_test(dev, 0x4000);
        noc_dma_test(dev, 0x204000);
        noc_dma_test(dev, 1ULL << 21);
        noc_dma_test(dev, 1ULL << 30);

        OK(tt_device_close(dev));
        printf("\n");
    }
    return 0;

    UNUSED(argc);
    UNUSED(argv);
}
