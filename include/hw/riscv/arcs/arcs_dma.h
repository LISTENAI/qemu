/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_DMA_H
#define HW_RISCV_ARCS_DMA_H

#include "hw/dma/listenai_dma.h"
typedef struct ArcsSoC ArcsSoC;
typedef ListenAIDMATransfer ArcsDMATransfer;
typedef ListenAIDMA ArcsDMA;

void arcs_dma_init(ArcsSoC *soc);
void arcs_dma_reset(ArcsSoC *soc);
#endif
