// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Reset Tool for Tenstorrent Devices.
//
// This is the reference implementation of the reset sequence. It requires
// tt-kmd 2.11.1 or later, for POST_RESET_POLL.
//
// 1. Open each device. The fd stays valid across the reset.
// 2. On each fd, reset the PCIe link (RESET_PCIE_LINK), then issue the reset.
//    On Galaxy the driver only marks the devices for reset (USER_RESET), and
//    the reset itself is triggered for all of them at once through IPMI.
// 3. Issue POST_RESET_POLL on each fd until it stops failing with EAGAIN, which
//    it does while the reset has not reached the chip or firmware has not come
//    up. Its result is the outcome of the reset.
// 4. If it fails with ENODEV instead, hotplug removed the device. The device
//    comes back as a new one, possibly under a different number, once probe
//    has finished, which includes waiting for firmware. Find it by its PCI
//    address and issue POST_RESET_POLL there for the outcome.
//
// Steps 2 to 4 run for all devices in parallel.
//
// Disclaimer: Iteratively AI-written with manual adjustments.
//
// To Compile:
//  gcc -Wall -pthread -o reset reset.c
//
// To Run:
//  ./reset [--asic | --dmc] [--exclusive] [--timeout <s>] (--all | <device_id>...)
//  ./reset --galaxy [--exclusive] [--timeout <s>]
//
//  --asic       ASIC reset, triggered by the driver (the default)
//  --dmc        ASIC and DMC reset, triggered by the driver
//  --galaxy     Reset every device on a Galaxy system through IPMI (as root)
//  --all        Reset every device
//  --exclusive  Fail rather than reset a device that is open elsewhere
//  --timeout    Give up waiting this many seconds after the reset (default 180)
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/types.h>
#include <time.h>

#define FATAL(fmt, ...) do { fprintf(stderr, "%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); exit(1); } while (0)

// --- Driver Definitions ---
#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_RESET_DEVICE _IO(TENSTORRENT_IOCTL_MAGIC, 6)

// Flags for tenstorrent_reset_device_in.flags
#define TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK 1
#define TENSTORRENT_RESET_DEVICE_USER_RESET 3
#define TENSTORRENT_RESET_DEVICE_ASIC_RESET 4
#define TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET 5
#define TENSTORRENT_RESET_DEVICE_POST_RESET_POLL 7

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

#define BDF_STRING_SIZE 18
#define MAX_DEVICES 64
#define DEFAULT_TIMEOUT_S 180
#define POLL_INTERVAL_US 100000

// ipmitool raw 0x30 0x8B <ubb> <device> <op mode> <reset time>, where 0xF and
// 0xFF select every UBB and every device on it. Older BMC firmware does not
// support op mode 0x3, only 0x0.
static char *galaxy_reset_cmd[] = { "ipmitool", "raw", "0x30", "0x8B", "0xF", "0xFF", "0x3", "0xF", NULL };
static char *galaxy_reset_cmd_old_bmc[] = { "ipmitool", "raw", "0x30", "0x8B", "0xF", "0xFF", "0x0", "0xF", NULL };

enum dev_state {
    WAITING,    // Polling POST_RESET_POLL on fd
    RETURNING,  // Removed by hotplug; waiting for it to come back
    DONE,
    FAILED,
};

struct dev {
    int id;     // Device number at the start
    int cur_id; // Differs from id if hotplug renumbered the device
    char bdf[BDF_STRING_SIZE];
    int fd;
    enum dev_state state;
};

// Set before any thread starts, read-only afterwards.
static __u32 reset_flag = TENSTORRENT_RESET_DEVICE_ASIC_RESET;
static const char *reset_name = "ASIC_RESET";
static int timeout_s = DEFAULT_TIMEOUT_S;
static double start;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int open_dev(int id, int flags)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/dev/tenstorrent/%d", id);

    // O_APPEND marks the fd as power-aware, so the driver leaves the power
    // state alone when it is opened and closed.
    return open(path, O_RDWR | O_APPEND | flags);
}

// Returns 0, or the ioctl's errno.
static int get_bdf(int fd, char *bdf)
{
    struct tenstorrent_get_device_info info = {0};

    info.in.output_size_bytes = sizeof(info.out);
    if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0)
        return errno;

    snprintf(bdf, BDF_STRING_SIZE, "%04x:%02x:%02x.%x",
             info.out.pci_domain, (info.out.bus_dev_fn >> 8) & 0xFF,
             (info.out.bus_dev_fn >> 3) & 0x1F, info.out.bus_dev_fn & 0x7);
    return 0;
}

static int compare_ints(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

// Fills ids with the number of every device, in order, and returns how many
// there are.
static int find_all_dev_ids(int *ids)
{
    struct dirent *entry;
    int count = 0;
    DIR *dir;

    dir = opendir("/sys/class/tenstorrent");
    if (!dir)
        FATAL("Could not list /sys/class/tenstorrent: %s", strerror(errno));

    while ((entry = readdir(dir)) != NULL) {
        if (sscanf(entry->d_name, "tenstorrent!%d", &ids[count]) != 1)
            continue;
        if (++count == MAX_DEVICES)
            FATAL("Too many devices (max %d)", MAX_DEVICES);
    }

    closedir(dir);
    qsort(ids, count, sizeof(*ids), compare_ints);
    return count;
}

// Returns the number of the device at bdf, or -1 if there is none yet. This
// uses sysfs rather than opening devices, because an open blocks while
// another process holds the device with O_EXCL.
static int find_dev_id(const char *bdf)
{
    char path[PATH_MAX];
    struct dirent *entry;
    int id = -1;
    DIR *dir;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/tenstorrent", bdf);
    dir = opendir(path);
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (sscanf(entry->d_name, "tenstorrent!%d", &id) == 1)
            break;
        id = -1;
    }

    closedir(dir);
    return id;
}

// Opens the device that hotplug brought back at d->bdf. Returns 0, EAGAIN if
// it is not back yet, or the errno of a failed open.
static int reopen_dev(struct dev *d)
{
    char bdf[BDF_STRING_SIZE];
    int id = find_dev_id(d->bdf);
    int fd;

    if (id < 0)
        return EAGAIN;

    // Udev may not have created the node yet, the number may still belong to
    // the removed device, and another process may already hold the new one
    // with O_EXCL. Try again later in all of those cases rather than block.
    fd = open_dev(id, O_NONBLOCK);
    if (fd < 0)
        return (errno == ENOENT || errno == ENODEV || errno == ENXIO || errno == EAGAIN) ? EAGAIN : errno;

    // The number may have been reused by another device since the lookup.
    if (get_bdf(fd, bdf) != 0 || strcmp(bdf, d->bdf) != 0) {
        close(fd);
        return EAGAIN;
    }

    d->fd = fd;
    d->cur_id = id;
    return 0;
}

// Returns the ioctl's errno (0 if the ioctl itself succeeded) and stores
// out.result in *result.
static int reset_ioctl(int fd, __u32 flags, __u32 *result)
{
    struct tenstorrent_reset_device cmd = {0};

    cmd.in.flags = flags;
    cmd.in.output_size_bytes = sizeof(cmd.out);
    if (ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &cmd) < 0)
        return errno;

    *result = cmd.out.result;
    return 0;
}

// Issues one reset ioctl that must succeed. Returns 1 if it did.
static int reset_step(struct dev *d, __u32 flags, const char *name)
{
    __u32 result = 0;
    int err = reset_ioctl(d->fd, flags, &result);

    if (err)
        printf("device %d: %s failed: %s\n", d->id, name, strerror(err));
    else if (result != 0)
        printf("device %d: %s failed (result %u)\n", d->id, name, result);
    else
        return 1;

    d->state = FAILED;
    return 0;
}

static void *reset_dev(void *arg)
{
    struct dev *d = arg;

    if (reset_step(d, TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK, "RESET_PCIE_LINK"))
        reset_step(d, reset_flag, reset_name);

    return NULL;
}

// Polls one device once. Returns 1 if it has finished, successfully or not.
static int poll_dev(struct dev *d)
{
    double elapsed = now_s() - start;
    __u32 result = 0;
    int err;

    if (d->state == RETURNING) {
        err = reopen_dev(d);
        if (err == EAGAIN)
            return 0;

        if (err) {
            printf("device %d: could not open it after hotplug: %s\n", d->id, strerror(err));
            d->state = FAILED;
            return 1;
        }

        d->state = WAITING;
    }

    err = reset_ioctl(d->fd, TENSTORRENT_RESET_DEVICE_POST_RESET_POLL, &result);
    if (err == EAGAIN)
        return 0;

    if (err == ENODEV) {
        printf("device %d: removed by hotplug after %.1f s, waiting for it to return\n", d->id, elapsed);
        close(d->fd);
        d->fd = -1;
        d->state = RETURNING;
        return 0;
    }

    if (err == EINVAL) {
        printf("device %d: POST_RESET_POLL is not supported; tt-kmd 2.11.1 or later is required\n", d->id);
        d->state = FAILED;
    } else if (err) {
        printf("device %d: POST_RESET_POLL failed: %s\n", d->id, strerror(err));
        d->state = FAILED;
    } else if (result != 0) {
        printf("device %d: reset failed after %.1f s (result %u)\n", d->id, elapsed, result);
        d->state = FAILED;
    } else if (d->cur_id != d->id) {
        printf("device %d: reset complete after %.1f s, now /dev/tenstorrent/%d\n", d->id, elapsed, d->cur_id);
        d->state = DONE;
    } else {
        printf("device %d: reset complete after %.1f s\n", d->id, elapsed);
        d->state = DONE;
    }

    return 1;
}

// No initial delay is needed: the driver sets the reset marker before it
// triggers the reset, so POST_RESET_POLL keeps failing with EAGAIN until the
// reset has actually happened.
static void *wait_dev(void *arg)
{
    struct dev *d = arg;

    while (!poll_dev(d)) {
        if (now_s() - start >= timeout_s) {
            if (d->state == WAITING)
                printf("device %d: timed out waiting for the reset or for firmware\n", d->id);
            else
                printf("device %d: timed out waiting for it to return after hotplug\n", d->id);
            d->state = FAILED;
            break;
        }

        usleep(POLL_INTERVAL_US);
    }

    return NULL;
}

// Runs fn on every device that has not failed yet, each in its own thread,
// and waits for all of them.
static void run_parallel(struct dev *devs, int num_devs, void *(*fn)(void *))
{
    pthread_t threads[MAX_DEVICES];
    int started[MAX_DEVICES] = {0};
    int i, err;

    for (i = 0; i < num_devs; i++) {
        if (devs[i].state == FAILED)
            continue;

        err = pthread_create(&threads[i], NULL, fn, &devs[i]);
        if (err)
            FATAL("pthread_create failed: %s", strerror(err));
        started[i] = 1;
    }

    for (i = 0; i < num_devs; i++)
        if (started[i])
            pthread_join(threads[i], NULL);
}

// Returns 1 if the command exited with status 0. Failing to run it is fatal.
static int run_command(char **argv)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        FATAL("fork failed: %s", strerror(errno));

    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0)
        FATAL("waitpid failed: %s", strerror(errno));

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        FATAL("Could not run %s; the devices were marked for reset but not reset", argv[0]);

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--asic | --dmc] [--exclusive] [--timeout <s>] (--all | <device_id>...)\n"
            "       %s --galaxy [--exclusive] [--timeout <s>]\n", prog, prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    struct dev devs[MAX_DEVICES] = {0};
    int ids[MAX_DEVICES];
    int galaxy = 0;
    int all = 0;
    int exclusive = 0;
    int num_devs = 0;
    int failures = 0;
    int i, j;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--asic") == 0) {
            reset_flag = TENSTORRENT_RESET_DEVICE_ASIC_RESET;
            reset_name = "ASIC_RESET";
        } else if (strcmp(argv[i], "--dmc") == 0) {
            reset_flag = TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET;
            reset_name = "ASIC_DMC_RESET";
        } else if (strcmp(argv[i], "--galaxy") == 0) {
            galaxy = 1;
        } else if (strcmp(argv[i], "--all") == 0) {
            all = 1;
        } else if (strcmp(argv[i], "--exclusive") == 0) {
            exclusive = 1;
        } else if (strcmp(argv[i], "--timeout") == 0) {
            if (++i >= argc || (timeout_s = atoi(argv[i])) <= 0)
                usage(argv[0]);
        } else {
            char *end;
            long id = strtol(argv[i], &end, 10);

            if (*end != '\0' || end == argv[i] || id < 0)
                usage(argv[0]);
            if (num_devs == MAX_DEVICES)
                FATAL("Too many devices (max %d)", MAX_DEVICES);
            for (j = 0; j < num_devs; j++)
                if (ids[j] == id)
                    FATAL("Device %ld given more than once", id);

            ids[num_devs++] = (int)id;
        }
    }

    if (galaxy) {
        // Check now: finding out when ipmitool runs would leave every device
        // marked for a reset that never happens.
        if (geteuid() != 0)
            FATAL("--galaxy must be run as root, for ipmitool");

        reset_flag = TENSTORRENT_RESET_DEVICE_USER_RESET;
        reset_name = "USER_RESET";
        all = 1;
    }

    if (all && num_devs > 0)
        usage(argv[0]);

    if (all) {
        num_devs = find_all_dev_ids(ids);
        if (num_devs == 0)
            FATAL("No devices found");
    }

    if (num_devs == 0)
        usage(argv[0]);

    // Open every device before resetting any of them: a hotplug reset of one
    // device can renumber the others.
    for (i = 0; i < num_devs; i++) {
        struct dev *d = &devs[i];
        int err;

        d->id = ids[i];
        d->cur_id = ids[i];
        d->state = WAITING;

        d->fd = open_dev(d->id, exclusive ? O_EXCL | O_NONBLOCK : 0);
        if (d->fd < 0 && exclusive && errno == EAGAIN)
            FATAL("Device %d is in use", d->id);
        if (d->fd < 0)
            FATAL("Could not open device %d: %s", d->id, strerror(errno));

        err = get_bdf(d->fd, d->bdf);
        if (err)
            FATAL("Could not get the PCI address of device %d: %s", d->id, strerror(err));
    }

    printf("resetting %d device%s with RESET_PCIE_LINK, then %s\n", num_devs, num_devs == 1 ? "" : "s", reset_name);

    // Step 2.
    run_parallel(devs, num_devs, reset_dev);

    if (galaxy) {
        // IPMI resets every device, including any that could not be marked.
        // Those would come back behind the driver's back, so stop instead.
        for (i = 0; i < num_devs; i++)
            if (devs[i].state == FAILED)
                FATAL("Not resetting through IPMI, since device %d could not be prepared", devs[i].id);

        printf("resetting through IPMI\n");
        if (!run_command(galaxy_reset_cmd)) {
            printf("IPMI reset with op mode 0x3 failed; retrying with op mode 0x0. Update your BMC firmware.\n");
            if (!run_command(galaxy_reset_cmd_old_bmc))
                FATAL("IPMI reset failed; the devices were marked for reset but not reset");
        }
    }

    // Steps 3 and 4.
    start = now_s();
    run_parallel(devs, num_devs, wait_dev);

    for (i = 0; i < num_devs; i++) {
        if (devs[i].fd >= 0)
            close(devs[i].fd);
        if (devs[i].state != DONE)
            failures++;
    }

    printf("%d of %d devices reset\n", num_devs - failures, num_devs);
    return failures ? 1 : 0;
}
