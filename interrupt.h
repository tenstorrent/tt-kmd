#ifndef TTDRIVER_INTERRUPT_H_INCLUDED
#define TTDRIVER_INTERRUPT_H_INCLUDED

struct grayskull_device;

bool tenstorrent_enable_interrupts(struct grayskull_device *gs_dev);
void tenstorrent_disable_interrupts(struct grayskull_device *gs_dev);

#endif
