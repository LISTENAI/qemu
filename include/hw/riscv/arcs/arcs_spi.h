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
// QSPI signals: 0 = clock, 1..4 = IO0..IO3.
bool arcs_qspi_pad_valid(unsigned signal, unsigned pad);
bool arcs_qspi_pad_selected(ArcsSoC *soc, unsigned signal, unsigned pad);
void arcs_qspi_init(ArcsSoC *soc);
void arcs_qspi_clock_changed(ArcsSoC *soc);
void arcs_spi_init(ArcsSoC *soc);
void arcs_spi_clock_changed(ArcsSoC *soc, unsigned index);
void arcs_spi_update_dma(ArcsSoC *soc);
void arcs_spi_pins_changed(ArcsSoC *soc);
void arcs_spi_pad_input(ArcsSoC *soc, unsigned pad, bool level);
void arcs_spi_reset(ArcsSoC *soc, unsigned index);
bool arcs_spi_pad_valid(unsigned index, unsigned pin, unsigned pad);
bool arcs_spi_pad_selected(ArcsSPI *s, unsigned pin, unsigned pad);
#endif
