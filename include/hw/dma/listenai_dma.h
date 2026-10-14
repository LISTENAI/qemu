/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DMA_LISTENAI_DMA_H
#define HW_DMA_LISTENAI_DMA_H
#include "system/memory.h"

typedef void (*ListenAIDMAIRQ)(void *opaque, uint32_t pending_channels);
typedef void (*ListenAIDMAFault)(void *opaque, hwaddr address, unsigned size,
                               bool write, uint64_t value);
typedef struct ListenAIDMATransfer {
    uint32_t remaining;
    unsigned width, source_mode, dest_mode, request;
    bool active;
} ListenAIDMATransfer;
typedef struct ListenAIDMA {
    MemoryRegion io;
    uint32_t regs[0x3b4 / 4];
    ListenAIDMATransfer channel[8];
    bool requests[16], servicing, transferring;
    hwaddr address;
    unsigned channel_count;
    ListenAIDMAIRQ irq;
    ListenAIDMAFault fault;
    void *opaque;
} ListenAIDMA;

void listenai_dma_init(ListenAIDMA *s, Object *owner, const char *name,
                       hwaddr address, unsigned channel_count,
                       ListenAIDMAIRQ irq, ListenAIDMAFault fault, void *opaque);
void listenai_dma_reset(ListenAIDMA *s);
void listenai_dma_request(ListenAIDMA *s, unsigned request, bool level);
#endif
