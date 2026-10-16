/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_PSRAM_H
#define HW_RISCV_ARCS_PSRAM_H

#include "hw/mem/listenai_psram.h"
typedef struct ArcsSoC ArcsSoC;
typedef ListenAIPSRAM ArcsPSRAM;

void arcs_xccela_init(ListenAIXccela *chip, uint64_t size);
void arcs_psram_init(ArcsSoC *soc);
void arcs_psram_reset(ArcsSoC *soc);
#endif
