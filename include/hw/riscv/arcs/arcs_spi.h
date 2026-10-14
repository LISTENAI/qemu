/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_SPI_H
#define HW_RISCV_ARCS_SPI_H
#include "hw/ssi/listenai_spi.h"
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/int128.h"
typedef struct ArcsSoC ArcsSoC;
typedef struct ArcsSPI {
    ListenaiSPI device;
    ArcsSoC *soc;
    unsigned index;
} ArcsSPI;
void arcs_spi_init(ArcsSoC *soc);
void arcs_spi_clock_changed(ArcsSoC *soc, unsigned index);
void arcs_spi_reset(ArcsSoC *soc, unsigned index);
#endif
