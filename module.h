#ifndef TTDRIVER_MODULE_H_INCLUDED
#define TTDRIVER_MODULE_H_INCLUDED

#include <linux/types.h>
// Module options that need to be passed to other files
extern bool auto_init;
extern bool arc_fw_override;
extern bool ddr_train_en;
extern uint ddr_frequency_override;
extern bool aiclk_ppm_en;
extern uint aiclk_fmax_override;
extern bool watchdog_fw_en;
extern bool watchdog_fw_override;
extern uint axiclk_override;
#endif
