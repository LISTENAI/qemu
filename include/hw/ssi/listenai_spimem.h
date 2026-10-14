/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SSI_LISTENAI_SPIMEM_H
#define HW_SSI_LISTENAI_SPIMEM_H
#include "hw/block/listenai_nor.h"

typedef ListenAINOR *(*ListenAISpiMemSelect)(void *opaque, uint32_t address);
typedef bool (*ListenAISpiMemValid)(void *opaque, hwaddr offset, bool write);
typedef void (*ListenAISpiMemIrq)(void *opaque, bool level);
typedef void (*ListenAISpiMemFault)(void *opaque, hwaddr address, unsigned size,
                                   bool write, uint64_t value);

typedef struct ListenAISpiMem {
    MemoryRegion io;
    hwaddr address;
    uint32_t regs[0x100 / 4];
    uint8_t tx[512], rx[1024];
    unsigned tx_count, rx_count, rx_head;
    bool pending;
    ListenAISpiMemSelect select;
    ListenAISpiMemValid valid;
    ListenAISpiMemIrq irq;
    ListenAISpiMemFault fault;
    void *opaque;
} ListenAISpiMem;

void listenai_spimem_init(ListenAISpiMem *s, Object *owner, const char *name,
                          hwaddr address, ListenAISpiMemSelect select,
                          ListenAISpiMemValid valid, ListenAISpiMemIrq irq,
                          ListenAISpiMemFault fault, void *opaque);
void listenai_spimem_reset(ListenAISpiMem *s);
#endif
