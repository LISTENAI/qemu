/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_VENUSA_DVP_H
#define HW_RISCV_VENUSA_DVP_H
#include "system/memory.h"
typedef struct VenusADVP {
    struct VenusASoC *soc;
    MemoryRegion io, fifo_io;
    uint32_t regs[0x44 / 4], fifo[64], word;
    unsigned head, count, packed, line, byte;
    int input[4];
    bool clock, frame, last_vsync, last_hsync, request;
    uint64_t samples, frames, overflow;
    uint32_t xclk_config, xclk_div;
    void (*camera_clock_changed)(void *opaque);
    void *camera_opaque;
} VenusADVP;
void venusa_dvp_init(struct VenusASoC *s);
void venusa_dvp_reset(struct VenusASoC *s);
void venusa_dvp_clock(struct VenusASoC *s);
void venusa_dvp_dma_update(struct VenusASoC *s);
void venusa_dvp_xclk_ratio(struct VenusASoC *s, uint64_t *n, uint64_t *d);
#endif
