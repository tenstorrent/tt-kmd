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

static bool poll_pcie_link_up(struct pci_dev *pdev, u32 timeout_ms) {
	u16 tt_vendor_id;
	ktime_t end_time = ktime_add_ms(ktime_get(), timeout_ms);

	pci_read_config_word(pdev, PCI_VENDOR_ID, &tt_vendor_id);
	while (tt_vendor_id != PCI_VENDOR_ID_TENSTORRENT) {
		if (ktime_after(ktime_get(), end_time)) {
			pr_debug("device timeout during link up.\n");
			return false;
		}

		pci_read_config_word(pdev, PCI_VENDOR_ID, &tt_vendor_id);
		msleep(100);
	}

	pr_debug("device link up successfully.\n");
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

	// Re-enable hotplug events. There is no pci_unignore_hotplug(), but the
	// flag is just a struct member we can clear directly.
	pdev->ignore_hotplug = 0;
	bridge_dev->ignore_hotplug = 0;

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

		if (!wormhole_send_arc_fw_message_with_args(reset_unit_regs, FW_MSG_PCIE_RETRAIN,
			target_link_speed | (last_retry << 15), subsys_vendor_id, 200000, &exit_code))
			return false;

		if (exit_code == 0) {
			pr_debug("pcie init passed after %u iterations.\n", i);
			return true;
		} else {
			pr_debug("pcie init failed on iteration %u.\n", i);
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

bool is_reset_marker_zero(struct pci_dev *pdev)
{
	u16 pci_command;

	// Read the reset marker
	pci_read_config_word(pdev, PCI_COMMAND, &pci_command);
	return (pci_command & PCI_COMMAND_PARITY) == 0;
}

// Wait for LBMS (Link Bandwidth Management Status) to be set, indicating that
// link retraining has completed. Returns true if LBMS was set, false on timeout.
static bool pcie_wait_for_lbms(struct pci_dev *dev, unsigned int timeout_ms)
{
	ktime_t end_time = ktime_add_ms(ktime_get(), timeout_ms);
	u16 lnksta;

	do {
		pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
		if (lnksta & PCI_EXP_LNKSTA_LBMS)
			return true;
		msleep(1);
	} while (ktime_before(ktime_get(), end_time));

	return false;
}

// Clear LBMS on a device. LBMS is RW1C (write 1 to clear).
static void pcie_clear_lbms(struct pci_dev *dev)
{
	pcie_capability_write_word(dev, PCI_EXP_LNKSTA, PCI_EXP_LNKSTA_LBMS);
}

// Workaround for Linux kernels 6.5-6.12 where pcie_failed_link_retrain()
// forces the link to Gen1 (2.5GT/s) during hot-plug enumeration.
//
// The kernel quirk (commit a89c82249c37) detects LBMS=1 with DLLLA=0,
// interprets this as a broken device, forces the target link speed to Gen1, and
// retrains the link. The restriction is only lifted for whitelisted devices.
//
// During hotplug enumeration on Blackhole Galaxy, there is a ~1 second window
// where the card is detected as present, link training occurs, but DLLLA is not
// yet set when the kernel checks. Linux's pcie_wait_for_link_status() polls for
// DLLLA for up to 1000ms. When it times out, pcie_failed_link_retrain() is
// called and sees LBMS=1, DLLLA=0, triggering the quirk.
//
// Fixed upstream in kernel 6.13 by commit 665745f27487 ("PCI/bwctrl: Re-add
// BW notification portdrv as PCIe BW controller").
//
// This function retrains the link to full speed by setting Target Link Speed
// to the minimum of device and bridge capabilities and triggering a retrain.
// Sometimes multiple retrains are needed as the link steps up.
#define PCIE_LINK_RETRAIN_TIMEOUT_MS 1000
#define PCIE_LINK_RETRAIN_MAX_ATTEMPTS 5
#define PCIE_DEVICE_ACCESSIBLE_TIMEOUT_MS 500

void pcie_retrain_link_to_max_speed(struct pci_dev *pdev)
{
	struct pci_dev *bridge = pci_upstream_bridge(pdev);
	u32 lnkcap, bridge_lnkcap;
	u16 lnksta, lnkctl2, vendor_id;
	u8 dev_max_speed, bridge_max_speed, target_speed, current_speed;
	ktime_t end_time;
	int retrain_count;

	if (!bridge)
		return;

	pcie_capability_read_dword(pdev, PCI_EXP_LNKCAP, &lnkcap);
	dev_max_speed = FIELD_GET(PCI_EXP_LNKCAP_SLS, lnkcap);

	if (lnkcap == ~0u) {
		dev_warn(&pdev->dev, "Device not accessible, skipping link retrain\n");
		return;
	}

	pcie_capability_read_dword(bridge, PCI_EXP_LNKCAP, &bridge_lnkcap);
	bridge_max_speed = FIELD_GET(PCI_EXP_LNKCAP_SLS, bridge_lnkcap);

	target_speed = min(dev_max_speed, bridge_max_speed);

	// Read current link speed from bridge (always accessible, even if the
	// device is momentarily unreachable during link training).
	pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &lnksta);
	current_speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta);

	if (current_speed >= target_speed)
		return;

	dev_info(&pdev->dev, "Link at Gen%u, retraining to Gen%u\n", current_speed, target_speed);

	pcie_capability_read_word(bridge, PCI_EXP_LNKCTL2, &lnkctl2);
	lnkctl2 = (lnkctl2 & ~PCI_EXP_LNKCTL2_TLS) | FIELD_PREP(PCI_EXP_LNKCTL2_TLS, target_speed);
	pcie_capability_write_word(bridge, PCI_EXP_LNKCTL2, lnkctl2);

	for (retrain_count = 0; retrain_count < PCIE_LINK_RETRAIN_MAX_ATTEMPTS; retrain_count++) {
		pcie_clear_lbms(bridge);
		pcie_capability_set_word(bridge, PCI_EXP_LNKCTL, PCI_EXP_LNKCTL_RL);

		if (!pcie_wait_for_lbms(bridge, PCIE_LINK_RETRAIN_TIMEOUT_MS)) {
			dev_warn(&pdev->dev, "Timeout waiting for link retrain to complete\n");
			break;
		}

		pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &lnksta);
		current_speed = FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta);

		if (current_speed >= target_speed)
			break;

		dev_dbg(&pdev->dev, "Retrain %d: link at Gen%u, target Gen%u\n",
			retrain_count + 1, current_speed, target_speed);
	}

	// Clear LBMS so the kernel's pcie_failed_link_retrain() won't
	// misinterpret it as a hardware failure.
	pcie_clear_lbms(bridge);
	pcie_clear_lbms(pdev);

	// Verify the device is accessible before returning.
	end_time = ktime_add_ms(ktime_get(), PCIE_DEVICE_ACCESSIBLE_TIMEOUT_MS);
	pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id);
	while (vendor_id != PCI_VENDOR_ID_TENSTORRENT) {
		if (ktime_after(ktime_get(), end_time)) {
			dev_warn(&pdev->dev,
				 "Device not accessible %ums after link retrain\n",
				 PCIE_DEVICE_ACCESSIBLE_TIMEOUT_MS);
			break;
		}
		msleep(1);
		pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id);
	}

	if (current_speed >= target_speed)
		dev_info(&pdev->dev, "Link retrained to Gen%u after %d attempt(s)\n", current_speed, retrain_count + 1);
	else
		dev_warn(&pdev->dev, "Link retrain incomplete: Gen%u (target Gen%u) after %d attempt(s)\n",
			 current_speed, target_speed, retrain_count + 1);
}
