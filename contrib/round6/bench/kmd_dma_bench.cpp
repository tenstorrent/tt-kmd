// Benchmark for TENSTORRENT_URING_CMD_DMA_WRITE (tt-kmd ajovanovic/bh_kmd_dma_chain).
// One thread, one io_uring, many chips. Prints one result line per run as key=value.
//
//   kmd_dma_bench --chips 0-31 --elem 16384 --descs 64 --inflight 2 --banks 1 [--interleave]
//                 [--open GBPS] [--secs 3] [--warmup 0.5] [--sqpoll CPU] [--verify-chips 0,5,13,31]

#include <liburing.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ioctl.h"

// Staging buffer per chip; --buf-kib shrinks it, e.g. to keep the sources in the CPU's L3.
static uint64_t BUF_BYTES = 256ull << 20;
static constexpr uint64_t DRAM_BASE = 0x10000000;  // destinations stay in [DRAM_BASE, DRAM_BASE + BUF_BYTES)
static constexpr uint64_t MAX_BUF_BYTES = 1ull << 30;  // keeps destinations below 0xFF000000, in usable GDDR

// Translated DRAM bank coordinates (port 0) on an unharvested Blackhole.
static const uint16_t BANK_X[8] = {17, 17, 17, 17, 18, 18, 18, 18};
static const uint16_t BANK_Y[8] = {12, 15, 18, 21, 12, 15, 18, 21};

static double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static double thread_cpu_s() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
[[noreturn]] static void die(const char *msg, int err = 0) {
    fprintf(stderr, "FATAL: %s %s\n", msg, err ? strerror(err) : "");
    exit(1);
}

static std::vector<int> parse_list(const std::string &s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto dash = tok.find('-');
        if (dash == std::string::npos) {
            out.push_back(std::stoi(tok));
        } else {
            for (int i = std::stoi(tok.substr(0, dash)); i <= std::stoi(tok.substr(dash + 1)); i++) out.push_back(i);
        }
    }
    return out;
}

// CPU time of this process's threads, by kind (main / iou-sqp / iou-wrk), from schedstat (ns).
static std::map<std::string, double> task_cpu_s() {
    std::map<std::string, double> out;
    DIR *d = opendir("/proc/self/task");
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string base = std::string("/proc/self/task/") + e->d_name;
        std::ifstream comm(base + "/comm"), sched(base + "/schedstat");
        std::string name;
        double ns = 0;
        std::getline(comm, name);
        sched >> ns;
        std::string kind = name.rfind("iou-sqp", 0) == 0 ? "sqp" : name.rfind("iou-wrk", 0) == 0 ? "wrk" : "main";
        out[kind] += ns * 1e-9;
    }
    closedir(d);
    return out;
}

// Fills a DMA_WRITE SQE. Built by hand: liburing 2.1's prep helpers write a field
// that, in the 6.8 SQE layout used here, lands in the next SQE.
static void prep_dma_write(io_uring_sqe *sqe, int reg_index, const tenstorrent_dma_desc *descs, uint32_t count,
                           uint64_t user_data) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->fd = reg_index;
    sqe->cmd_op = TENSTORRENT_URING_CMD_DMA_WRITE;
    sqe->user_data = user_data;
    tenstorrent_dma_write w{};
    w.descs = reinterpret_cast<uintptr_t>(descs);
    w.count = count;
    memcpy(sqe->cmd, &w, sizeof(w));
}

// HDMA read channel 0 registers in BAR2, for the prefetch experiment.
static constexpr uint32_t HDMA_RD_ELEM_PF = 0x108;
static constexpr uint32_t HDMA_RD_QOS = 0x13C;
static constexpr uint32_t QOS_PF_DEPTH_SHIFT = 16, QOS_PF_DEPTH_MASK = 0x3FF;

struct Chip;
struct Slot {
    Chip *chip;
    int index;
    bool busy = false;
    double t_submit = 0;
    std::vector<tenstorrent_dma_desc> descs;
    std::vector<uint64_t> stamps;  // --stamp: what each element's first and last word must read back as
};

struct Chip {
    int id;
    int fd;
    int reg_index;
    std::string bdf;
    uint8_t *buf;
    uint64_t iova;
    uint64_t cursor = 0;  // next source offset
    volatile uint32_t *bar2 = nullptr;
    std::vector<Slot> slots;
    int inflight = 0;
    uint64_t bytes = 0, cmds = 0;
    int irq[2] = {-1, -1};
};

struct Opts {
    std::vector<int> chips;
    uint32_t elem = 16384, descs = 64;
    int inflight = 1, banks = 1;
    bool interleave = false;
    double open_gbps = 0;
    double secs = 3, warmup = 0.5;
    int sqpoll_cpu = -1;
    std::vector<int> verify_chips;
    int max_open_inflight = 64;
    int pf_depth = -1;  // QOS.PF_DEPTH to set before the first command; -1 leaves it alone
    // Stress mode: unique stamps in every element, every element read back, errors counted not fatal.
    bool stamp = false;
    bool defer = false;  // IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN
    // --dst X,Y,BASE: one non-DRAM target (e.g. a Tensix L1) instead of the DRAM banks. Destination is
    // BASE + source offset, so the staging buffer (--buf-kib) bounds the destination range.
    int dst_x = -1, dst_y = -1;
    uint64_t dst_base = 0;
};

static uint64_t pattern(int chip, uint64_t word_index) { return (uint64_t(chip) << 56) ^ (word_index * 0x9E3779B97F4A7C15ull); }

static uint64_t noc_read64(int fd, uint16_t x, uint16_t y, uint64_t addr) {
    tenstorrent_noc_io r{};
    r.argsz = sizeof(r);
    r.x = x;
    r.y = y;
    r.width = 8;
    r.addr = addr;
    if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &r) != 0) die("NOC_READ", errno);
    return r.value;
}

static void pin(Chip &c) {
    void *p = mmap(nullptr, BUF_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap", errno);
    madvise(p, BUF_BYTES, MADV_HUGEPAGE);
    c.buf = static_cast<uint8_t *>(p);
    auto *w = reinterpret_cast<uint64_t *>(c.buf);
    for (uint64_t i = 0; i < BUF_BYTES / 8; i++) w[i] = pattern(c.id, i);

    tenstorrent_pin_pages pin{};
    pin.in.output_size_bytes = sizeof(pin.out);
    pin.in.virtual_address = reinterpret_cast<uintptr_t>(p);
    pin.in.size = BUF_BYTES;
    if (ioctl(c.fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0) die("PIN_PAGES", errno);
    c.iova = pin.out.physical_address;
}

static void find_irqs(Chip &c) {
    std::string dir = "/sys/bus/pci/devices/" + c.bdf + "/msi_irqs";
    std::vector<int> irqs;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (dirent *e = readdir(d))
        if (e->d_name[0] != '.') irqs.push_back(atoi(e->d_name));
    closedir(d);
    std::sort(irqs.begin(), irqs.end());
    for (size_t i = 0; i < irqs.size() && i < 2; i++) c.irq[i] = irqs[i];
}

static std::map<int, uint64_t> read_interrupts() {
    std::map<int, uint64_t> out;
    std::ifstream f("/proc/interrupts");
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string irq;
        ss >> irq;
        if (irq.empty() || !isdigit(irq[0])) continue;
        uint64_t sum = 0, v;
        while (ss >> v) sum += v;
        out[atoi(irq.c_str())] = sum;
    }
    return out;
}

struct Bench {
    Opts o;
    io_uring ring;
    std::vector<Chip> chips;
    uint64_t cmd_bytes;
    uint64_t usable_bytes;  // largest whole number of elements that fits the buffer
    uint64_t dst_base() const { return o.dst_x >= 0 ? o.dst_base : DRAM_BASE; }
    std::vector<double> lat_us;
    bool measuring = false;
    uint64_t verify_count = 0, verify_fail = 0, cmd_seq = 0;
    uint64_t stamp_seq = 0, stamp_bad_elements = 0, stamp_elements = 0, cqe_errors = 0;
    uint64_t stamp_late = 0, stamp_skipped = 0;
    double verify_time = 0;

    void fill_slot(Slot &s) {
        Chip &c = *s.chip;
        for (uint32_t i = 0; i < o.descs; i++) {
            int bank = o.interleave ? i % o.banks : i * o.banks / o.descs;
            // Commands larger than the buffer reuse it; wrapping on a whole number of elements
            // keeps every element inside the buffer.
            uint64_t off = (c.cursor + uint64_t(i) * o.elem) % usable_bytes;
            tenstorrent_dma_desc &d = s.descs[i];
            d = {};
            d.src = c.iova + off;
            d.addr = dst_base() + off;
            d.size = o.elem;
            d.x = o.dst_x >= 0 ? o.dst_x : BANK_X[bank];
            d.y = o.dst_x >= 0 ? o.dst_y : BANK_Y[bank];
            if (o.stamp) {
                // Sources are reused only a full buffer later, long after this command completes.
                uint64_t st = (++stamp_seq << 8) | uint64_t(c.id);
                auto *w = reinterpret_cast<uint64_t *>(c.buf + off);
                w[0] = st;
                w[o.elem / 8 - 1] = ~st;
                s.stamps[i] = st;
            }
        }
        c.cursor = (c.cursor + cmd_bytes) % usable_bytes;
    }

    void queue(Slot &s) {
        fill_slot(s);
        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (!sqe) die("SQ full");
        prep_dma_write(sqe, s.chip->reg_index, s.descs.data(), o.descs, reinterpret_cast<uintptr_t>(&s));
        s.busy = true;
        s.t_submit = now_s();
        s.chip->inflight++;
    }

    // Reads the first and last word of one descriptor back. Destination contents only ever depend
    // on the source offset, so later commands rewriting the same address do not change the answer.
    void verify(const Slot &s) {
        double t0 = now_s();
        const tenstorrent_dma_desc &d = s.descs[(cmd_seq / 10) % o.descs];
        uint64_t off = d.src - s.chip->iova;
        for (uint64_t w : {off, off + d.size - 8}) {
            uint64_t got = noc_read64(s.chip->fd, d.x, d.y, dst_base() + w);
            uint64_t want = pattern(s.chip->id, w / 8);
            verify_count++;
            if (got != want) {
                verify_fail++;
                fprintf(stderr, "VERIFY MISMATCH chip %d addr 0x%lx got 0x%lx want 0x%lx\n", s.chip->id,
                        dst_base() + w, got, want);
            }
        }
        verify_time += now_s() - t0;
    }

    void verify_stamps(const Slot &s) {
        for (uint32_t i = 0; i < o.descs; i++) {
            const tenstorrent_dma_desc &d = s.descs[i];
            uint64_t first = noc_read64(s.chip->fd, d.x, d.y, d.addr);
            uint64_t last = noc_read64(s.chip->fd, d.x, d.y, d.addr + d.size - 8);
            stamp_elements++;
            if (first != s.stamps[i] || last != ~s.stamps[i]) {
                // Re-read later: data that shows up was late (completion before visibility);
                // data that never does was skipped (a torn descriptor).
                usleep(100);
                bool late_100us = noc_read64(s.chip->fd, d.x, d.y, d.addr) == s.stamps[i] &&
                                  noc_read64(s.chip->fd, d.x, d.y, d.addr + d.size - 8) == ~s.stamps[i];
                usleep(10000);
                bool late_10ms = noc_read64(s.chip->fd, d.x, d.y, d.addr) == s.stamps[i] &&
                                 noc_read64(s.chip->fd, d.x, d.y, d.addr + d.size - 8) == ~s.stamps[i];
                (late_100us || late_10ms ? stamp_late : stamp_skipped)++;
                if (stamp_bad_elements++ < 10)
                    fprintf(stderr, "STAMP MISMATCH chip %d addr 0x%llx first 0x%lx last 0x%lx want 0x%lx -> %s\n",
                            s.chip->id, (unsigned long long)d.addr, first, last, s.stamps[i],
                            late_100us ? "late (<100us)" : late_10ms ? "late (<10ms)" : "skipped");
            }
        }
    }

    Slot *complete(io_uring_cqe *cqe) {
        Slot &s = *static_cast<Slot *>(io_uring_cqe_get_data(cqe));
        if (cqe->res != 0 && o.stamp) {
            if (cqe_errors++ < 5) fprintf(stderr, "CQE error chip %d: %s\n", s.chip->id, strerror(-cqe->res));
            s.busy = false;
            s.chip->inflight--;
            return &s;
        }
        if (o.stamp) verify_stamps(s);
        if (cqe->res != 0) {
            fprintf(stderr, "CQE error chip %d: %s\n", s.chip->id, strerror(-cqe->res));
            exit(2);
        }
        double t = now_s();
        s.busy = false;
        s.chip->inflight--;
        if (measuring) {
            lat_us.push_back((t - s.t_submit) * 1e6);
            s.chip->bytes += cmd_bytes;
            s.chip->cmds++;
        }
        cmd_seq++;
        if (cmd_seq % 10 == 0 &&
            std::find(o.verify_chips.begin(), o.verify_chips.end(), s.chip->id) != o.verify_chips.end())
            verify(s);
        return &s;
    }

    void setup() {
        cmd_bytes = uint64_t(o.elem) * o.descs;
        usable_bytes = BUF_BYTES / o.elem * o.elem;
        io_uring_params p{};
        p.cq_entries = 4096;
        p.flags = IORING_SETUP_CQSIZE;
        // Completion task work then runs only when this thread waits, in batches.
        if (o.defer) p.flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
        if (o.sqpoll_cpu >= 0) {
            p.flags |= IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
            p.sq_thread_cpu = o.sqpoll_cpu;
            p.sq_thread_idle = 2000;
        }
        int ret = io_uring_queue_init_params(1024, &ring, &p);
        if (ret) die("io_uring_queue_init", -ret);

        std::vector<int> fds;
        for (int id : o.chips) {
            Chip c;
            c.id = id;
            std::string path = "/dev/tenstorrent/" + std::to_string(id);
            c.fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (c.fd < 0) die(path.c_str(), errno);
            tenstorrent_get_device_info info{};
            info.in.output_size_bytes = sizeof(info.out);
            if (ioctl(c.fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info)) die("GET_DEVICE_INFO", errno);
            char bdf[32];
            snprintf(bdf, sizeof(bdf), "%04x:%02x:%02x.%x", info.out.pci_domain, info.out.bus_dev_fn >> 8,
                     (info.out.bus_dev_fn >> 3) & 0x1f, info.out.bus_dev_fn & 7);
            c.bdf = bdf;
            c.reg_index = fds.size();
            fds.push_back(c.fd);
            pin(c);
            find_irqs(c);
            chips.push_back(std::move(c));
        }
        ret = io_uring_register_files(&ring, fds.data(), fds.size());
        if (ret) die("io_uring_register_files", -ret);

        int nslots = o.open_gbps > 0 ? o.max_open_inflight : o.inflight;
        for (Chip &c : chips) {
            c.slots.resize(nslots);
            for (int i = 0; i < nslots; i++) {
                c.slots[i].chip = &c;
                c.slots[i].index = i;
                c.slots[i].descs.resize(o.descs);
                c.slots[i].stamps.resize(o.descs);
            }
        }
    }

    // The KMD reads QOS.PF_DEPTH when it sets up an idle channel, so this must run before any DMA.
    void set_pf_depth() {
        for (Chip &c : chips) {
            std::string path = "/sys/bus/pci/devices/" + c.bdf + "/resource2";
            int fd = open(path.c_str(), O_RDWR | O_SYNC);
            if (fd < 0) die(path.c_str(), errno);
            void *p = mmap(nullptr, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (p == MAP_FAILED) die("mmap BAR2", errno);
            c.bar2 = static_cast<volatile uint32_t *>(p);
            if (o.pf_depth < 0) continue;
            uint32_t qos = c.bar2[HDMA_RD_QOS / 4];
            qos = (qos & ~(QOS_PF_DEPTH_MASK << QOS_PF_DEPTH_SHIFT)) | (uint32_t(o.pf_depth) << QOS_PF_DEPTH_SHIFT);
            c.bar2[HDMA_RD_QOS / 4] = qos;
            if (c.bar2[HDMA_RD_QOS / 4] != qos) die("QOS write did not stick");
        }
    }

    // One small DMA per bank per chip, read back; proves coordinates and the path work.
    void check_targets() {
        for (Chip &c : chips) {
            for (int b = 0; b < o.banks; b++) {
                Slot &s = c.slots[0];
                tenstorrent_dma_desc d{};
                uint64_t off = (uint64_t(b) * 2 * 4096) % (BUF_BYTES - 4096);
                d.src = c.iova + off;
                d.addr = dst_base() + off;
                d.size = 4096;
                d.x = o.dst_x >= 0 ? o.dst_x : BANK_X[b];
                d.y = o.dst_x >= 0 ? o.dst_y : BANK_Y[b];
                s.descs.assign(1, d);
                io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                prep_dma_write(sqe, c.reg_index, s.descs.data(), 1, 0xC0DE0000ull | (c.id << 8) | b);
                io_uring_submit(&ring);
                io_uring_cqe *cqe;
                if (io_uring_wait_cqe(&ring, &cqe)) die("wait_cqe");
                if (cqe->res) {
                    fprintf(stderr, "target check chip %d bank %d: %s\n", c.id, b, strerror(-cqe->res));
                    exit(3);
                }
                io_uring_cqe_seen(&ring, cqe);
                for (uint64_t w8 : {off, off + 4096 - 8}) {
                    uint64_t got = noc_read64(c.fd, d.x, d.y, dst_base() + w8);
                    if (got != pattern(c.id, w8 / 8)) {
                        fprintf(stderr, "target check chip %d bank %d (%d,%d): got 0x%lx want 0x%lx\n", c.id, b,
                                d.x, d.y, got, pattern(c.id, w8 / 8));
                        exit(3);
                    }
                }
            }
            c.slots[0].descs.resize(o.descs);
        }
    }

    Slot *free_slot(Chip &c) {
        for (Slot &s : c.slots)
            if (!s.busy) return &s;
        return nullptr;
    }

    void drain() {
        while (true) {
            int inflight = 0;
            for (Chip &c : chips) inflight += c.inflight;
            if (!inflight) return;
            io_uring_submit_and_wait(&ring, 1);
            io_uring_cqe *cqe;
            unsigned head, n = 0;
            io_uring_for_each_cqe(&ring, head, cqe) {
                complete(cqe);
                n++;
            }
            io_uring_cq_advance(&ring, n);
        }
    }

    void run() {
        double t_end_warm = now_s() + o.warmup, t_end = t_end_warm + o.secs;
        double t0 = 0, cpu0 = 0;
        std::map<std::string, double> task0;
        std::map<int, uint64_t> irq0;
        rusage ru0{};
        uint64_t open_skipped = 0;
        size_t rr = 0;
        double interval = o.open_gbps > 0 ? cmd_bytes / (o.open_gbps * 1e9) : 0;
        double next_due = now_s();

        if (o.open_gbps <= 0)
            for (Chip &c : chips)
                for (Slot &s : c.slots) queue(s);

        while (true) {
            double t = now_s();
            if (!measuring && t >= t_end_warm) {
                measuring = true;
                t0 = t;
                cpu0 = thread_cpu_s();
                task0 = task_cpu_s();
                irq0 = read_interrupts();
                getrusage(RUSAGE_SELF, &ru0);
            }
            if (t >= t_end) break;

            if (o.open_gbps > 0) {
                while (next_due <= t) {
                    Chip &c = chips[rr++ % chips.size()];
                    if (Slot *s = free_slot(c))
                        queue(*s);
                    else if (measuring)
                        open_skipped++;
                    next_due += interval;
                }
                io_uring_submit(&ring);
                double wait = next_due - now_s();
                io_uring_cqe *cqe;
                if (wait > 0) {
                    __kernel_timespec ts{0, (long long)(wait * 1e9)};
                    io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
                }
            } else {
                io_uring_submit_and_wait(&ring, 1);
            }

            io_uring_cqe *cqe;
            unsigned head, n = 0;
            std::vector<Slot *> done;
            io_uring_for_each_cqe(&ring, head, cqe) {
                done.push_back(complete(cqe));
                n++;
            }
            io_uring_cq_advance(&ring, n);
            if (o.open_gbps <= 0)
                for (Slot *s : done) queue(*s);
        }

        double wall = now_s() - t0;
        uint32_t qos = chips[0].bar2[HDMA_RD_QOS / 4], elem_pf = chips[0].bar2[HDMA_RD_ELEM_PF / 4];
        double cpu = thread_cpu_s() - cpu0;
        auto task1 = task_cpu_s();
        auto irq1 = read_interrupts();
        rusage ru1{};
        getrusage(RUSAGE_SELF, &ru1);
        measuring = false;
        drain();

        uint64_t bytes = 0, cmds = 0, irq_v[2] = {0, 0};
        double min_chip = 1e30, max_chip = 0;
        int min_id = -1;
        std::string per_chip;
        for (Chip &c : chips) {
            bytes += c.bytes;
            cmds += c.cmds;
            double g = c.bytes / wall / 1e9;
            if (g < min_chip) min_chip = g, min_id = c.id;
            max_chip = std::max(max_chip, g);
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%s%d:%.2f", per_chip.empty() ? "" : ",", c.id, g);
            per_chip += tmp;
            for (int v = 0; v < 2; v++)
                if (c.irq[v] >= 0) irq_v[v] += irq1[c.irq[v]] - irq0[c.irq[v]];
        }
        std::sort(lat_us.begin(), lat_us.end());
        auto pct = [&](double p) { return lat_us.empty() ? 0 : lat_us[std::min(lat_us.size() - 1, size_t(p * lat_us.size()))]; };
        auto tv = [](const timeval &a) { return a.tv_sec + a.tv_usec * 1e-6; };
        double proc_cpu = tv(ru1.ru_utime) + tv(ru1.ru_stime) - tv(ru0.ru_utime) - tv(ru0.ru_stime);

        printf("RESULT chips=%zu elem=%u descs=%u inflight=%d banks=%d interleave=%d open_gbps=%.1f sqpoll=%d "
               "wall=%.2f GBps=%.3f Melem_s=%.4f cmds_s=%.0f min_chip_GBps=%.3f min_chip=%d max_chip_GBps=%.3f "
               "lat_p50_us=%.1f lat_p99_us=%.1f lat_max_us=%.1f thread_cpu_pct=%.1f verify_cpu_pct=%.1f "
               "proc_cpu_pct=%.1f sqp_cpu_pct=%.1f wrk_cpu_pct=%.1f irq0_per_cmd=%.3f irq1_per_cmd=%.3f "
               "open_skipped=%lu verify=%lu/%lu qos=0x%x elem_pf=%u stamp_ok=%lu/%lu stamp_late=%lu stamp_skipped=%lu cqe_errors=%lu per_chip=%s\n",
               chips.size(), o.elem, o.descs, o.inflight, o.banks, o.interleave, o.open_gbps, o.sqpoll_cpu, wall,
               bytes / wall / 1e9, cmds * o.descs / wall / 1e6, cmds / wall, min_chip, min_id, max_chip, pct(0.5),
               pct(0.99), lat_us.empty() ? 0 : lat_us.back(), 100 * cpu / wall, 100 * verify_time / wall,
               100 * proc_cpu / wall, 100 * (task1["sqp"] - task0["sqp"]) / wall,
               100 * (task1["wrk"] - task0["wrk"]) / wall, double(irq_v[0]) / cmds, double(irq_v[1]) / cmds,
               open_skipped, verify_count - verify_fail, verify_count, qos, elem_pf, stamp_elements - stamp_bad_elements,
               stamp_elements, stamp_late, stamp_skipped, cqe_errors, per_chip.c_str());
        fflush(stdout);
        if (verify_fail || stamp_bad_elements || cqe_errors) exit(4);
    }
};

int main(int argc, char **argv) {
    Bench b;
    Opts &o = b.o;
    o.chips = {0};
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) die("missing argument");
            return argv[++i];
        };
        if (a == "--chips") o.chips = parse_list(next());
        else if (a == "--elem") o.elem = std::stoul(next());
        else if (a == "--descs") o.descs = std::stoul(next());
        else if (a == "--inflight") o.inflight = std::stoi(next());
        else if (a == "--banks") o.banks = std::stoi(next());
        else if (a == "--interleave") o.interleave = true;
        else if (a == "--open") o.open_gbps = std::stod(next());
        else if (a == "--secs") o.secs = std::stod(next());
        else if (a == "--warmup") o.warmup = std::stod(next());
        else if (a == "--sqpoll") o.sqpoll_cpu = std::stoi(next());
        else if (a == "--verify-chips") o.verify_chips = parse_list(next());
        else if (a == "--pf-depth") o.pf_depth = std::stoi(next());
        else if (a == "--stamp") o.stamp = true;
        else if (a == "--defer") o.defer = true;
        else if (a == "--buf-kib") BUF_BYTES = std::stoull(next()) << 10;
        else if (a == "--dst") {
            std::string v = next();
            if (sscanf(v.c_str(), "%d,%d,%li", &o.dst_x, &o.dst_y, (long *)&o.dst_base) != 3) die("--dst X,Y,BASE");
        }
        else die(("unknown option " + a).c_str());
    }
    if (o.banks < 1 || o.banks > 8 || o.elem % 8 || o.elem > BUF_BYTES || BUF_BYTES > MAX_BUF_BYTES ||
        BUF_BYTES % 4096)
        die("bad options");
    if (o.pf_depth > 15) die("pf-depth above 15 was never validated");
    b.setup();
    b.set_pf_depth();
    b.check_targets();
    b.run();
    return 0;
}
