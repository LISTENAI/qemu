/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DMA_VENUSA_DMA2D_H
#define HW_DMA_VENUSA_DMA2D_H
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/int128.h"
#define VENUSA_DMA2D_CHANNELS 6

typedef struct VenusDMA2DCallbacks {
    void *opaque;
    void (*irq)(void *opaque, unsigned channel, bool level);
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value);
    bool (*memory_allowed)(void *opaque, hwaddr address, unsigned size, bool write);
    /* Peripheral request mux and FIFO readiness, checked for every beat. */
    bool (*request)(void *opaque, unsigned handshake, hwaddr address, bool write);
} VenusDMA2DCallbacks;

typedef struct VenusDMA2DDescriptor {
    uint32_t control, src, dst, length, irq_enable, block_mask;
} VenusDMA2DDescriptor;

typedef struct VenusDMA2DChannel {
    VenusDMA2DDescriptor working, queued;
    uint32_t transferred, raw, src_cursor, dst_cursor;
    bool active, pending;
} VenusDMA2DChannel;

typedef struct VenusDMA2D {
    MemoryRegion io;
    hwaddr base;
    VenusDMA2DCallbacks cb;
    uint32_t regs[0x504 / 4];
    VenusDMA2DChannel channels[VENUSA_DMA2D_CHANNELS];
    QEMUTimer *event;
    uint64_t clock_n, clock_d;
    Int128 clock_work;
    int64_t event_epoch;
    bool clock_enabled, servicing;
} VenusDMA2D;

void venusa_dma2d_init(VenusDMA2D *s, Object *owner, hwaddr base, VenusDMA2DCallbacks cb);
void venusa_dma2d_reset(VenusDMA2D *s);
void venusa_dma2d_clock(VenusDMA2D *s, bool enabled, uint64_t numerator, uint64_t denominator);
void venusa_dma2d_request(VenusDMA2D *s);
#endif
