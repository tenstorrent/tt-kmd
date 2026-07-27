#include "ioctl_helpers.h"

#include <linux/kernel.h>
#include <linux/string.h>


/* copy_struct_to_user() appeared in v6.13; its ignored_trailing logic was
 * fixed in v7.2. Until our minimum supported kernel includes the fix, use a corrected local copy.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0)
int copy_struct_to_user_fixed(void __user *dst, size_t usize,
			      const void *src, size_t ksize,
			      bool *ignored_trailing)
{
	size_t size = min(ksize, usize);
	size_t rest = max(ksize, usize) - size;

	/* new caller, old kernel: zero the tail */
	if (usize > ksize && clear_user(dst + size, rest))
		return -EFAULT;

	/* old caller, new kernel: did we drop data? */
	if (ignored_trailing)
		*ignored_trailing = ksize > usize
			&& memchr_inv(src + size, 0, rest) != NULL;

	if (copy_to_user(dst, src, size))
		return -EFAULT;

	return 0;
}
#endif
