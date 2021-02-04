#ifndef TTDRIVER_GRAYSKULL_H_INCLUDED
#define TTDRIVER_GRAYSKULL_H_INCLUDED

#include <linux/types.h>

struct grayskull_device;

bool grayskull_send_arc_fw_message(struct grayskull_device *gs_dev, u8 message_id, u32 timeout_us);
bool grayskull_init(struct grayskull_device *gs_dev);
void grayskull_cleanup(struct grayskull_device *gs_dev);

#endif
