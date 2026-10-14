/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_LISTENAI_SPI_H
#define HW_SSI_LISTENAI_SPI_H
#include "hw/ssi/ssi.h"
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/int128.h"
/* Exact rate scale for the supported integer PLL and N/M dividers. */
#define LISTENAI_SPI_CLOCK_SCALE UINT64_C(4324320)
typedef struct ListenaiSPICallbacks {
    void *opaque;
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
    void (*irq)(void *opaque, bool level);
    void (*cs)(void *opaque, bool level);
    void (*dma)(void *opaque, bool receive, bool transmit);
    bool (*dma_active)(void *opaque);
    uint64_t (*clock_rate)(void *opaque);
} ListenaiSPICallbacks;
typedef struct ListenaiSPI {
    ListenaiSPICallbacks callbacks;
    hwaddr base;
    MemoryRegion io;
    SSIBus *bus;
    QEMUTimer *service;
    uint32_t regs[32], fifo[16], pending;
    unsigned head, count, bits;
    uint64_t remaining, frames;
    Int128 work;
    uint64_t rate;
    int64_t updated;
    bool active, io_busy, phase, safe_reads;
    bool (*route_valid)(void *opaque);
    void *route_opaque;
} ListenaiSPI;
void listenai_spi_init(ListenaiSPI *s, DeviceState *owner, const char *name,
                       hwaddr base, ListenaiSPICallbacks callbacks);
void listenai_spi_reset(ListenaiSPI *s);
void listenai_spi_clock_changed(ListenaiSPI *s);
#endif
