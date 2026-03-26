# I/O v2: Kernel-Mediated NOC I/O Interface

## What it is

A single ioctl (`TENSTORRENT_IOCTL_NOC_IO`) for reading and writing
device NOC addresses.  The kernel manages TLB configuration, caching
mode (UC vs WC), and ordering internally.  Userspace provides tile
coordinates (x, y), a NOC address, and a data buffer pointer.

Supports 32-bit register access and bulk block transfers.  Also
wired as `uring_cmd` for io_uring submission.

## Implementation (done)

### Ioctl interface

`struct tenstorrent_noc_io` uses the argsz extensibility pattern
(see `docs/ioctl.md`).  Two flag bits select the operation:

- `TENSTORRENT_NOC_IO_WRITE` — read vs write
- `TENSTORRENT_NOC_IO_BLOCK` — 32-bit vs bulk

All data flows through `data_ptr` / `data_len`.  For 32-bit ops
the caller passes a pointer to a `u32`.  For block ops the caller
passes a buffer of arbitrary size.  NOC 0 is hardcoded; NOC
selection can be added via a flag bit later.

### Kernel side

- `chardev.c`: `do_noc_io()` handles validation and dispatch for
  both ioctl and uring_cmd paths.  Block transfers pin user pages
  (`pin_user_pages_fast`), pass the `struct page **` array to the
  chip-specific vtable, and unpin on completion.  Data moves
  directly between user pages and MMIO via `memcpy_toio` /
  `memcpy_fromio` — single copy, no bounce buffer.

- `device.h`: vtable entries `noc_read32`, `noc_write32`,
  `noc_block_read`, `noc_block_write`.

- `blackhole.c`: UC kernel TLB for reads and 32-bit ops.  WC
  kernel TLB (`ioremap_wc`) for block writes.  Both share
  `kernel_tlb_mutex`.  Block transfer implementations iterate by
  TLB window (2 MiB), then by page within each window.

- `wormhole.c`: same structure, 16 MiB kernel TLB.  WC TLB not
  yet added (block writes use UC).

### io_uring

`chardev_fops` has `.uring_cmd` (gated on `CONFIG_IO_URING`).  The
`tenstorrent_noc_io` struct (40 bytes) sits in the SQE's inline
`cmd` field; requires `IORING_SETUP_SQE128`.  The handler shares
`do_noc_io()` with the ioctl path.

### Test tool (`tools/io_test.c`)

Correctness tests run sequentially, each gated on the previous:

1. NOC sanity — read node ID from every Tensix tile, verify (x, y).
2. L1 write/readback (32-bit) — 256 words to a single tile.
3. L1 write/readback (block) — 64 KiB.
4. GDDR write/readback (block) — 1 MiB.
5. Loopback DMA — pin host buffer with `NOC_DMA`, write pattern
   through PCIe tile, verify host memory.

### Benchmark tools

`tools/io_bench.c` — measures direct MMIO baseline (userspace
volatile loads/stores through mmap'd TLB) alongside ioctl block
write/read throughput at various sizes.

`tools/io_uring_bench.c` — compares ioctl, io_uring submit+wait,
and io_uring SQPOLL for 32-bit latency.

## Performance (Blackhole, single thread)

### 32-bit latency

|                  | ns/op |
|------------------|------:|
| Direct MMIO read |   853 |
| ioctl read       |  1496 |
| Direct MMIO write|   317 |
| ioctl write      |   836 |

Per-operation kernel overhead is ~500–640 ns (syscall entry/exit,
ioctl dispatch, argsz negotiation, rwsem, mutex, TLB configure,
get_user/put_user).

### Block write throughput

|                          | L1 (MB/s) | DRAM (MB/s) |
|--------------------------|----------:|------------:|
| Direct MMIO (WC, 1 MiB) |     5,502 |       8,006 |
| ioctl block write (1 MiB)|    5,533 |       7,984 |

Zero measurable overhead at bulk sizes.  The ioctl per-operation
cost is amortized across the transfer.

### Block read throughput

4.7 MB/s (UC MMIO reads, ~850 ns per 4-byte read).  Improving
this requires DMA, which is a separate project.

### io_uring

Single-operation io_uring does not beat ioctl — the ring machinery
(SQE processing, CQE posting) costs more than the syscall it
replaces for one-at-a-time operations.  The win requires batched
submission with concurrent kernel execution.

## Next steps

### TLB pool with LRU and per-TLB locking

The current implementation uses one UC and one WC kernel TLB,
serialized on a single mutex.  This limits throughput to one
operation at a time and reconfigures the TLB on every access.

A pool of TLBs with LRU eviction would:

- Amortize TLB configuration cost for repeated accesses to the
  same tile (common in telemetry, NOC sanity, sequential L1
  writes).
- Enable per-TLB locking, eliminating contention between
  concurrent readers and writers and between threads.
- Separate UC and WC pools (caching attribute is set at ioremap
  time).

The TLB config caching optimization from the ethdump pattern
(only write TLB registers when values actually change) falls out
naturally from LRU — each pool entry tracks its current mapping.

Design questions: pool size (too many starves the userspace mmap
path of TLB slots), eviction policy under contention, per-fd
partitioning.

### Batched io_uring with concurrent execution

With a TLB pool and per-TLB locking, batched io_uring submission
becomes meaningful: the kernel can process multiple SQEs in
parallel across different pool TLBs.  This is the path to
matching or beating direct MMIO for 32-bit register access
patterns (e.g., reading 100 tile registers in one batch).

Requires `IORING_SETUP_SQE128` and possibly newer liburing (the
distro liburing 2.1 predates uring_cmd; the current benchmark
uses raw syscalls).

### Wormhole WC TLB

Add a WC kernel TLB to `wormhole.c` (same pattern as blackhole).
Block writes on WH currently use UC through the 16 MiB kernel TLB.

### Block read performance

UC MMIO reads are fundamentally slow (~4.7 MB/s).  Options:

- DMA engine (available on WH, broken on BH for device→host).
- Investigate whether relaxed-ordering reads improve throughput.
- Accept the limitation for BH and focus on DMA for future chips.
