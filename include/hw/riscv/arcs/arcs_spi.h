/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_SPI_H
#define HW_RISCV_ARCS_SPI_H
#include "hw/ssi/listenai_spi.h"
typedef struct ArcsSoC ArcsSoC;
typedef struct ArcsSPI {
    ListenaiSPI device;
    bool pin_enabled[4], pin_level[4];
    ArcsSoC *soc;
    unsigned index;
    int selected_pad[4];
    bool dma_request[2];
} ArcsSPI;
void arcs_spi_init(ArcsSoC *soc);
void arcs_spi_clock_changed(ArcsSoC *soc, unsigned index);
void arcs_spi_update_dma(ArcsSoC *soc);
void arcs_spi_pins_changed(ArcsSoC *soc);
void arcs_spi_pad_input(ArcsSoC *soc, unsigned pad, bool level);
void arcs_spi_reset(ArcsSoC *soc, unsigned index);
#endif
