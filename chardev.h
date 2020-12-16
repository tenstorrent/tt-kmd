#ifndef TTDRIVER_CHARDEV_H_INCLUDED
#define TTDRIVER_CHARDEV_H_INCLUDED

struct grayskull_device;

extern int init_char_driver(unsigned int max_devices);
extern void cleanup_char_driver(void);

int tenstorrent_register_device(struct grayskull_device *gs_dev);
void tenstorrent_unregister_device(struct grayskull_device *gs_dev);

#endif
