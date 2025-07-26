/**
 * SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * @file tt_kmd_lib.h
 * @brief Userspace library for the Tenstorrent Kernel Mode Driver (tt-kmd).
 *
 * This library provides a stable interface for interacting with Tenstorrent
 * Wormhole (WH) and Blackhole (BH) devices. It serves as a low-level API for
 * userspace clients.
 */

#ifndef TT_KMD_LIB_H_
#define TT_KMD_LIB_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a Tenstorrent PCIe device.
 */
typedef struct tt_device_t tt_device_t;

/**
 * @brief Opaque handle to a TLB window.
 *
 * A TLB window is a fixed size aperture in the host address space that is
 * mappable to a device NOC (Network on Chip) location.
 */
typedef struct tt_tlb_t tt_tlb_t;

/**
 * @brief Opaque handle to a DMA mapping.
 *
 * A DMA mapping is host memory made device-accessible by the driver.
 */
typedef struct tt_dma_t tt_dma_t;

/**
 * @brief Configuration for a TLB window's mapping to the device NOC.
 *
 * These parameters control how memory operations on a TLB window are translated
 * into transactions on the device's NOC. See `tt_tlb_map()` for details.
 */
typedef struct tt_noc_addr_config_t {
    uint64_t addr;      /**< Local address aligned to the TLB window size */
    uint16_t x_end;     /**< X coord for unicast; rectangle end for multicast */
    uint16_t y_end;     /**< Y coord for unicast; rectangle end for multicast */
    uint16_t x_start;   /**< 0 for unicast; rectangle start for multicast */
    uint16_t y_start;   /**< 0 for unicast; rectangle start for multicast */
    uint8_t noc;        /**< 0 or 1 */
    uint8_t mcast;      /**< 1 to enable multicast */
    uint8_t ordering;   /**< Ordering semantics; see `enum tt_noc_ordering` */
    uint8_t static_vc;  /**< 1 to enable static virtual channel */
} tt_noc_addr_config_t;

/**
 * @brief Supported Tenstorrent device architectures.
 */
enum tt_device_arch {
    TT_DEVICE_ARCH_UNKNOWN = 0,
    TT_DEVICE_ARCH_WORMHOLE,
    TT_DEVICE_ARCH_BLACKHOLE
};

/**
 * @brief Queryable attributes of a Tenstorrent device.
 */
enum tt_device_attr {
    TT_DEVICE_ATTR_PCI_DOMAIN = 0,
    TT_DEVICE_ATTR_PCI_BUS = 1,
    TT_DEVICE_ATTR_PCI_DEVICE = 2,
    TT_DEVICE_ATTR_PCI_FUNCTION = 3,
    TT_DEVICE_ATTR_PCI_VENDOR_ID = 4,
    TT_DEVICE_ATTR_PCI_DEVICE_ID = 5,
    TT_DEVICE_ATTR_PCI_SUBSYSTEM_ID = 6,
    TT_DEVICE_ATTR_CHIP_ARCH = 7,
    TT_DEVICE_ATTR_NUM_1M_TLBS = 8,
    TT_DEVICE_ATTR_NUM_2M_TLBS = 9,
    TT_DEVICE_ATTR_NUM_16M_TLBS = 10,
    TT_DEVICE_ATTR_NUM_4G_TLBS = 11,
};

/**
 * @brief Queryable attributes of the Tenstorrent driver.
 */
enum tt_driver_attr {
    TT_DRIVER_API_VERSION = 0,
    TT_DRIVER_SEMVER_MAJOR = 1,
    TT_DRIVER_SEMVER_MINOR = 2,
    TT_DRIVER_SEMVER_PATCH = 3,
};

/**
 * @brief Caching modes for TLB windows mapped to the NOC.
 */
enum tt_tlb_cache_mode {
    TT_MMIO_CACHE_MODE_UC = 0, /**< Uncached; use for register accesses */
    TT_MMIO_CACHE_MODE_WC = 1, /**< Write Combined; use for memory accesses */
};

/**
 * @brief Ordering modes for NOC transactions.
 */
enum tt_noc_ordering {
    TT_NOC_ORDERING_RELAXED         = 0,    /**< Relaxed (no read-after-write hazard) */
    TT_NOC_ORDERING_STRICT          = 1,    /**< Full AXI ordering */
    TT_NOC_ORDERING_POSTED          = 2,    /**< May have read-after-write hazard */
    TT_NOC_ORDERING_POSTED_STRICT   = 3     /**< BH only, Unicast only */
};

/**
 * @brief Supported TLB window sizes.
 */
#define TT_TLB_SIZE_1M  (1ULL << 20)  /**< 1 MiB TLB window (WH only) */
#define TT_TLB_SIZE_2M  (1ULL << 21)  /**< 2 MiB TLB window (WH and BH) */
#define TT_TLB_SIZE_16M (1ULL << 24)  /**< 16 MiB TLB window (WH only) */
#define TT_TLB_SIZE_4G  (1ULL << 32)  /**< 4 GiB TLB window (BH only) */

/**
 * @brief Open a Tenstorrent device.
 *
 * @param chardev_path e.g. "/dev/tenstorrent/0"
 * @param out_dev Device handle
 */
int tt_device_open(const char* chardev_path, tt_device_t** out_dev);

/**
 * @brief Close a Tenstorrent device.
 *
 * @param dev Device handle
 */
int tt_device_close(tt_device_t* dev);

/**
 * @brief Query device attributes.
 *
 * @param dev Device handle
 * @param attr Attribute to query
 * @param out_value Attribute value
 * @return 0 on success, error code on failure
 */
int tt_device_get_attr(tt_device_t* dev, enum tt_device_attr attr, uint64_t* out_value);

/**
 * @brief Query driver attributes.
 *
 * @param dev Device handle; may be NULL for API version query
 * @param attr Attribute to query
 * @param out_value Attribute value
 * @return 0 on success, error code on failure
 */
int tt_driver_get_attr(tt_device_t* dev, enum tt_driver_attr attr, uint64_t* out_value);

/**
 * @brief Convenience function to read a 32-bit value from a device NOC address.
 *
 * Appropriate for reading device registers or memory.
 * Inefficient due to resource lifecycle management overhead.
 *
 * @param dev Device handle
 * @param x NOC0 x-coordinate
 * @param y NOC0 y-coordinate
 * @param addr NOC address
 * @param value Pointer to store the read value
 * @return int 0 on success, error code on failure
 */
int tt_noc_read32(tt_device_t* dev, uint8_t x, uint8_t y, uint64_t addr, uint32_t* value);

/**
 * @brief Convenience function to write a 32-bit value to a device NOC address.
 *
 * Appropriate for writing device registers or memory.
 * Inefficient due to resource lifecycle management overhead.
 *
 * @param dev Device handle
 * @param x NOC0 x-coordinate
 * @param y NOC0 y-coordinate
 * @param addr NOC address
 * @param value Value to write
 * @return int 0 on success, error code on failure
 */
int tt_noc_write32(tt_device_t* dev, uint8_t x, uint8_t y, uint64_t addr, uint32_t value);

/**
 * @brief Convenience function for reading from the device NOC.
 *
 * Appropriate for reading device memory (L1/DRAM).
 * Inefficient due to resource lifecycle management overhead.
 *
 * @param dev Device handle
 * @param x NOC0 x-coordinate
 * @param y NOC0 y-coordinate
 * @param addr NOC address
 * @param dst Pointer to store the read data
 * @param len Number of bytes to read
 * @return int 0 on success, error code on failure
 */
int tt_noc_read(tt_device_t* dev, uint8_t x, uint8_t y, uint64_t addr, void* dst, size_t len);

/**
 * @brief Convenience function for writing to the device NOC.
 *
 * Appropriate for writing device memory (L1/DRAM).
 * Inefficient due to resource lifecycle management overhead.
 *
 * @param dev Device handle
 * @param x NOC0 x-coordinate
 * @param y NOC0 y-coordinate
 * @param addr NOC address
 * @param src Pointer to the data to write
 * @param len Number of bytes to write
 * @return int 0 on success, error code on failure
 */
int tt_noc_write(tt_device_t* dev, uint8_t x, uint8_t y, uint64_t addr, const void* src, size_t len);

/**
 * @brief Flags to control how a host memory buffer is mapped for device access.
 *
 * These flags are used with `tt_dma_map()` to control how a NOC address is
 * generated for the host memory buffer.
 *
 * `TT_DMA_FLAG_NOC` and `TT_DMA_FLAG_NOC_TOP_DOWN` are mutually exclusive.
 */
enum tt_dma_map_flags {
    /**
     * @brief Do not request a mapping in the device's NOC-to-host aperture.
     */
    TT_DMA_FLAG_NONE = 0,

    /**
     * @brief Requests a mapping in the device's NOC-to-host aperture, allocated
     * from the bottom up.
     *
     * This flag instructs the driver to reserve a region within the PCIe tile's
     * NOC-to-host address space, mapping it to the pinned host memory. The
     * driver allocates the lowest available address range within the aperture.
     *
     * This technique is intended for applications that have expectations about
     * the NOC address (i.e. hard-coded in device-side software). Because the
     * aperture is a shared resource among all clients, the application MUST
     * validate the address returned by `tt_dma_get_noc_addr()` to ensure it
     * matches its expectation.
     */
    TT_DMA_FLAG_NOC = 1 << 0,

    /**
     * @brief Requests a mapping in the device's NOC-to-host aperture, allocated
     * from the top down.
     *
     * This flag acts similarly to `TT_DMA_FLAG_NOC`, but allocates from the
     * highest available address range within the aperture.
     *
     * It is intended for tools and runtime components, allowing them to avoid
     * collisions with bottom-up application mappings. This separation is useful
     * on Wormhole devices due to their more constrained aperture. While this
     * flag is supported on Blackhole for consistency, its use is less critical
     * given Blackhole's larger address space.
     */
    TT_DMA_FLAG_NOC_TOP_DOWN = 1 << 1,
};

/**
 * @brief Pins a host memory buffer and maps it for device access.
 *
 * This function makes a region of host memory accessible to a Tenstorrent
 * device. It can be used to prepare a buffer for access by the hardware DMA
 * engine or by device-side software via NOC transactions. If the system IOMMU
 * is not active, the buffer must be physically contiguous.
 *
 * `TT_DMA_FLAG_NOC` or `TT_DMA_FLAG_NOC_TOP_DOWN` flags impose constraints:
 * WH:
 * - Per-buffer size: 0x1000 <= len <= 0xFFFE_0000
 * - Cumulative mapping size limit: 0xFFFE_0000
 * - Maximum mappings: 16 simultaneous
 * BH:
 * - Per-buffer size: 0x1000 <= len <= 0xFFFF_F000
 * - Maximum mappings: 16 simultaneous
 *
 * @param dev Device handle
 * @param addr Virtual address of memory to map; must be page-aligned
 * @param len Number of bytes; must be a multiple of the page size
 * @param flags Bitmask of flags from `enum tt_dma_map_flags`
 * @param out_dma On success, a handle for the pinned mapping
 * @return 0 on success, error code on failure
 */
int tt_dma_map(tt_device_t* dev, void* addr, size_t len, int flags, tt_dma_t** out_dma);

/**
 * @brief Unpins a previously mapped memory region.
 *
 * Releases all resources associated with the mapping.
 *
 * @param dev Device handle
 * @param dma DMA handle from `tt_dma_map()`
 * @return 0 on success, error code on failure
 */
int tt_dma_unmap(tt_device_t* dev, tt_dma_t* dma);

/**
 * @brief Gets the DMA address for a mapped memory region.
 *
 * The address will be an I/O Virtual Address (IOVA) if an IOMMU is active on
 * the system, or a physical address (PA) otherwise. The address is always
 * available and is suitable for programming the hardware PCIe DMA engine.
 *
 * @param dma DMA handle from `tt_dma_map()`
 * @param out_dma_addr DMA address (IOVA or PA) for PCIe DMA operations
 * @return 0 on success, error code on failure
 */
int tt_dma_get_dma_addr(tt_dma_t* dma, uint64_t* out_dma_addr);

/**
 * @brief Gets the NOC-accessible address for a mapped memory region.
 *
 * Returns the address that device-side software must use to access the pinned
 * host buffer via the NOC.
 *
 * @param dma DMA handle from `tt_dma_map()`
 * @param out_noc_addr NOC address for device-side software access
 * @return 0 on success, error code on failure
 */
int tt_dma_get_noc_addr(tt_dma_t* dma, uint64_t* out_noc_addr);

/**
 * @brief Allocates a TLB window.
 *
 * Quantities and sizes of TLB windows vary by device architecture:
 *
 * Wormhole:
 *   156x  1 MiB windows
 *    10x  2 MiB windows
 *    20x 16 MiB windows
 * Blackhole:
 *   202x  2 MiB windows
 *     8x  4 GiB windows
 *
 * The driver may reserve one or more TLB windows for internal use.
 *
 * @param dev Device handle
 * @param size 1, 2, or 16 MiB (WH); 2 MiB or 4 GiB (BH)
 * @param cache Caching attribute; see `enum tt_tlb_cache_mode`
 * @param out_tlb On success, a handle to the allocated TLB window
 * @return 0 on success, error code on failure
 */
int tt_tlb_alloc(tt_device_t* dev, size_t size, enum tt_tlb_cache_mode cache, tt_tlb_t** out_tlb);

/**
 * @brief Releases a TLB window.
 *
 * @param dev Device handle
 * @param tlb TLB window to release
 * @return 0 on success, error code on failure
 */
int tt_tlb_free(tt_device_t* dev, tt_tlb_t* tlb);

/**
 * @brief Get a pointer to the MMIO region of a TLB window.
 *
 * Loads/stores using this pointer will access the device NOC according to the
 * TLB window's configuration. Dereferencing the pointer after calling
 * `tt_tlb_free()` on the TLB handle will invoke undefined behavior.
 *
 * @param tlb TLB window handle from `tt_tlb_alloc()`
 * @param out_mmio Pointer to the MMIO region of the TLB window
 * @return 0 on success, error code on failure
 */
int tt_tlb_get_mmio(tt_tlb_t* tlb, void** out_mmio);

/**
 * @brief Maps a TLB window to a NOC endpoint.
 *
 * @param dev Device handle
 * @param tlb TLB window handle from `tt_tlb_alloc()`
 * @param config NOC address configuration
 * @return 0 on success, error code on failure
 */
int tt_tlb_map(tt_device_t* dev, tt_tlb_t* tlb, tt_noc_addr_config_t* config);

/**
 * @brief Maps a TLB window to a NOC endpoint.
 *
 * This is a convenience function for a common operation. See `tt_tlb_map()`.
 *
 * @param dev Device handle
 * @param tlb TLB window handle from `tt_tlb_alloc()`
 * @param x NOC0 x-coordinate
 * @param y NOC0 y-coordinate
 * @param addr Address in the tile; must be a multiple of the TLB size
 * @return 0 on success, error code on failure
 */
int tt_tlb_map_unicast(tt_device_t* dev, tt_tlb_t* tlb, uint8_t x, uint8_t y, uint64_t addr);

#ifdef __cplusplus
}
#endif

#endif // TT_KMD_LIB_H_