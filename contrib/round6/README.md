# Round 6: KMD io_uring DMA on BH Galaxy (g08blx07)

Reference setups behind the round 6 numbers. Not for merging.

Host: g08blx07, kernel 6.8, EPYC 9354P, 28 x1 + 4 x8 links, MPS as configured (x1 at 128).
All runs: 32 chips from one thread pinned to CPU 4, 16 KiB elements, 64 per command (1 MiB), K=4 in flight.

## Build

    g++ -std=c++17 -O2 -Wall -I<this dir>/shim -I<tt-kmd root> -o bench/kmd_dma_bench bench/kmd_dma_bench.cpp -luring

The shim is liburing 2.1 headers with the 6.8 UAPI `io_uring.h`, because Ubuntu 22.04 ships liburing 2.1.
With the shim, never use `io_uring_prep_*`: they write past the SQE. The bench fills SQEs by hand.
On liburing 2.2 or newer, drop `-I<shim>`.

## Stable shape: 181.7 GB/s

- Driver: `ajovanovic/bh_kmd_dma_chain` (d7c6e6c), unmodified.
- The bench raises QOS.PF_DEPTH to 7 through BAR2 (`--pf-depth 7`), so the driver's ELEM_PF cap of 7 applies.
  Without it, PF_DEPTH stays at its reset value 1 and the result is 158 GB/s.

      kmd_dma_bench --chips 0-31 --elem 16384 --descs 64 --inflight 4 --pf-depth 7 --secs 3 --warmup 1

`run_pf.sh` sweeps PF_DEPTH. It used DMA_ELEMENT_PREFETCH 15 in the driver, needed only above 7.
At 8 KiB, PF above 1 is slower (106 vs 83 GB/s at K=4).

## Experimental shape (G4): 194.7 GB/s

- Driver: this branch (4993728 = d7c6e6c + the experiment commit).
- Module parameters, set by `run_submit.sh G4 2000 1 1 20 --defer`:
  `dma_ring=1 dma_watermark=1 dma_guard_ns=2000 dma_guard_mode=1 dma_fast_submit=1 dma_keep_ms=20`.
- The bench runs with `--pf-depth 7 --defer`.
- Guard mode 1 skipped 2 elements in ~147 M; mode 2 skipped 2 in ~332 M (45 min stress).
  The torn-descriptor guard is not clean yet.

## Restore after a run

Load the stock driver, restore MPS on the x1 links, then `tt-smi -r`.
After unloading this driver, MSI shows `Count=1/2` until the reset.
