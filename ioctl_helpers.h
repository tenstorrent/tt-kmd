#ifndef TTDRIVER_IOCTL_HELPERS_H_INCLUDED
#define TTDRIVER_IOCTL_HELPERS_H_INCLUDED

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
#define copy_struct_to_user_fixed copy_struct_to_user
#else
int copy_struct_to_user_fixed(void __user *dst, size_t usize,
			      const void *src, size_t ksize,
			      bool *ignored_trailing);
#endif

#define READ_IOCTL_INPUT(arg, data, minsz)									\
	do {													\
		int read_ioctl_input_ret;									\
														\
		if (get_user((data).argsz, &(arg)->argsz))							\
			return -EFAULT;										\
														\
		if ((data).argsz < minsz)									\
			return -EINVAL;										\
														\
		read_ioctl_input_ret = copy_struct_from_user(&(data), sizeof(data), (arg), (data).argsz);	\
		if (read_ioctl_input_ret)									\
			return read_ioctl_input_ret;								\
	} while (0)

#define WRITE_IOCTL_OUTPUT(arg, data)								\
	({											\
		uint32_t input_ioctl_size = (data).argsz;					\
		(data).argsz = sizeof(data);							\
		copy_struct_to_user_fixed(arg, input_ioctl_size, &data, sizeof(data), NULL);	\
	})

#endif
