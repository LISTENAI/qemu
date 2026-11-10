/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_ECC_H
#define HW_RISCV_ARCS_ECC_H

#include "hw/riscv/arcs/arcs_ecc_math.h"
#include "qemu/timer.h"
#include "system/memory.h"

typedef struct ArcsSoC ArcsSoC;
typedef struct ArcsECC {
    ArcsSoC *soc;
    MemoryRegion mmio;
    QEMUTimer *timer;
    uint32_t ram[0x1000 / 4];
    uint32_t control, entry, ie;
    unsigned stride;
    ArcsECCField field;
    int64_t remaining;
    bool clock, busy, pending, zero;
} ArcsECC;

void arcs_ecc_init(ArcsSoC *soc);
void arcs_ecc_finalize(ArcsSoC *soc);
void arcs_ecc_reset(ArcsSoC *soc);
void arcs_ecc_clock(ArcsSoC *soc, bool enabled);

#endif
