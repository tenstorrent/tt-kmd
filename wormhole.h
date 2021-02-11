#ifndef TTDRIVER_WORMHOLE_H_INCLUDED
#define TTDRIVER_WORMHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"

struct wormhole_device {
	struct tenstorrent_device tt;
	u8 __iomem *bar2_mapping;
	u8 __iomem *bar4_mapping;
};

#endif
