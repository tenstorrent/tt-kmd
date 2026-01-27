// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Reset Tool for Tenstorrent Devices.
//
// Disclaimer: Iteratively AI-written with manual adjustments.
//
// To Compile:
//  gcc -o reset reset.c
//
// To Run:
//  ./reset [--dmc] <device_id>
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <time.h>

// --- Logging Macros ---
#define NOISY 0
#if NOISY
#define INFO(fmt, ...) do { fprintf(stdout, "%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } while (0)
#else
#define INFO(fmt, ...) do { } while (0)
#endif
#define FATAL(fmt, ...) do { fprintf(stderr, "%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); exit(1); } while (0)

// --- Driver Definitions ---
#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_RESET_DEVICE _IO(TENSTORRENT_IOCTL_MAGIC, 6)

// Flags for tenstorrent_reset_device_in.flags
#define TENSTORRENT_RESET_DEVICE_ASIC_RESET 4
#define TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET 5
#define TENSTORRENT_RESET_DEVICE_POST_RESET 6

#define BDF_STRING_SIZE 18

// --- Device Definitions ---
#define BLACKHOLE_PCI_DEVICE_ID 0xb140
#define WORMHOLE_PCI_DEVICE_ID  0x401e

enum TtDeviceType {
    DEVICE_TYPE_UNKNOWN,
    DEVICE_TYPE_WORMHOLE,
    DEVICE_TYPE_BLACKHOLE
};

struct tenstorrent_get_device_info {
    struct { __u32 output_size_bytes; } in;
    struct {
        __u32 output_size_bytes;
        __u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id, bus_dev_fn, max_dma_buf_size_log2, pci_domain;
    } out;
};

struct tenstorrent_reset_device {
    struct { __u32 output_size_bytes; __u32 flags; } in;
    struct { __u32 output_size_bytes; __u32 result; } out;
};

/**
 * @brief Retrieves the device type (Wormhole or Blackhole) for a given device ID.
 * @param dev_id The device index under /dev/tenstorrent.
 * @return The device type as an enum TtDeviceType. Returns DEVICE_TYPE_UNKNOWN on failure.
 */
enum TtDeviceType get_device_type(int dev_id) {
    char dev_path[PATH_MAX];
    snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);

    int fd = open(dev_path, O_RDWR | O_APPEND);
    if (fd < 0) {
        // This function should be non-fatal to be used safely.
        return DEVICE_TYPE_UNKNOWN;
    }

    struct tenstorrent_get_device_info info = {0};
    info.in.output_size_bytes = sizeof(info.out);

    if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
        close(fd);
        return DEVICE_TYPE_UNKNOWN;
    }
    close(fd);

    switch (info.out.device_id) {
        case WORMHOLE_PCI_DEVICE_ID:
            return DEVICE_TYPE_WORMHOLE;
        case BLACKHOLE_PCI_DEVICE_ID:
            return DEVICE_TYPE_BLACKHOLE;
        default:
            return DEVICE_TYPE_UNKNOWN;
    }
}

/**
 * @brief Retrieves the PCI BDF string for a given device ID. Returns 0 on success.
 * This function is non-fatal, designed to be used in loops.
 */
int get_bdf_for_dev_id(int dev_id, char *bdf_buf) {
    char dev_path[PATH_MAX];
    snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);

    int fd = open(dev_path, O_RDWR | O_APPEND);
    if (fd < 0) return -1;

    struct tenstorrent_get_device_info info = {0};
    info.in.output_size_bytes = sizeof(info.out);

    if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
        close(fd);
        return -1;
    }
    close(fd);

    snprintf(bdf_buf, BDF_STRING_SIZE, "%04x:%02x:%02x.%x",
             info.out.pci_domain, (info.out.bus_dev_fn >> 8) & 0xFF,
             (info.out.bus_dev_fn >> 3) & 0x1F, info.out.bus_dev_fn & 0x7);
    return 0;
}

/**
 * @brief Scans /dev/tenstorrent to find the device ID for a given BDF.
 * @return device ID on success, -1 on failure.
 */
int find_dev_id_by_bdf(const char *target_bdf) {
    DIR *d = opendir("/dev/tenstorrent/");
    if (!d) return -1;

    struct dirent *dir;
    int found_id = -1;
    while ((dir = readdir(d)) != NULL) {
        char *endptr;
        long dev_id = strtol(dir->d_name, &endptr, 10);
        if (*endptr != '\0') continue; // Skip non-numeric entries

        char current_bdf[BDF_STRING_SIZE];
        if (get_bdf_for_dev_id((int)dev_id, current_bdf) == 0) {
            if (strcmp(target_bdf, current_bdf) == 0) {
                found_id = (int)dev_id;
                break;
            }
        }
    }
    closedir(d);
    return found_id;
}


int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s [--dmc] <device_id>\n", argv[0]);
        exit(1);
    }

    int dmc_reset = 0;
    int dev_id_arg_index = 1;
    if (argc == 3) {
        if (strcmp(argv[1], "--dmc") == 0) {
            dmc_reset = 1;
            dev_id_arg_index = 2;
        } else {
            FATAL("Invalid option: %s. Usage: %s [--dmc] <device_id>", argv[1], argv[0]);
        }
    }

    int initial_dev_id = atoi(argv[dev_id_arg_index]);
    char pci_bdf[BDF_STRING_SIZE];
    char dev_path[PATH_MAX];
    snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", initial_dev_id);

    INFO("Starting reset on device /dev/tenstorrent/%d (%s)...", initial_dev_id, dmc_reset ? "ASIC+DMC" : "ASIC-only");

    // Step 1: Get BDF and trigger reset
    if (get_bdf_for_dev_id(initial_dev_id, pci_bdf) != 0) {
        FATAL("Could not get BDF for initial device ID %d", initial_dev_id);
    }
    INFO("/dev/tenstorrent/%d has BDF %s.", initial_dev_id, pci_bdf);

    enum TtDeviceType device_type = get_device_type(initial_dev_id);
    if (device_type == DEVICE_TYPE_UNKNOWN) FATAL("Unknown device type for /dev/tenstorrent/%d", initial_dev_id);
    INFO("/dev/tenstorrent/%d is of type %s.", initial_dev_id,
         device_type == DEVICE_TYPE_WORMHOLE ? "Wormhole" :
         device_type == DEVICE_TYPE_BLACKHOLE ? "Blackhole" : "Unknown");

    int fd = open(dev_path, O_RDWR | O_APPEND);
    if (fd < 0) FATAL("Could not open device %s: %s", dev_path, strerror(errno));

    struct tenstorrent_reset_device reset_cmd = {0};
    reset_cmd.in.flags = dmc_reset ? TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET : TENSTORRENT_RESET_DEVICE_ASIC_RESET;
    reset_cmd.in.output_size_bytes = sizeof(reset_cmd.out);
    if (ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &reset_cmd) < 0) {
        close(fd);
        FATAL("Reset trigger ioctl failed: %s", strerror(errno));
    }
    if (reset_cmd.out.result != 0) {
        close(fd);
        FATAL("Reset trigger %u returned error code %u", reset_cmd.in.flags, reset_cmd.out.result);
    }
    close(fd);

    // Step 2: Wait for reset to complete
    char sysfs_path[128];
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/pci/devices/%s", pci_bdf);

    INFO("Waiting for reset to complete for device %s...", pci_bdf);

    if (device_type == DEVICE_TYPE_WORMHOLE) {
        // Some amount of time here seems necessary for WH.
        // tt-smi uses 2 seconds, but that seems excessive.
        // On my system, 20ms isn't long enough but 40ms is.
        usleep(500000); // 500ms
    }

    int timeout_s = dmc_reset ? 10 : 5; // TODO: 10, 5??
    time_t start_time = time(NULL);
    int reset_complete = 0;
    int device_disappeared = 0;
    while (time(NULL) - start_time < timeout_s) {
        if (access(sysfs_path, F_OK) == 0) {
            if (device_disappeared) {
                INFO("Device reappeared on bus.");
                reset_complete = 1;
                break;
            }
            char config_path[256];
            snprintf(config_path, sizeof(config_path), "%s/config", sysfs_path);
            int config_fd = open(config_path, O_RDONLY);
            if (config_fd >= 0) {
                char cmd_reg;
                if (pread(config_fd, &cmd_reg, 1, 4) == 1 && ((cmd_reg >> 6) & 1) == 0) {
                    INFO("In-place reset completed (marker cleared).");
                    reset_complete = 1;
                }
                close(config_fd);
            }
            if (reset_complete) break;
        } else {
            if (!device_disappeared) {
                INFO("Device disappeared from bus, waiting for it to return...");
                device_disappeared = 1;
            }
        }
        usleep(100000); // 100ms
    }


    if (!reset_complete) FATAL("Timed out waiting for reset to complete.");

    // Step 3: Find the new device ID and perform Post-Reset action
    INFO("Searching for device with BDF %s...", pci_bdf);
    int post_reset_timeout_s = 10;
    start_time = time(NULL);
    int new_dev_id = -1;
    while (time(NULL) - start_time < post_reset_timeout_s) {
        if ((new_dev_id = find_dev_id_by_bdf(pci_bdf)) != -1) break;
        usleep(200000); // 200ms
    }

    if (new_dev_id == -1) FATAL("Could not find device with BDF %s after reset.", pci_bdf);
    INFO("Found device with BDF %s at new device ID %d.", pci_bdf, new_dev_id);

    char new_dev_path[PATH_MAX];
    snprintf(new_dev_path, sizeof(new_dev_path), "/dev/tenstorrent/%d", new_dev_id);
    fd = open(new_dev_path, O_RDWR | O_APPEND);
    if (fd < 0) FATAL("Could not open re-discovered device node %s: %s", new_dev_path, strerror(errno));

    memset(&reset_cmd, 0, sizeof(reset_cmd));
    reset_cmd.in.flags = TENSTORRENT_RESET_DEVICE_POST_RESET;
    reset_cmd.in.output_size_bytes = sizeof(reset_cmd.out);
    if (ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &reset_cmd) < 0) {
        close(fd);
        FATAL("POST_RESET ioctl failed: %s", strerror(errno));
    }
    if (reset_cmd.out.result != 0) {
        close(fd);
        FATAL("POST_RESET ioctl returned error code %u", reset_cmd.out.result);
    }
    close(fd);

    INFO("Reset process completed successfully.");

    return 0;
}
