#ifndef TTDRIVER_MODULE_H_INCLUDED
#define TTDRIVER_MODULE_H_INCLUDED

#include <linux/types.h>
// Module options that need to be passed to other files
extern bool arc_fw_init;
extern bool arc_fw_override;
extern bool arc_fw_stage2_init;
extern bool ddr_train_en;
extern bool ddr_test_mode;
extern uint ddr_frequency_override;
extern bool aiclk_ppm_en;
extern uint aiclk_fmax_override;
extern uint arc_fw_feat_dis_override;
extern bool watchdog_fw_en;
extern bool watchdog_fw_override;
extern bool smbus_fw_en;
extern bool smbus_fw_override;
extern uint axiclk_override;
extern uint tensix_harvest_override;
extern uint dma_address_bits;
#endif
