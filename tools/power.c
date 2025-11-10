// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// This tool allows fine-grained control over Tenstorrent device power features
// for performance tuning and power management. It provides options to set
// specific power-related flags and numeric settings via a kernel driver ioctl.
//
// To Compile:
//  gcc -o power power.c
//
// To Run:
//  ./power <device_path> [OPTIONS]
//
// Examples:
//  ./power /dev/tenstorrent/0 -f 1,0           # bit 0=1, bit 1=0 (controlling 2 bits)
//  ./power /dev/tenstorrent/0 -f 1,1           # bit 0=1, bit 1=1 (controlling 2 bits)
//  ./power /dev/tenstorrent/0 -s 100,200,300   # Set power_settings[0]=100, [1]=200, [2]=300
//  ./power /dev/tenstorrent/0 -f 1,0 -s 50,75  # Combine flags and settings
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <getopt.h>

#define INFO(fmt, ...) do { fprintf(stdout, "%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } while (0)
#define FATAL(fmt, ...) do { fprintf(stderr, "%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); exit(1); } while (0)

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_SET_POWER_STATE _IO(TENSTORRENT_IOCTL_MAGIC, 15)

struct tenstorrent_power_state {
	__u32 argsz;
	__u32 flags;
	__u8 reserved0;
	__u8 validity;
#define TT_POWER_VALIDITY_FLAGS(n)      (((n) & 0xF) << 0)
#define TT_POWER_VALIDITY_SETTINGS(n)   (((n) & 0xF) << 4)
#define TT_POWER_VALIDITY(flags, settings) (TT_POWER_VALIDITY_FLAGS(flags) | TT_POWER_VALIDITY_SETTINGS(settings))
	__u16 power_flags;
#define TT_POWER_FLAG_MAX_AI_CLK        (1U << 0) /* 1=Max AI Clock, 0=Min AI Clock */
#define TT_POWER_FLAG_MRISC_PHY_WAKEUP  (1U << 1) /* 1=PHY Wakeup,   0=PHY Powerdown */
	__u16 power_settings[14];
};


static void set_power_state(int fd, __u16 power_flags, __u8 num_flags,
                           __u16 *power_settings, __u8 num_settings) {
    struct tenstorrent_power_state power_state = {0};

    power_state.argsz = sizeof(power_state);
    power_state.validity = TT_POWER_VALIDITY(num_flags, num_settings);
    power_state.power_flags = power_flags;
    
    // Copy power settings if provided
    if (num_settings > 0 && power_settings) {
        memcpy(power_state.power_settings, power_settings, 
               num_settings * sizeof(__u16));
    }

    INFO("Setting power state:");
    INFO("  flags: 0x%04X (validity: %d bits)", 
         power_state.power_flags, num_flags);
    if (num_settings > 0) {
        INFO("  settings: %d values", num_settings);
        for (int i = 0; i < num_settings; i++) {
            INFO("    [%d] = %u", i, power_state.power_settings[i]);
        }
    }

    if (ioctl(fd, TENSTORRENT_IOCTL_SET_POWER_STATE, &power_state) < 0) {
        FATAL("Failed to set power state: %s", strerror(errno));
    }

    INFO("Successfully set power state.");
}

// Parse comma-delimited list of flag bit values
// Example: "1,0" sets bit 0=1, bit 1=0 (controlling 2 bits)
// Example: "1,1,0" sets bit 0=1, bit 1=1, bit 2=0 (controlling 3 bits)
// Returns the bitmask, and sets num_flags to the count of values provided
static __u16 parse_flags(const char *flag_str, __u8 *num_flags) {
    __u16 flags = 0;
    *num_flags = 0;

    char *str = strdup(flag_str);
    char *saveptr;
    char *token = strtok_r(str, ",", &saveptr);

    while (token != NULL) {
        char *endptr;
        long value = strtol(token, &endptr, 10);

        // Check for conversion errors or invalid values
        if (*endptr != '\0' || value < 0 || value > 1) {
            FATAL("Invalid flag value: %s (must be 0 or 1)", token);
        }
        
        if (value == 1) {
            flags |= (1U << (*num_flags));
        }
        
        (*num_flags)++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    
    free(str);
    
    // Sanity check: validity field only has 4 bits, so max num_flags is 15
    if (*num_flags > 15) {
        FATAL("Too many flag bits: %d (validity field max is 15)", *num_flags);
    }
    
    return flags;
}

// Parse comma-delimited list of setting values
// Example: "100,200,300" sets settings[0]=100, [1]=200, [2]=300
static void parse_settings(const char *settings_str, __u16 *settings, __u8 *num_settings) {
    *num_settings = 0;

    char *str = strdup(settings_str);
    char *saveptr;
    char *token = strtok_r(str, ",", &saveptr);

    while (token != NULL) {
        if (*num_settings >= 14) {
            FATAL("Too many settings (max 14)");
        }

        char *endptr;
        long value = strtol(token, &endptr, 10);

        // Check for conversion errors or negative values
        if (*endptr != '\0' || value < 0 || value > 65535) {
            FATAL("Invalid setting value: %s (must be 0-65535)", token);
        }

        settings[*num_settings] = (__u16)value;
        (*num_settings)++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(str);
}

void print_usage(const char *exec_name) {
    fprintf(stderr, "Usage: %s <device_path> [OPTIONS]\n\n", exec_name);
    
    fprintf(stderr, "OPTIONS:\n");
    fprintf(stderr, "  -f, --flags <bit_values>    Comma-delimited list of flag bit values (0 or 1, max 15).\n");
    fprintf(stderr, "                              Position in list = bit index, value = bit state.\n");
    fprintf(stderr, "                              IMPORTANT: The driver aggregates settings from all clients.\n");
    fprintf(stderr, "                              For flags, unspecified bits are treated as ON by the driver\n");
    fprintf(stderr, "                              to ensure backward compatibility. For example,\n");
    fprintf(stderr, "                              '-f 1' sets bit 0 to 1, and the driver will consider\n");
    fprintf(stderr, "                              bits 1-14 to be ON for this client's request.\n");
    fprintf(stderr, "                              To turn a flag OFF, you must explicitly provide a 0.\n");
    fprintf(stderr, "                              Example: -f 1,0  (bit 0=1, bit 1=0)\n");
    fprintf(stderr, "  -s, --settings <val_list>   Comma-delimited list of setting values (0-65535, max 14).\n");
    fprintf(stderr, "                              Example: -s 100,200  (sets settings[0]=100, [1]=200)\n");
    fprintf(stderr, "  -h, --help                  Print this help message\n\n");
    
    fprintf(stderr, "NOTE: The final power state is an aggregation of settings from all applications\n");
    fprintf(stderr, "      currently using the device. If a setting does not appear to take effect,\n");
    fprintf(stderr, "      check for other running Tenstorrent processes.\n\n");
    fprintf(stderr, "      Additionally, whether a setting is supported depends on the device firmware\n");
    fprintf(stderr, "      version. Older firmware may not implement all features, but for forward\n");
    fprintf(stderr, "      compatibility, the driver will not return an error for unknown settings.\n\n");

    fprintf(stderr, "FLAG BITS:\n");
    fprintf(stderr, "  Bit 0 = TT_POWER_FLAG_MAX_AI_CLK       (AI Clock: 1=Max,    0=Min)\n");
    fprintf(stderr, "  Bit 1 = TT_POWER_FLAG_MRISC_PHY_WAKEUP (GDDR PHY: 1=Wakeup, 0=Powerdown)\n");
    fprintf(stderr, "  Bits 2-14: Reserved for future use (TBD)\n\n");

    fprintf(stderr, "SETTING VALUES:\n");
    fprintf(stderr, "  Values 0-13: Reserved for future use (TBD)\n\n");
    
    fprintf(stderr, "EXAMPLES:\n");
    fprintf(stderr, "  %s /dev/tenstorrent/0 -f 1,0      # Set bit 0=1, bit 1=0.\n", exec_name);
    fprintf(stderr, "  %s /dev/tenstorrent/0 -s 100,200  # Set settings[0]=100, [1]=200\n", exec_name);
    fprintf(stderr, "  %s /dev/tenstorrent/0 -f 1,0 -s 50# Combine flags and settings\n", exec_name);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        exit(1);
    }

    __u16 power_flags = 0;
    __u8 num_flags = 0;
    __u16 power_settings[14] = {0};
    __u8 num_settings = 0;
    int has_flags = 0;
    int has_settings = 0;

    // The first argument is the device ID, so we start parsing options at argv[2].
    // To do this with getopt, we'll handle argv[1] manually first, then reset
    // optind to 2 so getopt starts parsing at the right place.
    if (argc < 2 || (argv[1][0] == '-' && argv[1][1] != '\0')) {
        print_usage(argv[0]);
        exit(1);
    }

    char *dev_path = argv[1];

    // Basic validation: check if it's a path-like string
    if (strchr(dev_path, '/') == NULL) {
        FATAL("Invalid argument: %s. Please provide a full device path (e.g., /dev/tenstorrent/0)", dev_path);
    }

    static struct option long_options[] = {
        {"flags",    required_argument, 0, 'f'},
        {"settings", required_argument, 0, 's'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 2; // Start parsing from the second argument
    while ((opt = getopt_long(argc, argv, "f:s:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                power_flags = parse_flags(optarg, &num_flags);
                has_flags = 1;
                break;
            case 's':
                parse_settings(optarg, power_settings, &num_settings);
                has_settings = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case '?':
                // getopt_long already printed an error message.
                print_usage(argv[0]);
                exit(1);
            default:
                abort();
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Unknown option or command: %s\n", argv[optind]);
        print_usage(argv[0]);
        exit(1);
    }

    if (!has_flags && !has_settings) {
        fprintf(stderr, "Error: Must specify at least one of -f/--flags or -s/--settings\n\n");
        print_usage(argv[0]);
        exit(1);
    }

    // Open device
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        FATAL("Could not open device %s: %s", dev_path, strerror(errno));
    }

    // Set power state
    set_power_state(fd, power_flags, num_flags, 
                    has_settings ? power_settings : NULL, num_settings);

    close(fd);
    return 0;
}
