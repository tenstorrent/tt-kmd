// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "interrupt.h"

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/interrupt.h>

#include "device.h"
#include "enumerate.h"

static irqreturn_t irq_handler(int irq, void *device)
{
	struct tenstorrent_device *tt_dev = device;

	// Multi-message MSI vectors get consecutive IRQ numbers.
	if (tt_dev->dev_class->interrupt)
		tt_dev->dev_class->interrupt(tt_dev, irq - pci_irq_vector(tt_dev->pdev, 0));

	return IRQ_HANDLED;
}

static void free_irqs(struct tenstorrent_device *tt_dev, unsigned int count)
{
	while (count--)
		free_irq(pci_irq_vector(tt_dev->pdev, count), tt_dev);
}

bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
	unsigned int max_vectors = 1;
	int vectors;
	int i;

	if (tt_dev->dev_class->prepare_msi_vectors)
		max_vectors = tt_dev->dev_class->prepare_msi_vectors(tt_dev);

	// The PCI core falls back to fewer vectors (one MSI without interrupt
	// remapping on x86) or to INTx.
	vectors = pci_alloc_irq_vectors(tt_dev->pdev, 1, max_vectors, PCI_IRQ_ALL_TYPES);
	if (vectors <= 0)
		goto out_pci_alloc_irq_vectors_failed;

	if (vectors < max_vectors)
		dev_dbg(&tt_dev->pdev->dev, "Allocated %d of %u interrupt vectors\n", vectors, max_vectors);

	for (i = 0; i < vectors; i++) {
		if (request_irq(pci_irq_vector(tt_dev->pdev, i), irq_handler,
				IRQF_SHARED, TENSTORRENT, tt_dev) != 0)
			goto out_request_irq_failed;
	}

	tt_dev->interrupt_vectors = vectors;
	tt_dev->interrupt_enabled = true;
	return true;

out_request_irq_failed:
	free_irqs(tt_dev, i);
	pci_free_irq_vectors(tt_dev->pdev);
out_pci_alloc_irq_vectors_failed:
	return false;
}

void tenstorrent_disable_interrupts(struct tenstorrent_device *tt_dev)
{
	if (tt_dev->interrupt_enabled) {
		free_irqs(tt_dev, tt_dev->interrupt_vectors);
		pci_free_irq_vectors(tt_dev->pdev);
		tt_dev->interrupt_enabled = false;
	}
}
