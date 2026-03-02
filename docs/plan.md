# Galaxy PCIe Link Recovery Experiments

## Goal

Systematically reproduce the degraded-link state (Gen1 stuck) on Galaxy x1
chips, then test recovery strategies from userspace. Either find a reliable
software recovery or confirm the hardware is the limiting factor.

## Prerequisites

- Machine: bh-glx6u-28 (32x Blackhole, 4 trays × 8 chips)
- Kernel: 6.8.0-94-generic (has the pcie_failed_link_retrain quirk)
- Driver: v2.7.99-no-retrain (retrain workaround `#if 0`'d out, hotplug active)
- Tools: ipmitool, gcc, workspace/ tools built during this experiment

## Stage 0: Decoder Ring & Mapping Verification

**Objective:** Build the BDF ↔ (UBB, ASIC) ↔ IPMI parameter mapping.

Built `workspace/galaxy-map.c`. Findings:

- Bus layout per tray: low nibble 1-8 (not 0-7 as vendor docs suggested).
  ASIC N = bus low nibble N, IPMI dev_num = 1 << (N-1).
- x8 link is ASIC 6 (bus low nibble 6) in all 4 trays. 28 x1, 4 x8.
- `max_link_width` in sysfs reports x8 for all 32 devices (endpoint
  capability) even the 28 physically wired as x1. Must use
  `current_link_width` to distinguish.
- UBB-to-bus: UBB1=0x0_, UBB2=0x4_, UBB3=0xC_, UBB4=0x8_.
- **Per-chip IPMI reset does not work.** BMC ignores the UBB/ASIC bitmask and
  does a global reset regardless. Confirmed by attempting single-chip reset
  and observing all 32 bridges fire Link Down. tt-umd also always resets all
  32 (`ubb_num=0xF, dev_num=0xFF`).

## Stage 1: Build Degraded Pool

**Objective:** Get a pool of x1 chips stuck at Gen1.

Since per-chip IPMI is unavailable, used repeated global resets. Each round
the kernel quirk catches ~85% of x1 chips. Automated with
`workspace/degrade.sh`.

Typical results (retrain workaround disabled): 20-25 x1 chips at Gen1 after
1-2 IPMI reset rounds. The specific set differs each round.

## Stage 2: Baseline Characterization

**Objective:** Characterize the degraded state before attempting recovery.

Built `workspace/baseline.c` -- reads PCIe registers from all bridges and
runs quick NOC (140 Tensix cores) + DMA (4KB loopback) on degraded chips.

All 24 degraded chips showed identical register state:
- **TLS = 1**: the kernel quirk set Target Link Speed to Gen1 on the bridge.
  This is the direct cause -- the bridge is *requesting* Gen1.
- DLLLA = 1, LBMS = 1 (link up, bandwidth changed).
- EqComplete = 0, Phase 1/2/3 = 0 (expected -- equalization is Gen3+).
- PerformEqu = 0.

Healthy chips: TLS=5, EqComplete=1, Phase 1/2/3 all pass.

**All 24 degraded chips pass NOC + DMA at Gen1.** Silicon is fine. The
problem is purely that the kernel wrote TLS=1 to the bridge.

## Stage 3: Recovery Experiments

Built `workspace/recover.c` -- applies recovery methods from userspace config
space writes to the upstream bridge. Supports `--verify` flag for NOC + DMA
functional checks after recovery.

### Method A: TLS + Retrain (no settle delay)

Set TLS=5 on bridge, trigger retrain via RL bit, poll LBMS, loop up to 10x.

| Round | Pool | Gen1→Gen5 | Gen1→Gen4 | Stuck Gen1 |
|-------|------|-----------|-----------|------------|
| 1     | 24   | 0 (0%)    | 24        | 0          |
| 2     | 25   | 0 (0%)    | 25        | 0          |

**0% single-pass success.** Every chip reaches Gen4 but cannot make the
Gen4→Gen5 jump in a tight retrain loop. Running the tool a second time
(from Gen4) recovers 100% in a single attempt. The equalization hardware
needs settling time between reaching Gen4 and attempting Gen5.

### Method B: PerformEqu + Retrain

Set PerformEqu bit in LnkCtl3 before the retrain loop to force fresh Phase
0-3 equalization.

| Round | Pool | Gen1→Gen5 | Gen1→Gen4 | Stuck Gen1 |
|-------|------|-----------|-----------|------------|
| 1     | 21   | 0 (0%)    | 19        | 2          |

**No improvement over A. Slightly worse.** Same Gen4 wall. Two chips
(44:00.0, 47:00.0) failed to even reach Gen4, with 47:00.0 becoming
inaccessible. PerformEqu discards cached coefficients that were working,
causing regressions on marginal links.

**Conclusion: The Gen4→Gen5 barrier is not stale equalization.** The
uncommitted driver code adding PerformEqu should be dropped.

### Method C: TLS + Retrain + Settle Delay

Same as Method A but with a pause between retrain attempts.

| Config              | Pool | Gen1→Gen5  | Stuck Gen1 |
|---------------------|------|------------|------------|
| 200ms settle, 15att | 24   | 22 (92%)   | 2          |
| 500ms settle, 20att | 22   | 21 (95%)   | 1 (47)     |
| 500ms settle, 20att | 25   | 24 (96%)   | 1 (42)     |

The settle delay eliminated the Gen4 wall. 92-96% single-pass recovery.
The 1-2 stragglers per round are marginal chips that self-recover to Gen5
within seconds (likely slow equalization that completes after our tool's
measurement window).

All recovered chips pass NOC + DMA at Gen5.

### Methods D/E: SBR and IPMI Assert/Deassert

Skipped. SBR during probe is risky (save/restore PCI state, suppress
hotplug). IPMI from the driver is a non-starter. Method C is sufficient.

## Stage 4: Driver Integration

Applied findings to the driver's `pcie_retrain_link_to_max_speed()` (on top
of commit `b6d77cc`). Changes:

1. **LBMS timeout 1s → 2s**: catches chips with slow training events.
2. **Max attempts 5 → 8**: room for speed-stepping + settle.
3. **`msleep(500)` between retrain attempts**: the key fix. Unconditional
   sleep after each failed attempt gives equalization hardware time to
   settle. Eliminates the Gen4→Gen5 wall without complex state tracking.
4. Moved all-ones guard before FIELD_GET (latent bug fix from `b6d77cc`).

The uncommitted PerformEqu rewrite was dropped -- our data shows it adds no
value and can destabilize marginal links.

### Driver test results

Loaded the updated driver and ran `workspace/degrade.sh` stress loop (IPMI
global reset, wait for re-enumeration, check for Gen1 chips). The driver's
retrain workaround now recovers all chips during probe:

- **10 rounds, 0 degraded chips.** The kernel quirk fires on ~20 chips per
  round (visible in dmesg), and the driver recovers all of them to Gen5.
- Most chips retrain in 5 attempts (~2.5s with settle delays).
- Occasional chips need 7 attempts (~4s).
- Rare chips (~1/20) report "retrain incomplete" at Gen4 but self-recover
  in the background before any functional impact.

### Remaining work

- [ ] Stress test the driver build (100+ rounds)
- [ ] Clean up module.h version string
- [ ] Prepare commit on top of `b6d77cc` with proper commit message
- [ ] Separate question: `pci_ignore_hotplug()` at probe for Galaxy (the
  root-cause fix that makes the retrain workaround unnecessary in most cases)

## Tools

All experiment tools in `workspace/`:

| Tool | Purpose |
|------|---------|
| `galaxy-map.c` | BDF ↔ UBB/ASIC ↔ IPMI decoder ring |
| `baseline.c` | PCIe register dump + NOC/DMA functional check |
| `recover.c` | Userspace recovery methods A/B/C with --verify |
| `degrade.sh` | Automate IPMI reset loop to build degraded pool |
