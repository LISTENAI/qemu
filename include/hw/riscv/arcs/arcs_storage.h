/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_STORAGE_H
#define HW_RISCV_ARCS_STORAGE_H

#include "hw/ssi/listenai_spimem.h"
typedef struct ArcsSoC ArcsSoC;

typedef ListenAINOR ArcsNOR;

typedef struct ArcsFlash {
    ArcsSoC *soc;
    ListenAISpiMem core;
    ArcsNOR *chips[2];
} ArcsFlash;

typedef struct ArcsOTP {
    ArcsSoC *soc;
    MemoryRegion io;
    uint8_t bytes[512];
    uint32_t control, timing, divider;
} ArcsOTP;

void arcs_nor_init(ArcsNOR *chip, hwaddr address, const char *image, bool persist);
void arcs_storage_init(ArcsSoC *soc);
void arcs_storage_reset(ArcsSoC *soc);
void arcs_flash_reset(ArcsSoC *soc);
#endif
