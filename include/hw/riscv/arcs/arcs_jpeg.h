/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_JPEG_H
#define HW_RISCV_ARCS_JPEG_H
#include "hw/misc/listenai_jpeg.h"
typedef struct ArcsSoC ArcsSoC;
bool arcs_jpeg_dma_output_info(ArcsSoC *, unsigned, uint32_t *, uint32_t *, bool *);
void arcs_jpeg_init(ArcsSoC *);
void arcs_jpeg_reset(ArcsSoC *);
void arcs_jpeg_clock(ArcsSoC *, bool);
#endif
