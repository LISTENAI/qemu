/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_GPT_H
#define HW_RISCV_ARCS_GPT_H
#include "system/memory.h"
typedef struct ArcsSoC ArcsSoC;
typedef struct ArcsGPT {
    ArcsSoC *soc;
    MemoryRegion io;
    uint32_t regs[0x148 / 4];
    bool running[8];
    void (*changed)(void *opaque);
    void *opaque;
} ArcsGPT;
void arcs_gpt_init(ArcsSoC *soc);
void arcs_gpt_reset(ArcsSoC *soc);
double arcs_gpt_duty(ArcsGPT *s, unsigned channel);
#endif
