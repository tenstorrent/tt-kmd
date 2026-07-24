// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_TEST_FWLOG_COMPAT_H_INCLUDED
#define TTDRIVER_TEST_FWLOG_COMPAT_H_INCLUDED

#include <linux/types.h>

typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;

// The kernel build gets __packed from <linux/compiler_attributes.h>; the host
// build needs its own definition.
#ifndef __packed
#define __packed __attribute__((packed))
#endif

#endif // TTDRIVER_TEST_FWLOG_COMPAT_H_INCLUDED
