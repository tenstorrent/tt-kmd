// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <iostream>
#include <vector>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "tlbs.h"
#include "test_failure.h"

namespace
{

uint32_t allocate_window(int fd, size_t size = TWO_MEG)
{
    tenstorrent_allocate_tlb alloc{};
    alloc.in.size = size;
    if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
        THROW_TEST_FAILURE("Failed to allocate TLB window");
    return alloc.out.id;
}

void free_window(int fd, uint32_t id)
{
    tenstorrent_free_tlb free_tlb{};
    free_tlb.in.id = id;
    if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0)
        THROW_TEST_FAILURE("Failed to free TLB window");
}

// Export [offset, offset+size) of a window. Returns the ioctl result (0 on
// success); on success the dma-buf fd is stored in *out_fd.
int try_export(int fd, uint32_t tlb_id, uint64_t offset, uint64_t size, int *out_fd)
{
    tenstorrent_export_tlb_dmabuf exp{};
    exp.argsz = sizeof(exp);
    exp.tlb_id = tlb_id;
    exp.offset = offset;
    exp.size = size;

    errno = 0;
    int ret = ioctl(fd, TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF, &exp);
    if (ret == 0)
        *out_fd = exp.fd;
    return ret;
}

// The dma-buf export path requires Linux >= 5.8; on older kernels the driver
// still builds but the ioctl returns -EOPNOTSUPP. Probe so the suite can skip
// gracefully there rather than report spurious failures.
bool export_supported(int fd)
{
    uint32_t id = allocate_window(fd);
    int dmabuf_fd = -1;
    int ret = try_export(fd, id, 0, 0, &dmabuf_fd);
    bool supported = true;

    if (ret == 0)
        close(dmabuf_fd);
    else if (errno == EOPNOTSUPP)
        supported = false;

    free_window(fd, id);
    return supported;
}

// A basic export of a whole window should succeed and yield a usable fd.
void VerifyExportBasic(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    uint32_t id = allocate_window(fd);

    int dmabuf_fd = -1;
    if (try_export(fd, id, 0, 0, &dmabuf_fd) != 0)
        THROW_TEST_FAILURE("Failed to export TLB window as dma-buf");
    if (dmabuf_fd < 0)
        THROW_TEST_FAILURE("Export returned an invalid fd");

    close(dmabuf_fd);
    free_window(fd, id);
}

// Malformed export requests must be rejected with EINVAL.
void VerifyExportBadArgs(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    uint32_t id = allocate_window(fd);

    auto expect_einval = [&](tenstorrent_export_tlb_dmabuf exp, const char *what) {
        errno = 0;
        int ret = ioctl(fd, TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF, &exp);
        if (ret == 0) {
            close(exp.fd);
            THROW_TEST_FAILURE(what);
        }
        if (errno != EINVAL)
            THROW_TEST_FAILURE(what);
    };

    {
        tenstorrent_export_tlb_dmabuf exp{};
        exp.argsz = sizeof(exp) + 1;
        exp.tlb_id = id;
        expect_einval(exp, "Export accepted a bad argsz");
    }
    {
        tenstorrent_export_tlb_dmabuf exp{};
        exp.argsz = sizeof(exp);
        exp.flags = 1;
        exp.tlb_id = id;
        expect_einval(exp, "Export accepted nonzero flags");
    }
    {
        tenstorrent_export_tlb_dmabuf exp{};
        exp.argsz = sizeof(exp);
        exp.tlb_id = 0xFFFFFFFFu;
        expect_einval(exp, "Export accepted an out-of-range tlb_id");
    }
    {
        tenstorrent_export_tlb_dmabuf exp{};
        exp.argsz = sizeof(exp);
        exp.tlb_id = id;
        exp.offset = 0x1; // not page-aligned
        expect_einval(exp, "Export accepted a misaligned offset");
    }
    {
        tenstorrent_export_tlb_dmabuf exp{};
        exp.argsz = sizeof(exp);
        exp.tlb_id = id;
        exp.size = 0x1; // not page-aligned
        expect_einval(exp, "Export accepted a misaligned size");
    }
    {
        tenstorrent_export_tlb_dmabuf exp{};
        exp.argsz = sizeof(exp);
        exp.tlb_id = id;
        exp.offset = TWO_MEG; // at/after end of window
        expect_einval(exp, "Export accepted an out-of-range offset");
    }

    free_window(fd, id);
}

// A window may only be exported by the fd that owns it.
void VerifyExportRequiresOwnership(const EnumeratedDevice &dev)
{
    DevFd owner(dev.path);
    DevFd other(dev.path);

    uint32_t id = allocate_window(owner.get());

    int dmabuf_fd = -1;
    errno = 0;
    int ret = try_export(other.get(), id, 0, 0, &dmabuf_fd);
    if (ret == 0) {
        close(dmabuf_fd);
        free_window(owner.get(), id);
        THROW_TEST_FAILURE("Exported a window not owned by the fd");
    }
    if (errno != EPERM) {
        free_window(owner.get(), id);
        THROW_TEST_FAILURE("Expected EPERM exporting an unowned window");
    }

    free_window(owner.get(), id);
}

// The headline case: a live dma-buf export keeps its TLB window allocated
// across FREE_TLB (and would across fd close); the window only returns to the
// pool when the dma-buf is closed. Observed by exhausting the window pool: a
// freed-but-exported window must not become re-allocatable until the dma-buf
// is released.
void VerifyExportKeepsTlbAliveAcrossFree(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    // Exhaust every 2M window so the pool is full.
    std::vector<uint32_t> ids;
    for (;;) {
        tenstorrent_allocate_tlb alloc{};
        alloc.in.size = TWO_MEG;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
            break;
        ids.push_back(alloc.out.id);
    }
    if (ids.empty())
        THROW_TEST_FAILURE("Could not allocate any 2M TLB window");

    auto free_all = [&]() {
        for (uint32_t id : ids) {
            tenstorrent_free_tlb free_tlb{};
            free_tlb.in.id = id;
            ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb);
        }
    };

    // Export one window, then drop the owner's reference with FREE_TLB. With
    // the refcount model FREE_TLB succeeds, but the export keeps the window
    // out of the pool.
    uint32_t exported_id = ids.back();
    ids.pop_back();

    int dmabuf_fd = -1;
    if (try_export(fd, exported_id, 0, 0, &dmabuf_fd) != 0) {
        free_all();
        THROW_TEST_FAILURE("Failed to export TLB window as dma-buf");
    }

    {
        tenstorrent_free_tlb free_tlb{};
        free_tlb.in.id = exported_id;
        if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0) {
            close(dmabuf_fd);
            free_all();
            THROW_TEST_FAILURE("FREE_TLB failed on an exported window");
        }
    }

    // The window is still held by the export, so the pool remains full.
    {
        tenstorrent_allocate_tlb alloc{};
        alloc.in.size = TWO_MEG;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) == 0) {
            ids.push_back(alloc.out.id);
            close(dmabuf_fd);
            free_all();
            THROW_TEST_FAILURE("Exported window returned to the pool after FREE_TLB");
        }
    }

    // Releasing the dma-buf drops the last reference; the window returns to the
    // pool and becomes allocatable again.
    close(dmabuf_fd);

    {
        tenstorrent_allocate_tlb alloc{};
        alloc.in.size = TWO_MEG;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0) {
            free_all();
            THROW_TEST_FAILURE("Window not reusable after closing the dma-buf");
        }
        ids.push_back(alloc.out.id);
    }

    free_all();
}

// Closing the chardev fd that allocated and exported a window (without ever
// calling FREE_TLB) must not tear the window down while the export is live: fd
// release drops the owning reference, but the dma-buf keeps the window
// allocated until it too is closed. Observed from a second fd via pool
// exhaustion, as above.
void VerifyExportSurvivesFdClose(const EnumeratedDevice &dev)
{
    DevFd observer(dev.path);
    int obs = observer.get();

    std::vector<uint32_t> ids;
    auto free_all = [&]() {
        for (uint32_t id : ids) {
            tenstorrent_free_tlb free_tlb{};
            free_tlb.in.id = id;
            ioctl(obs, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb);
        }
    };

    int dmabuf_fd = -1;
    {
        DevFd owner(dev.path);
        int own = owner.get();

        uint32_t id = allocate_window(own);
        if (try_export(own, id, 0, 0, &dmabuf_fd) != 0) {
            free_window(own, id);
            THROW_TEST_FAILURE("Failed to export TLB window as dma-buf");
        }

        // Exhaust the remaining pool from the observer fd, so that after the
        // owner fd closes the only thing keeping the pool full is the export.
        for (;;) {
            tenstorrent_allocate_tlb alloc{};
            alloc.in.size = TWO_MEG;
            if (ioctl(obs, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
                break;
            ids.push_back(alloc.out.id);
        }

        // The owner fd is closed here WITHOUT FREE_TLB. Its release drops the
        // owning reference; the live export must keep the window allocated.
    }

    // The window is still held by the export, so the pool remains full.
    {
        tenstorrent_allocate_tlb alloc{};
        alloc.in.size = TWO_MEG;
        if (ioctl(obs, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) == 0) {
            ids.push_back(alloc.out.id);
            close(dmabuf_fd);
            free_all();
            THROW_TEST_FAILURE("Exported window returned to the pool after owner fd close");
        }
    }

    // Releasing the dma-buf returns the window to the pool.
    close(dmabuf_fd);

    {
        tenstorrent_allocate_tlb alloc{};
        alloc.in.size = TWO_MEG;
        if (ioctl(obs, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0) {
            free_all();
            THROW_TEST_FAILURE("Window not reusable after closing the dma-buf");
        }
        ids.push_back(alloc.out.id);
    }

    free_all();
}

} // namespace

void TestTlbExport(const EnumeratedDevice &dev)
{
    {
        DevFd probe(dev.path);
        if (!export_supported(probe.get())) {
            std::cout << "  EXPORT_TLB_DMABUF unsupported on this kernel; skipping\n";
            return;
        }
    }

    VerifyExportBasic(dev);
    VerifyExportBadArgs(dev);
    VerifyExportRequiresOwnership(dev);
    VerifyExportKeepsTlbAliveAcrossFree(dev);
    VerifyExportSurvivesFdClose(dev);
}
