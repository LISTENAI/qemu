/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_LUNA_H
#define HW_RISCV_ARCS_LUNA_H
#include "hw/misc/listenai_luna.h"
typedef struct ArcsSoC ArcsSoC;
typedef ListenAILUNA ArcsLUNA;
void arcs_luna_init(ArcsSoC *s);
void arcs_luna_reset(ArcsSoC *s);
#endif
