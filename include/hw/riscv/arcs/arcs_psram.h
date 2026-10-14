/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_PSRAM_H
#define HW_RISCV_ARCS_PSRAM_H

#include "hw/mem/listenai_psram.h"
typedef struct ArcsSoC ArcsSoC;
typedef ListenAIXccela ArcsXccela128;
typedef ListenAIPSRAM ArcsPSRAM;

void arcs_xccela_init(ArcsXccela128 *chip);
void arcs_psram_init(ArcsSoC *soc);
void arcs_psram_reset(ArcsSoC *soc);
#endif
