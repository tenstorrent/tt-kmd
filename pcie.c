// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "pcie.h"

#include <linux/delay.h>

#include "module.h"
#include "device.h"
#include "enumerate.h"
#include "grayskull.h"

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

	if (!bridge_dev)
		return false;

	// reset link - like pci_reset_secondary_bus, but we don't want the full 1s delay.
	pci_read_config_word(bridge_dev, PCI_BRIDGE_CONTROL, &bridge_ctrl);
	pci_write_config_word(bridge_dev, PCI_BRIDGE_CONTROL, bridge_ctrl | PCI_BRIDGE_CTL_BUS_RESET);

	msleep(2);
	pci_write_config_word(bridge_dev, PCI_BRIDGE_CONTROL, bridge_ctrl);
	msleep(500);

	if (!poll_pcie_link_up(pdev, 10000))
		return false;

	if (!safe_pci_restore_state(pdev))
		return false;

	return true;
}


bool complete_pcie_init(struct tenstorrent_device *tt_dev, u8 __iomem* reset_unit_regs) {
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

		if (!grayskull_send_arc_fw_message_with_args(reset_unit_regs, FW_MSG_PCIE_RETRAIN,
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
