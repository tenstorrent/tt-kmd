// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_COMPAT_H_INCLUDED
#define TTDRIVER_COMPAT_H_INCLUDED

#include <linux/version.h>

// class_create() API changed in kernel 6.4.0 to take only one argument.
// RHEL 9.0+ backported this change.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define TT_CLASS_CREATE_NEW_API
#else
#  ifdef RHEL_RELEASE_CODE
#    if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)
#      define TT_CLASS_CREATE_NEW_API
#    endif
#  endif
#endif

// PCIe AER functions were removed in kernel 6.0.0.
// RHEL 9.0+ backported this change.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define TT_NO_PCIE_AER
#else
#  ifdef RHEL_RELEASE_CODE
#    if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9, 0)
#      define TT_NO_PCIE_AER
#    endif
#  endif
#endif

#ifdef TT_NO_PCIE_AER
#define pci_enable_pcie_error_reporting(dev) do { } while (0)
#define pci_disable_pcie_error_reporting(dev) do { } while (0)
#endif

#endif

