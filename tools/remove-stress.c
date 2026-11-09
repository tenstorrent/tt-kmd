// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Remove-handler stress tool for Tenstorrent devices.
//
// One thread unbinds and rebinds the driver through sysfs in a loop while
// worker threads keep every other entry point busy: open, close, ioctl, mmap,
// page pinning, DMA buffers, TLB dma-buf export, blocking waits, sysfs/hwmon,
// procfs, debugfs and (optionally) device reset. Workers expect every call to
// fail at any time; the driver is judged by the kernel log, which the tool
// follows and scans for warnings, splats and IOMMU faults. Run it on a kernel
// with lockdep and a memory debugger enabled.
//
// Disclaimer: AI-written.
//
// To Compile:
//  gcc -O2 -Wall -pthread -o remove-stress remove-stress.c
//
// To Run (as root):
//  ./remove-stress [-t seconds] [-r] [-w workers] <bdf>
//
//  -t  run time in seconds (default 30)
//  -r  also race RESET_DEVICE against remove (default off; see below)
//  -w  comma-separated list of workers to run (default: all)
//
// With -r the device can be unbound while it is mid-reset, which leaves its
// config space wiped. The tool then removes and rescans the device to get it
// back, but if that fails the device stays broken until the host is rebooted.
//
// Exit status: 0 if the kernel log stayed clean and no worker hung, else 1.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../ioctl.h"

#define BLACKHOLE_PCI_DEVICE_ID 0xb140

// Blackhole ARC: a scratch register that is always safe to read, and the
// firmware's no-op test message.
#define BH_ARC_X 8
#define BH_ARC_Y 0
#define BH_ARC_SCRATCH 0x80030400ULL
#define BH_ARC_MSG_TEST 0x90

#define TLB_2M (1UL << 21)

#define HANG_SECONDS 10

static const char *bdf;
static bool is_blackhole;
static atomic_bool stop;
static atomic_ulong cycles; // completed unbind/bind cycles

// --- Helpers ---

static void msleep(unsigned ms)
{
    usleep(ms * 1000);
}

// Per-thread xorshift; quality is irrelevant, cheapness is not.
static __thread uint32_t rng_state;

static uint32_t rnd(uint32_t n)
{
    if (!rng_state)
        rng_state = (uint32_t)(uintptr_t)&rng_state ^ (uint32_t)time(NULL);
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state % n;
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int write_str(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;

    if (fd < 0)
        return -errno;

    n = write(fd, s, strlen(s));
    n = (n < 0) ? -errno : 0;
    close(fd);
    return n;
}

// The device's current ordinal, or -1 while it is unbound.
static int dev_ordinal(void)
{
    char path[256];
    struct dirent *e;
    int ordinal = -1;
    DIR *d;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/tenstorrent", bdf);
    d = opendir(path);
    if (!d)
        return -1;

    while ((e = readdir(d)) != NULL)
        if (sscanf(e->d_name, "tenstorrent!%d", &ordinal) == 1)
            break;

    closedir(d);
    return ordinal;
}

static int dev_open(int extra_flags)
{
    char path[64];
    int ordinal = dev_ordinal();

    if (ordinal < 0) {
        errno = ENOENT;
        return -1;
    }

    snprintf(path, sizeof(path), "/dev/tenstorrent/%d", ordinal);
    return open(path, O_RDWR | O_CLOEXEC | extra_flags);
}

static bool dev_alive(int fd)
{
    struct tenstorrent_get_device_info info = { 0 };

    info.in.output_size_bytes = sizeof(info.out);
    return ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) == 0;
}

// --- Workers ---

struct worker {
    const char *name;
    void (*fn)(struct worker *);
    bool enabled;
    pthread_t thread;
    atomic_ulong beat;     // bumped on every iteration; watched for hangs
    atomic_ulong ok;       // iterations that did the whole operation
    atomic_ulong dead_fds; // opens that returned an fd for an already-dead device
    unsigned long last_beat;
    double last_beat_time;
    unsigned long hangs;
};

// Open the device for a worker iteration. Counts fds that are born dead: open
// succeeded but the very first ioctl says the device is gone. A few are
// expected (remove can land between the two calls); a driver that admits
// opens on a detached device produces many.
static int worker_open(struct worker *w, int extra_flags)
{
    int fd = dev_open(extra_flags);

    if (fd < 0) {
        msleep(1);
        return -1;
    }

    if (!dev_alive(fd))
        atomic_fetch_add(&w->dead_fds, 1);

    return fd;
}

static void worker_open_close(struct worker *w)
{
    static const int flags[] = { 0, O_APPEND, O_EXCL | O_NONBLOCK, O_APPEND | O_NONBLOCK };

    while (!stop) {
        int fd = worker_open(w, flags[rnd(4)]);

        atomic_fetch_add(&w->beat, 1);
        if (fd < 0)
            continue;

        close(fd);
        atomic_fetch_add(&w->ok, 1);
    }
}

// A blocking O_EXCL open waits for every other fd to close. Remove must not
// leave it waiting on a device that no longer exists.
static void worker_open_excl(struct worker *w)
{
    while (!stop) {
        int fd = worker_open(w, O_EXCL);

        atomic_fetch_add(&w->beat, 1);
        if (fd < 0)
            continue;

        msleep(rnd(5));
        close(fd);
        atomic_fetch_add(&w->ok, 1);
    }
}

static void worker_ioctl(struct worker *w)
{
    while (!stop) {
        int fd = worker_open(w, O_APPEND);
        int i;

        atomic_fetch_add(&w->beat, 1);
        if (fd < 0)
            continue;

        for (i = 0; i < 200 && !stop; i++) {
            struct tenstorrent_power_state power = { 0 };
            struct tenstorrent_noc_io io = { 0 };
            struct tenstorrent_smc_msg msg = { 0 };

            atomic_fetch_add(&w->beat, 1);

            if (!dev_alive(fd))
                break;

            power.argsz = sizeof(power);
            power.validity = TT_POWER_VALIDITY(1, 0);
            power.power_flags = rnd(2) ? TT_POWER_FLAG_MAX_AI_CLK : 0;
            ioctl(fd, TENSTORRENT_IOCTL_SET_POWER_STATE, &power);

            if (is_blackhole) {
                io.argsz = sizeof(io);
                io.x = BH_ARC_X;
                io.y = BH_ARC_Y;
                io.width = 4;
                io.addr = BH_ARC_SCRATCH;
                ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &io);

                msg.argsz = sizeof(msg);
                msg.flags = TENSTORRENT_SMC_MSG_POST;
                msg.message[0] = BH_ARC_MSG_TEST;
                if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg) == 0 && rnd(4)) {
                    int tries;

                    // Usually collect the response; otherwise leave the
                    // message outstanding for close() to abandon.
                    msg.flags = TENSTORRENT_SMC_MSG_POLL;
                    for (tries = 0; tries < 50; tries++) {
                        if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg) == 0 || errno != EAGAIN)
                            break;
                        usleep(200);
                    }
                }
            }

            atomic_fetch_add(&w->ok, 1);
        }

        close(fd);
    }
}

// SIGBUS is the expected result of touching a mapping the driver has zapped.
static __thread sigjmp_buf bus_jmp;
static __thread volatile bool bus_armed;

static void on_sigbus(int sig)
{
    (void)sig;

    if (!bus_armed)
        abort();

    bus_armed = false;
    siglongjmp(bus_jmp, 1);
}

static void worker_mmap(struct worker *w)
{
    while (!stop) {
        struct tenstorrent_allocate_tlb tlb = { 0 };
        struct tenstorrent_configure_tlb config = { 0 };
        volatile uint32_t *regs;
        int fd = worker_open(w, O_APPEND);
        int i;

        atomic_fetch_add(&w->beat, 1);
        if (fd < 0)
            continue;

        tlb.in.size = TLB_2M;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            goto out_close;

        // Only Blackhole has a target here that is known safe to read; on
        // anything else the window is mapped but never touched.
        if (is_blackhole) {
            config.in.id = tlb.out.id;
            config.in.config.addr = BH_ARC_SCRATCH & ~(TLB_2M - 1);
            config.in.config.x_end = BH_ARC_X;
            config.in.config.y_end = BH_ARC_Y;
            config.in.config.ordering = 1;
            if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &config) != 0)
                goto out_close;
        }

        regs = mmap(NULL, TLB_2M, PROT_READ | PROT_WRITE, MAP_SHARED, fd, tlb.out.mmap_offset_uc);
        if (regs == MAP_FAILED)
            goto out_close;

        if (is_blackhole && sigsetjmp(bus_jmp, 1) == 0) {
            bus_armed = true;
            for (i = 0; i < 2000 && !stop; i++) {
                (void)regs[(BH_ARC_SCRATCH & (TLB_2M - 1)) / 4];
                atomic_fetch_add(&w->beat, 1);
            }
            bus_armed = false;
        }

        // Sometimes close the fd first so the mapping outlives it.
        if (rnd(2)) {
            close(fd);
            fd = -1;
        }

        munmap((void *)regs, TLB_2M);
        atomic_fetch_add(&w->ok, 1);

out_close:
        if (fd >= 0)
            close(fd);
    }
}

static void worker_pin(struct worker *w)
{
    size_t size = 64 * sysconf(_SC_PAGESIZE);
    void *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

    if (buf == MAP_FAILED)
        return;

    while (!stop) {
        struct tenstorrent_pin_pages pin = { 0 };
        struct tenstorrent_unpin_pages unpin = { 0 };
        int fd = worker_open(w, O_APPEND);

        atomic_fetch_add(&w->beat, 1);
        if (fd < 0)
            continue;

        pin.in.output_size_bytes = sizeof(pin.out);
        pin.in.virtual_address = (uintptr_t)buf;
        pin.in.size = size;
        pin.in.flags = rnd(2) ? TENSTORRENT_PIN_PAGES_NOC_DMA : 0;

        if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) == 0) {
            atomic_fetch_add(&w->ok, 1);

            // Sometimes leave the pinning for close() or remove to undo.
            if (rnd(2)) {
                unpin.in.virtual_address = (uintptr_t)buf;
                unpin.in.size = size;
                ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin);
            } else {
                msleep(rnd(20));
            }
        }

        close(fd);
    }

    munmap(buf, size);
}

// Driver-owned DMA buffers are allocated but never mapped: remove against a
// mapped DMA buffer is a known problem that this tool does not exercise.
static void worker_dma_buf(struct worker *w)
{
    while (!stop) {
        struct tenstorrent_allocate_dma_buf buf = { 0 };
        int fd = worker_open(w, O_APPEND);

        atomic_fetch_add(&w->beat, 1);
        if (fd < 0)
            continue;

        buf.in.requested_size = sysconf(_SC_PAGESIZE);
        buf.in.buf_index = rnd(4);
        buf.in.flags = rnd(2) ? TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA : 0;

        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF, &buf) == 0) {
            atomic_fetch_add(&w->ok, 1);
            msleep(rnd(20));
        }

        close(fd);
    }
}

// Export a TLB window as a dma-buf and hold the dma-buf fd past the close of
// the device fd, and usually past a remove.
static void worker_export(struct worker *w)
{
    while (!stop) {
        struct tenstorrent_allocate_tlb tlb = { 0 };
        struct tenstorrent_export_tlb_dmabuf export = { 0 };
        int fd = worker_open(w, O_APPEND);

        atomic_fetch_add(&w->beat, 1);
        if (fd < 0)
            continue;

        tlb.in.size = TLB_2M;
        export.argsz = sizeof(export);
        export.fd = -1;

        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) == 0) {
            export.tlb_id = tlb.out.id;
            if (ioctl(fd, TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF, &export) == 0)
                atomic_fetch_add(&w->ok, 1);
            else
                export.fd = -1;
        }

        close(fd);

        if (export.fd >= 0) {
            msleep(rnd(100));
            close(export.fd);
        }
    }
}

// One fd holds a resource lock while another blocks acquiring it. Only remove
// (or the hang watchdog's signal) can end the wait.
static void worker_lock(struct worker *w)
{
    while (!stop) {
        struct tenstorrent_lock_ctl lock = { 0 };
        int holder = worker_open(w, O_APPEND);
        int waiter;

        atomic_fetch_add(&w->beat, 1);
        if (holder < 0)
            continue;

        waiter = worker_open(w, O_APPEND);
        if (waiter < 0) {
            close(holder);
            continue;
        }

        lock.in.output_size_bytes = sizeof(lock.out);
        lock.in.index = TENSTORRENT_LOCK_INDEX_ETH15;
        lock.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;

        if (ioctl(holder, TENSTORRENT_IOCTL_LOCK_CTL, &lock) == 0 && lock.out.value == 1) {
            lock.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING;
            if (ioctl(waiter, TENSTORRENT_IOCTL_LOCK_CTL, &lock) != 0 && errno == ENODEV)
                atomic_fetch_add(&w->ok, 1);
        }

        close(waiter);
        close(holder);
    }
}

static void read_file(const char *path)
{
    char buf[4096];
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return;

    while (read(fd, buf, sizeof(buf)) > 0)
        ;
    close(fd);
}

static void read_dir_files(const char *dir_path)
{
    char path[512];
    struct dirent *e;
    DIR *d = opendir(dir_path);

    if (!d)
        return;

    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_REG)
            continue;

        snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
        read_file(path);
    }

    closedir(d);
}

// Read every attribute the driver publishes outside the chardev.
static void worker_sysfs(struct worker *w)
{
    char path[512];
    char hwmon[256];

    while (!stop) {
        int ordinal = dev_ordinal();
        struct dirent *e;
        DIR *d;

        atomic_fetch_add(&w->beat, 1);
        if (ordinal < 0) {
            msleep(1);
            continue;
        }

        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/tenstorrent/tenstorrent!%d", bdf, ordinal);
        read_dir_files(path);

        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/tenstorrent/tenstorrent!%d/pcie_perf_counters",
                 bdf, ordinal);
        read_dir_files(path);

        snprintf(hwmon, sizeof(hwmon), "/sys/bus/pci/devices/%s/hwmon", bdf);
        d = opendir(hwmon);
        if (d) {
            while ((e = readdir(d)) != NULL) {
                if (strncmp(e->d_name, "hwmon", 5) != 0)
                    continue;

                snprintf(path, sizeof(path), "%s/%s", hwmon, e->d_name);
                read_dir_files(path);
            }
            closedir(d);
        }

        snprintf(path, sizeof(path), "/proc/driver/tenstorrent/%d/pids", ordinal);
        read_file(path);

        snprintf(path, sizeof(path), "/sys/kernel/debug/tenstorrent/%d/mappings", ordinal);
        read_file(path);

        atomic_fetch_add(&w->ok, 1);
    }
}

static int config_read(unsigned offset, void *value, size_t size)
{
    char path[128];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    n = pread(fd, value, size, offset);
    close(fd);
    return n == (ssize_t)size ? 0 : -1;
}

// The driver marks a reset in progress with the parity bit of the command
// register; hardware clears it when the reset completes.
static bool reset_marker_set(void)
{
    uint16_t command;

    return config_read(4, &command, sizeof(command)) == 0 && (command & 0x40);
}

// Reset the device the way tools/reset.c does, with no coordination with the
// remover.
static void worker_reset(struct worker *w)
{
    while (!stop) {
        struct tenstorrent_reset_device reset = { 0 };
        int fd;
        int i;

        atomic_fetch_add(&w->beat, 1);
        msleep(1000 + rnd(2000));

        fd = worker_open(w, O_APPEND);
        if (fd < 0)
            continue;

        reset.in.output_size_bytes = sizeof(reset.out);
        reset.in.flags = TENSTORRENT_RESET_DEVICE_ASIC_RESET;
        if (ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &reset) != 0 || reset.out.result != 0) {
            close(fd);
            continue;
        }

        for (i = 0; i < 50 && reset_marker_set(); i++) {
            atomic_fetch_add(&w->beat, 1);
            msleep(100);
        }

        // The resetting fd survives an in-place reset. If the device was
        // removed meanwhile, finish on whatever device is there now.
        if (!dev_alive(fd)) {
            close(fd);
            fd = worker_open(w, O_APPEND);
            if (fd < 0)
                continue;
        }

        memset(&reset, 0, sizeof(reset));
        reset.in.output_size_bytes = sizeof(reset.out);
        reset.in.flags = TENSTORRENT_RESET_DEVICE_POST_RESET;
        if (ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &reset) == 0 && reset.out.result == 0)
            atomic_fetch_add(&w->ok, 1);

        close(fd);
    }
}

static struct worker workers[] = {
    { "open",   worker_open_close, true },
    { "excl",   worker_open_excl,  true },
    { "ioctl",  worker_ioctl,      true },
    { "mmap",   worker_mmap,       true },
    { "pin",    worker_pin,        true },
    { "dmabuf", worker_dma_buf,    true },
    { "export", worker_export,     true },
    { "lock",   worker_lock,       true },
    { "sysfs",  worker_sysfs,      true },
    { "reset",  worker_reset,      false },
};

#define NUM_WORKERS (sizeof(workers) / sizeof(workers[0]))

static void *worker_main(void *arg)
{
    struct worker *w = arg;

    w->fn(w);
    return NULL;
}

// --- Kernel log ---

static const char *const kmsg_patterns[] = {
    "WARNING:", "BUG:", "Oops", "KASAN", "KFENCE", "general protection", "Unable to handle",
    "locking dependency", "recursive locking", "inconsistent lock state", "lock held when returning",
    "suspicious RCU", "sleeping function called", "scheduling while atomic", "blocked for more than",
    "refcount_t:", "use-after-free", "Call Trace", "DMAR:", "AMD-Vi: Event", "cannot create duplicate",
    "kernfs", "list_add corruption", "list_del corruption", "ODEBUG",
};

static int kmsg_fd = -1;
static unsigned long kmsg_hits;

static void kmsg_open(void)
{
    kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (kmsg_fd < 0) {
        perror("/dev/kmsg");
        exit(1);
    }

    lseek(kmsg_fd, 0, SEEK_END);
}

// Print every new kernel log record that looks like trouble.
static void kmsg_scan(void)
{
    char rec[8192];
    ssize_t n;

    // Each read returns one record: "prio,seq,usec,flags;text\n...".
    while ((n = read(kmsg_fd, rec, sizeof(rec) - 1)) > 0 || (n < 0 && errno == EPIPE)) {
        char *text;
        size_t i;

        if (n < 0)
            continue; // Records were overwritten before we read them.

        rec[n] = '\0';
        text = strchr(rec, ';');
        if (!text)
            continue;

        text++;
        text[strcspn(text, "\n")] = '\0';

        for (i = 0; i < sizeof(kmsg_patterns) / sizeof(kmsg_patterns[0]); i++) {
            if (strstr(text, kmsg_patterns[i])) {
                if (kmsg_hits++ < 40)
                    printf("  kmsg: %s\n", text);
                break;
            }
        }
    }
}

// --- Remover ---

static bool driver_bound(void)
{
    char path[128];

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", bdf);
    return access(path, F_OK) == 0;
}

// A device unbound mid-reset comes back with its BARs wiped, which nothing
// short of re-enumeration repairs.
static bool bars_wiped(void)
{
    uint64_t bar0; // 64-bit BAR: both dwords

    return config_read(0x10, &bar0, sizeof(bar0)) == 0 && (bar0 & ~0xFULL) == 0;
}

static unsigned long recoveries;

static bool recover_device(void)
{
    char path[128];
    int i;

    for (i = 0; i < 50 && reset_marker_set(); i++)
        msleep(100);

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/remove", bdf);
    write_str(path, "1");
    msleep(100);
    write_str("/sys/bus/pci/rescan", "1");

    for (i = 0; i < 50; i++) {
        if (driver_bound() && !bars_wiped()) {
            recoveries++;
            return true;
        }
        msleep(100);
    }

    return false;
}

static void on_kick(int sig)
{
    (void)sig;
}

static void check_hangs(void)
{
    double t = now();
    size_t i;

    for (i = 0; i < NUM_WORKERS; i++) {
        struct worker *w = &workers[i];
        unsigned long beat = atomic_load(&w->beat);

        if (!w->enabled)
            continue;

        if (beat != w->last_beat) {
            w->last_beat = beat;
            w->last_beat_time = t;
            continue;
        }

        if (t - w->last_beat_time > HANG_SECONDS) {
            printf("  worker %s made no progress for %d s; interrupting it\n", w->name, HANG_SECONDS);
            w->hangs++;
            w->last_beat_time = t;
            pthread_kill(w->thread, SIGUSR1);
        }
    }
}

static void usage(const char *argv0)
{
    size_t i;

    fprintf(stderr, "Usage: %s [-t seconds] [-r] [-w workers] <bdf>\n  workers:", argv0);
    for (i = 0; i < NUM_WORKERS; i++)
        fprintf(stderr, " %s", workers[i].name);
    fprintf(stderr, "\n");
    exit(2);
}

static void select_workers(const char *list)
{
    char *copy = strdup(list);
    char *name;
    size_t i;

    for (i = 0; i < NUM_WORKERS; i++)
        workers[i].enabled = false;

    for (name = strtok(copy, ","); name; name = strtok(NULL, ",")) {
        for (i = 0; i < NUM_WORKERS; i++) {
            if (strcmp(name, workers[i].name) == 0) {
                workers[i].enabled = true;
                break;
            }
        }

        if (i == NUM_WORKERS) {
            fprintf(stderr, "Unknown worker: %s\n", name);
            exit(2);
        }
    }

    free(copy);
}

int main(int argc, char *argv[])
{
    struct sigaction sa = { 0 };
    const char *unbind = "/sys/bus/pci/drivers/tenstorrent/unbind";
    const char *bind = "/sys/bus/pci/drivers/tenstorrent/bind";
    const char *worker_list = NULL;
    unsigned long hangs = 0;
    bool with_reset = false;
    bool failed = false;
    int seconds = 30;
    uint16_t device_id;
    double end;
    size_t i;
    int opt;
    int ret;

    while ((opt = getopt(argc, argv, "t:rw:")) != -1) {
        switch (opt) {
        case 't': seconds = atoi(optarg); break;
        case 'r': with_reset = true; break;
        case 'w': worker_list = optarg; break;
        default: usage(argv[0]);
        }
    }

    if (optind != argc - 1)
        usage(argv[0]);

    bdf = argv[optind];

    if (worker_list)
        select_workers(worker_list);
    else
        workers[NUM_WORKERS - 1].enabled = with_reset;

    for (i = 0; i < NUM_WORKERS; i++)
        if (workers[i].fn == worker_reset && workers[i].enabled)
            with_reset = true;

    if (config_read(2, &device_id, sizeof(device_id)) != 0) {
        fprintf(stderr, "%s: no such PCI device\n", bdf);
        return 2;
    }
    is_blackhole = (device_id == BLACKHOLE_PCI_DEVICE_ID);

    if (!driver_bound()) {
        fprintf(stderr, "%s: not bound to the tenstorrent driver\n", bdf);
        return 2;
    }

    sa.sa_handler = on_sigbus;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);

    // No SA_RESTART: the signal exists to break a worker out of a blocked call.
    sa.sa_handler = on_kick;
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    kmsg_open();

    for (i = 0; i < NUM_WORKERS; i++) {
        workers[i].last_beat_time = now();
        if (workers[i].enabled)
            pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]);
    }

    end = now() + seconds;
    while (now() < end) {
        // Vary how far the workers get before the device goes away.
        msleep(rnd(4) ? rnd(30) : 100 + rnd(200));

        ret = write_str(unbind, bdf);
        if (ret != 0 && ret != -ENODEV) {
            printf("unbind failed: %s\n", strerror(-ret));
            failed = true;
            break;
        }

        msleep(rnd(10));

        ret = write_str(bind, bdf);
        if ((ret != 0 && ret != -EBUSY) || !driver_bound() || (with_reset && bars_wiped())) {
            if (!with_reset || !recover_device()) {
                printf("bind failed (%s) and the device could not be recovered\n", strerror(-ret));
                failed = true;
                break;
            }
        }

        atomic_fetch_add(&cycles, 1);
        kmsg_scan();
        check_hangs();
    }

    stop = true;

    // Blocked workers need the device gone, or a signal, to notice stop.
    for (i = 0; i < NUM_WORKERS; i++) {
        struct timespec ts;
        int joined = -1;
        int tries;

        if (!workers[i].enabled)
            continue;

        for (tries = 0; tries < 20 && joined != 0; tries++) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }

            joined = pthread_timedjoin_np(workers[i].thread, NULL, &ts);
            if (joined != 0)
                pthread_kill(workers[i].thread, SIGUSR1);
        }

        if (joined != 0) {
            printf("worker %s would not stop\n", workers[i].name);
            workers[i].hangs++;
        }
    }

    msleep(500);
    kmsg_scan();

    printf("\n%lu remove cycles in %d s", atomic_load(&cycles), seconds);
    if (with_reset)
        printf(", %lu recoveries from a wiped config space", recoveries);
    printf("\n%-8s %12s %12s %10s %6s\n", "worker", "iterations", "completed", "dead fds", "hangs");

    for (i = 0; i < NUM_WORKERS; i++) {
        struct worker *w = &workers[i];

        if (!w->enabled)
            continue;

        printf("%-8s %12lu %12lu %10lu %6lu\n", w->name, atomic_load(&w->beat), atomic_load(&w->ok),
               atomic_load(&w->dead_fds), w->hangs);
        hangs += w->hangs;
    }

    printf("kernel log: %lu suspicious record%s\n", kmsg_hits, kmsg_hits == 1 ? "" : "s");

    return (failed || hangs || kmsg_hits) ? 1 : 0;
}
