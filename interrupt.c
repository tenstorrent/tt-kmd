#include <linux/pci.h>
#include <linux/types.h>
#include <linux/interrupt.h>

#include "device.h"
#include "enumerate.h"

static irqreturn_t irq_handler(int irq, void *device)
{
	struct tenstorrent_device *tt_dev = device;
	(void)tt_dev;	// to be used later

	return IRQ_HANDLED;
}

bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev)
{
	// MSI-X capability exists in GS but is not fully implemented
	if (pci_alloc_irq_vectors(tt_dev->pdev, 1, 1, PCI_IRQ_LEGACY | PCI_IRQ_MSI) <= 0)
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

void tenstorrent_disable_interrupts(struct tenstorrent_device *tt_dev)
{
	if (tt_dev->interrupt_enabled) {
		free_irq(pci_irq_vector(tt_dev->pdev, 0), tt_dev);
		pci_free_irq_vectors(tt_dev->pdev);
		tt_dev->interrupt_enabled = false;
	}
}
