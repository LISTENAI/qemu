/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_VENUSA_SPI_H
#define HW_RISCV_VENUSA_SPI_H
#include "hw/ssi/listenai_spi.h"
typedef struct VenusASoC VenusASoC;
typedef struct VenusASPI {
    VenusASoC *soc;
    ListenaiSPI device;
    unsigned index, n, m;
    uint32_t clock_config;
    bool rx_request, tx_request;
    qemu_irq cs;
} VenusASPI;
void venusa_spi_init(VenusASoC *s);
void venusa_spi_reset(VenusASoC *s);
void venusa_spi_clock(VenusASoC *s);
void venusa_spi_dma_update(VenusASoC *s);
#endif
