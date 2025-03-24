// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "interrupt.h"

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/msi.h>

#include "device.h"
#include "enumerate.h"

// TODO fix this
#define NUM_IRQS 32

static irqreturn_t irq_handler(int irq, void *device)
{
	struct tenstorrent_device *tt_dev = device;
	(void)tt_dev;	// to be used later

	return IRQ_HANDLED;
}


bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
	int nvec;
	int irq;

	nvec = pci_alloc_irq_vectors(tt_dev->pdev, NUM_IRQS, NUM_IRQS, PCI_IRQ_MSI);
	if (nvec < 0)
		return false;

	dev_info(&tt_dev->pdev->dev, "Allocated %d IRQ vectors\n", nvec);

	for (int i = 0; i < NUM_IRQS; i++) {
		irq = pci_irq_vector(tt_dev->pdev, i);
		pr_info("IRQ %d: %d\n", i, irq);

		if (irq_get_msi_desc(irq)) {
			struct msi_msg msi;
			get_cached_msi_msg(irq, &msi);
			pr_info("\tdata: %x addr: %x\n", msi.data, msi.address_lo);

		}
	}

	// TODO clean this up
	irq = pci_irq_vector(tt_dev->pdev, 21);
	if (request_irq(pci_irq_vector(tt_dev->pdev, 21), irq_handler,
			IRQF_SHARED, "tenstorrent", tt_dev) != 0) {
		goto out_request_irq_failed;
	}


	tt_dev->interrupt_enabled = true;

	return true;
out_request_irq_failed:
	pci_free_irq_vectors(tt_dev->pdev);
	return false;
}


#if 0
bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
	if (pci_alloc_irq_vectors(tt_dev->pdev, 1, 1, PCI_IRQ_ALL_TYPES) <= 0)
		goto out_pci_alloc_irq_vectors_failed;

	if (request_irq(pci_irq_vector(tt_dev->pdev, 0), irq_handler,
			IRQF_SHARED, TENSTORRENT, tt_dev) != 0)
		goto out_request_irq_failed;

	tt_dev->interrupt_enabled = true;
	return true;

out_request_irq_failed:
	pci_free_irq_vectors(tt_dev->pdev);
out_pci_alloc_irq_vectors_failed:
	return false;
}
#endif

void tenstorrent_disable_interrupts(struct tenstorrent_device *tt_dev)
{
	if (tt_dev->interrupt_enabled) {
		free_irq(pci_irq_vector(tt_dev->pdev, 21), tt_dev); // TODO
		pci_free_irq_vectors(tt_dev->pdev);
		tt_dev->interrupt_enabled = false;
	}
}
