// *************************************** //
//      THIS FILE IS AUTO GENERATED        //
// *************************************** //

#ifndef _TTKMD_ARC_IF_H_
#define _TTKMD_ARC_IF_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define TTKMD_ARC_MAGIC_NUMBER_0  (0x54544b4d)
#define TTKMD_ARC_MAGIC_NUMBER_1  (0x44415243)
#define TTKMD_ARC_IF_VERSION      0x2

typedef struct {
  uint32_t  magic_number[2];
  uint32_t  version;
  uint8_t   auto_init;
  uint8_t   padding0[3];
  uint8_t   aiclk_ppm_en;
  uint8_t   padding1[3];
  uint32_t  aiclk_ppm_ovr;
  uint8_t   ddr_train_en;
  uint8_t   padding2[3];
  uint32_t  ddr_freq_ovr;
  uint8_t   watchdog_fw_load;
  uint8_t   watchdog_fw_en;
  uint8_t   padding3[2];
  uint32_t  watchdog_fw_reset_vec;
} ttkmd_arc_if_t; // 10 * 4 = 40B

typedef union {
  uint32_t        val[0x1000 / sizeof(uint32_t)];
  ttkmd_arc_if_t  f;
} ttkmd_arc_if_u; // 1024 * 4 = 4096B

#endif