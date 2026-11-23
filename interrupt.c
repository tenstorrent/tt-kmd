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

	// Multi-message MSI vectors get consecutive IRQs. irq_base is cached
	// because pci_irq_vector() may sleep.
	if (tt_dev->dev_class->interrupt)
		tt_dev->dev_class->interrupt(tt_dev, irq - tt_dev->irq_base);

	return IRQ_HANDLED;
}

static void free_irqs(struct tenstorrent_device *tt_dev, unsigned int count)
{
	while (count--)
		free_irq(pci_irq_vector(tt_dev->pdev, count), tt_dev);
}

static bool enable_interrupts(struct tenstorrent_device *tt_dev, unsigned int max_vectors,
			      unsigned int irq_types)
{
	int vectors;
	int i;

	vectors = pci_alloc_irq_vectors(tt_dev->pdev, 1, max_vectors, irq_types);
	if (vectors <= 0)
		return false;

	// Set before any handler can run: the handler reads both.
	tt_dev->irq_base = pci_irq_vector(tt_dev->pdev, 0);
	tt_dev->interrupt_vectors = vectors;

	for (i = 0; i < vectors; i++) {
		if (request_irq(pci_irq_vector(tt_dev->pdev, i), irq_handler,
				IRQF_SHARED, TENSTORRENT, tt_dev) != 0) {
			free_irqs(tt_dev, i);
			pci_free_irq_vectors(tt_dev->pdev);
			tt_dev->interrupt_vectors = 0;
			return false;
		}
	}

	return true;
}

bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
	unsigned int max_vectors = max(tt_dev->dev_class->max_msi_vectors, 1u);

	// Only multi-message MSI guarantees the consecutive IRQs the handler
	// relies on. If it fails, fall back to one vector of any type, so a
	// failure on an extra vector does not cost vector 0.
	tt_dev->interrupt_enabled =
		(max_vectors > 1 && enable_interrupts(tt_dev, max_vectors, PCI_IRQ_MSI)) ||
		enable_interrupts(tt_dev, 1, PCI_IRQ_ALL_TYPES);

	if (tt_dev->interrupt_enabled && tt_dev->interrupt_vectors < max_vectors)
		dev_info(&tt_dev->pdev->dev, "Got %u of %u interrupt vectors\n",
			 tt_dev->interrupt_vectors, max_vectors);

	return tt_dev->interrupt_enabled;
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
