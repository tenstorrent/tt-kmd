#include <linux/delay.h>
#include <linux/types.h>
#include <asm/io.h>

#include "grayskull.h"

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

bool grayskull_send_arc_fw_message(u8 __iomem* reset_unit_regs, u8 message_id, u32 timeout_us) {
	u32 delay_counter = 0;
	void __iomem *scratch_reg_5 = reset_unit_regs + SCRATCH_REG(5);

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

static u32 read_fw_post_code(u8 __iomem* reset_unit_regs) {
	u32 post_code = ioread32(reset_unit_regs + POST_CODE_REG);
	return post_code & POST_CODE_MASK;
}

// This is shared with wormhole.
bool grayskull_shutdown_firmware(u8 __iomem* reset_unit_regs) {
	const u32 post_code_timeout = 1000;
	u32 delay_counter = 0;

	if (!grayskull_send_arc_fw_message(reset_unit_regs, GS_FW_MSG_SHUTDOWN, 5000)) // 2249 observed
		return false;

	while (1) {
		u32 post_code = read_fw_post_code(reset_unit_regs);
		if (post_code == POST_CODE_ARC_SLEEP)
			return true;

		if (delay_counter++ >= post_code_timeout) {
			printk(KERN_WARNING "Timeout waiting for sleep post code.\n");
			return false;
		}
		udelay(1);
	}
}

bool grayskull_init(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = container_of(tt_dev, struct grayskull_device, tt);

	gs_dev->reset_unit_regs = pci_iomap_range(gs_dev->tt.pdev, RESET_UNIT_BAR, RESET_UNIT_REG_START, RESET_UNIT_REG_LEN);
	return (gs_dev->reset_unit_regs != NULL);
}

void grayskull_cleanup(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = container_of(tt_dev, struct grayskull_device, tt);

	if (gs_dev->reset_unit_regs != NULL) {
		grayskull_shutdown_firmware(gs_dev->reset_unit_regs);
		pci_iounmap(gs_dev->tt.pdev, gs_dev->reset_unit_regs);
	}
}

struct tenstorrent_device_class grayskull_class = {
	.name = "Grayskull",
	.instance_size = sizeof(struct grayskull_device),
	.init_device = grayskull_init,
	.cleanup_device = grayskull_cleanup,
};
