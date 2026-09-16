// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "pcie.h"

#include <linux/bitfield.h>
#include <linux/delay.h>

#include "module.h"
#include "device.h"
#include "enumerate.h"
#include "wormhole.h"

#define FW_MSG_PCIE_RETRAIN 0xB6
#define INTERFACE_TIMER_CONTROL_OFF 0x930
#define INTERFACE_TIMER_TARGET_OFF 0x934

#define INTERFACE_TIMER_TARGET 0x1
#define INTERFACE_TIMER_EN 0x1
#define INTERFACE_FORCE_PENDING 0x10

// Ceiling on the exponential backoff in wait_reset_marker_clear(). This is also
// the worst-case latency between the link coming back and us noticing.
#define RESET_MARKER_POLL_MAX_MS 512

static bool poll_pcie_link_up(struct pci_dev *pdev, u32 timeout_ms) {
	u16 tt_vendor_id;
	ktime_t end_time = ktime_add_ms(ktime_get(), timeout_ms);

	pci_read_config_word(pdev, PCI_VENDOR_ID, &tt_vendor_id);
	while (tt_vendor_id != PCI_VENDOR_ID_TENSTORRENT) {
		if (ktime_after(ktime_get(), end_time)) {
			dev_dbg(&pdev->dev, "device timeout during link up\n");
			return false;
		}

		pci_read_config_word(pdev, PCI_VENDOR_ID, &tt_vendor_id);
		msleep(100);
	}

	dev_dbg(&pdev->dev, "device link up successfully\n");
	return true;
}

bool safe_pci_restore_state(struct pci_dev *pdev) {
	u16 vendor_id;

	if (!pdev->state_saved)
		return false;

	// Start with a test read. pci_restore_state calls pci_find_next_ext_capability which has
	// a bounded loop that is still long enough to trigger a soft lockup warning if hardware
	// is extremely misbehaving.
	if (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id) != PCIBIOS_SUCCESSFUL
	    || vendor_id != PCI_VENDOR_ID_TENSTORRENT)
		return false;

	pci_restore_state(pdev);
	pci_save_state(pdev);
	return true;
}

bool pcie_hot_reset_and_restore_state(struct pci_dev *pdev) {
	struct pci_dev *bridge_dev = pci_upstream_bridge(pdev);
	u16 bridge_ctrl;
	bool result;
	bool saved_ignore_hotplug = pdev->ignore_hotplug;

	if (!bridge_dev)
		return false;

	pci_ignore_hotplug(pdev);

	// reset link - like pci_reset_secondary_bus, but we don't want the full 1s delay.
	pci_read_config_word(bridge_dev, PCI_BRIDGE_CONTROL, &bridge_ctrl);
	pci_write_config_word(bridge_dev, PCI_BRIDGE_CONTROL, bridge_ctrl | PCI_BRIDGE_CTL_BUS_RESET);

	msleep(2);
	pci_write_config_word(bridge_dev, PCI_BRIDGE_CONTROL, bridge_ctrl);
	msleep(500);

	result = poll_pcie_link_up(pdev, 10000) && safe_pci_restore_state(pdev);

	if (!saved_ignore_hotplug) {
		// There is no pci_unignore_hotplug(), but the flag is just a struct
		// member we can clear directly.
		pdev->ignore_hotplug = 0;
		bridge_dev->ignore_hotplug = 0;
	}

	return result;
}

bool wormhole_complete_pcie_init(struct tenstorrent_device *tt_dev, u8 __iomem* reset_unit_regs) {
	struct pci_dev *pdev = tt_dev->pdev;
	struct pci_dev *bridge_dev = pci_upstream_bridge(pdev);

	unsigned int i;

	if (!bridge_dev || reset_limit == 0)
		return true;

	for (i = 0; i < reset_limit; i++) {
		u16 target_link_speed;
		u16 subsys_vendor_id;
		u16 exit_code;
		bool last_retry = (i == reset_limit - 1);

		pcie_capability_read_word(bridge_dev, PCI_EXP_LNKCTL2, &target_link_speed);
		target_link_speed &= PCI_EXP_LNKCTL2_TLS;

		pci_read_config_word(bridge_dev, PCI_SUBSYSTEM_VENDOR_ID, &subsys_vendor_id);

		if (!wormhole_send_arc_fw_message_with_args(pdev, reset_unit_regs, FW_MSG_PCIE_RETRAIN,
			target_link_speed | (last_retry << 15), subsys_vendor_id, 200000, &exit_code))
			return false;

		if (exit_code == 0) {
			dev_dbg(&pdev->dev, "pcie init passed after %u iterations\n", i);
			return true;
		} else {
			dev_dbg(&pdev->dev, "pcie init failed on iteration %u\n", i);
			if (last_retry)
				return false;
		}

		pci_save_state(pdev);
		if (!pcie_hot_reset_and_restore_state(pdev))
			return false;
	}

	return false;
}

bool pcie_timer_interrupt(struct pci_dev *pdev)
{
	pci_write_config_dword(pdev, INTERFACE_TIMER_TARGET_OFF, INTERFACE_TIMER_TARGET);
	pci_write_config_dword(pdev, INTERFACE_TIMER_CONTROL_OFF, INTERFACE_TIMER_EN | INTERFACE_FORCE_PENDING);
	return true;
}

bool set_reset_marker(struct pci_dev *pdev)
{
	u16 pci_command;

	// pci_command_parity is used as reset marker. Set to 1, check if cleared to 0 after reset
	pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
	pci_write_config_word(pdev, PCI_COMMAND, pci_command | PCI_COMMAND_PARITY);

	return true;
}

// The marker is clear once the device answers config reads with its own vendor
// ID and PCI_COMMAND_PARITY is zero. Checking the vendor ID guards against root
// ports that return zeros rather than all-ones while the link is down, which
// would otherwise look like a cleared marker.
static bool reset_marker_is_clear(struct pci_dev *pdev)
{
	u16 vendor_id;
	u16 pci_command;

	if (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id) != PCIBIOS_SUCCESSFUL
	    || vendor_id != PCI_VENDOR_ID_TENSTORRENT)
		return false;

	if (pci_read_config_word(pdev, PCI_COMMAND, &pci_command) != PCIBIOS_SUCCESSFUL)
		return false;

	return (pci_command & PCI_COMMAND_PARITY) == 0;
}

// Wait for the marker set by set_reset_marker() to clear, which happens when the
// device's config space is reset and the link comes back up. The poll interval
// starts at 1 ms and doubles on each miss up to RESET_MARKER_POLL_MAX_MS, so a
// chip that comes back quickly is seen quickly and one that takes seconds (a
// Galaxy tray after an IPMI reset) is not hammered with config reads meanwhile.
//
// Returns 0 when the marker is clear, -ETIMEDOUT after timeout_ms, or -EINTR
// if the caller was signalled. The caller holds reset_rwsem exclusive, so
// open()/release() on this device block for the duration of the wait.
int wait_reset_marker_clear(struct pci_dev *pdev, u32 timeout_ms)
{
	ktime_t start = ktime_get();
	ktime_t end_time = ktime_add_ms(start, timeout_ms);
	u32 interval_ms = 1;
	unsigned int polls = 0;

	for (;;) {
		polls++;
		if (reset_marker_is_clear(pdev)) {
			dev_dbg(&pdev->dev, "reset marker cleared after %lld ms, %u polls\n",
				ktime_ms_delta(ktime_get(), start), polls);
			return 0;
		}

		if (ktime_after(ktime_get(), end_time)) {
			dev_dbg(&pdev->dev, "reset marker still set after %u ms, %u polls\n",
				timeout_ms, polls);
			return -ETIMEDOUT;
		}

		if (msleep_interruptible(interval_ms) != 0)
			return -EINTR;

		interval_ms = min(interval_ms * 2, (u32)RESET_MARKER_POLL_MAX_MS);
	}
}
