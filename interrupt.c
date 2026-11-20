// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "interrupt.h"

#include <linux/pci.h>
#include <linux/types.h>
#include <linux/interrupt.h>

#include "device.h"
#include "enumerate.h"
#include "module.h"

static irqreturn_t irq_handler(int irq, void *device)
{
	struct tenstorrent_device *tt_dev = device;

	// Multi-message MSI vectors get consecutive IRQ numbers. irq_base is
	// cached because pci_irq_vector() may sleep.
	if (tt_dev->dev_class->interrupt)
		tt_dev->dev_class->interrupt(tt_dev, irq - tt_dev->irq_base);

	return IRQ_HANDLED;
}

static void free_irqs(struct tenstorrent_device *tt_dev, unsigned int count)
{
	while (count--)
		free_irq(pci_irq_vector(tt_dev->pdev, count), tt_dev);
}

static int alloc_irq_vectors(struct tenstorrent_device *tt_dev)
{
	struct pci_dev *pdev = tt_dev->pdev;
	unsigned int max_vectors = max(tt_dev->dev_class->max_msi_vectors, 1u);
	int vectors;

	if (max_msi_vectors)
		max_vectors = min(max_vectors, max_msi_vectors);

	// Several vectors are only possible with MSI. The PCI core grants fewer
	// than asked when the device advertises fewer (old firmware) or the
	// platform cannot do multi-message MSI (x86 without interrupt remapping).
	if (max_vectors > 1) {
		vectors = pci_alloc_irq_vectors(pdev, 1, max_vectors, PCI_IRQ_MSI);
		if (vectors > 0) {
			if ((unsigned int)vectors < max_vectors)
				dev_info(&pdev->dev, "Allocated %d of %u MSI vectors\n", vectors, max_vectors);
			return vectors;
		}
	}

	return pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
}

bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
	int vectors;
	int i;

	vectors = alloc_irq_vectors(tt_dev);
	if (vectors <= 0)
		goto out_pci_alloc_irq_vectors_failed;

	// Set before any handler can run: the handler reads both.
	tt_dev->irq_base = pci_irq_vector(tt_dev->pdev, 0);
	tt_dev->interrupt_vectors = vectors;

	for (i = 0; i < vectors; i++) {
		if (request_irq(pci_irq_vector(tt_dev->pdev, i), irq_handler,
				IRQF_SHARED, TENSTORRENT, tt_dev) != 0)
			goto out_request_irq_failed;
	}

	tt_dev->interrupt_enabled = true;
	return true;

out_request_irq_failed:
	free_irqs(tt_dev, i);
	pci_free_irq_vectors(tt_dev->pdev);
	tt_dev->interrupt_vectors = 0;
out_pci_alloc_irq_vectors_failed:
	return false;
}

void tenstorrent_disable_interrupts(struct tenstorrent_device *tt_dev)
{
	if (tt_dev->interrupt_enabled) {
		free_irqs(tt_dev, tt_dev->interrupt_vectors);
		pci_free_irq_vectors(tt_dev->pdev);
		tt_dev->interrupt_vectors = 0;
		tt_dev->interrupt_enabled = false;
	}
}
