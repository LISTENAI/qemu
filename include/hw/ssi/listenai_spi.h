/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_LISTENAI_SPI_H
#define HW_SSI_LISTENAI_SPI_H

#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/int128.h"

/* Exact rate scale for the supported integer PLL and N/M dividers. */
#define LISTENAI_SPI_CLOCK_SCALE UINT64_C(4328644320)

typedef struct ListenaiSPICallbacks {
    void *opaque;
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
    bool (*dma_active)(void *opaque);
    /* Source clock multiplied by LISTENAI_SPI_CLOCK_SCALE; zero is gated. */
    uint64_t (*clock_rate)(void *opaque);
    /* Select a concrete digital route; observation alone must not select it. */
    bool (*pins_connected)(void *opaque);
} ListenaiSPICallbacks;

/* Synthesized IP properties, independent of board routing. */
typedef struct ListenaiSPIHardware {
    unsigned tx_depth, rx_depth;
    uint32_t feature;
} ListenaiSPIHardware;

#define TYPE_LISTENAI_SPI "listenai-spi"
OBJECT_DECLARE_SIMPLE_TYPE(ListenaiSPI, LISTENAI_SPI)

struct ListenaiSPI {
    SysBusDevice parent_obj;

    /* SysBus IRQ; GPIO cs, dma[RX/TX], pins-out, pins-enable and pins-in. */
    qemu_irq irq, cs, dma[2], pins[4], pin_enable[4];
    char *bus_name;
    ListenaiSPICallbacks callbacks;
    hwaddr base;
    MemoryRegion io;
    SSIBus *bus;
    QEMUTimer *service;
    uint32_t regs[32], fifo[16], rx_fifo[16], pending;
    unsigned head, count, rx_head, rx_count, bits, mode;
    ListenaiSPIHardware hardware;
    bool serial, selected, pin_clock, pin_data, inflight, consumed;
    unsigned bit_index, half_edges, tx_left, rx_packed;
    uint32_t serial_tx, serial_rx, packed_tx, packed_rx;
    uint64_t remaining, frames;
    Int128 work;
    uint64_t rate;
    int64_t updated;
    bool active, io_busy, phase, safe_reads;
    bool (*route_valid)(void *opaque);
    void *route_opaque;
};

enum {
    LISTENAI_SPI_CLOCK,
    LISTENAI_SPI_MOSI,
    LISTENAI_SPI_MISO,
    LISTENAI_SPI_CS
};

void listenai_spi_pin_input(ListenaiSPI *s, unsigned pin, bool level);
void listenai_spi_reset(ListenaiSPI *s);
void listenai_spi_clock_changed(ListenaiSPI *s);

#endif
