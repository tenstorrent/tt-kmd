#include <linux/delay.h>
#include <linux/types.h>
#include <asm/io.h>

#include "enumerate.h"

#define RESET_UNIT_BAR 0
#define RESET_UNIT_REG_START 0x1FF30000
#define RESET_UNIT_REG_LEN 0x10000

#define SCRATCH_REG(n) (0x60 + (n)*sizeof(u32))	/* byte offset */

#define POST_CODE_REG SCRATCH_REG(0)
#define POST_CODE_MASK ((u32)0x3FFF)
#define POST_CODE_ARC_SLEEP 2

// Scratch register 5 is used for the firmware message protocol.
// Write 0xAA00 | message_id into scratch register 5, wait for message_id to appear.
// After reading the message, the firmware will immediately reset SR5 to 0 and write message_id when done.
// Appearance of any other value indicates a conflict with another message.
#define GS_FW_MESSAGE_PRESENT 0xAA00

#define GS_FW_MSG_SHUTDOWN 0x55

bool grayskull_send_arc_fw_message(struct grayskull_device *gs_dev, u8 message_id, u32 timeout_us) {
	u32 delay_counter = 0;
	void __iomem *scratch_reg_5 = gs_dev->reset_unit_regs + SCRATCH_REG(5);

	iowrite32(GS_FW_MESSAGE_PRESENT | message_id, scratch_reg_5);

	while (1) {
		u32 response = ioread32(scratch_reg_5);
		if (response == message_id)
			return true;

		if (delay_counter++ >= timeout_us) {
			printk(KERN_WARNING "Tenstorrent FW message timeout: %08X.\n", (unsigned int)message_id);
			return false;
		}
		udelay(1);
	}
}

static u32 read_fw_post_code(struct grayskull_device *gs_dev) {
	u32 post_code = ioread32(gs_dev->reset_unit_regs + POST_CODE_REG);
	return post_code & POST_CODE_MASK;
}

static bool grayskull_shutdown_firmware(struct grayskull_device *gs_dev) {
	const u32 post_code_timeout = 1000;
	u32 delay_counter = 0;

	if (!grayskull_send_arc_fw_message(gs_dev, GS_FW_MSG_SHUTDOWN, 5000)) // 2249 observed
		return false;

	while (1) {
		u32 post_code = read_fw_post_code(gs_dev);
		if (post_code == POST_CODE_ARC_SLEEP)
			return true;

		if (delay_counter++ >= post_code_timeout) {
			printk(KERN_WARNING "Timeout waiting for sleep post code.\n");
			return false;
		}
		udelay(1);
	}
}

bool grayskull_init(struct grayskull_device *gs_dev) {
	gs_dev->reset_unit_regs = pci_iomap_range(gs_dev->pdev, RESET_UNIT_BAR, RESET_UNIT_REG_START, RESET_UNIT_REG_LEN);
	return (gs_dev->reset_unit_regs != NULL);
}

void grayskull_cleanup(struct grayskull_device *gs_dev) {
	grayskull_shutdown_firmware(gs_dev);

	if (gs_dev->reset_unit_regs != NULL)
		pci_iounmap(gs_dev->pdev, gs_dev->reset_unit_regs);
}
