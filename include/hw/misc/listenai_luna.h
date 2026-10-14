/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_LISTENAI_LUNA_H
#define HW_MISC_LISTENAI_LUNA_H
#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "lisem/luna.h"

typedef struct ListenAILUNA {
    MemoryRegion io;
    QEMUTimer *completion;
    LisemLuna *backend;
    void *opaque;
    void (*fatal)(void *, const char *);
    void (*irq)(void *, bool);
    bool (*read_extra)(void *, hwaddr, uint64_t *);
    bool (*write_extra)(void *, hwaddr, uint32_t);
    void (*reset_extra)(void *);
    uint32_t irq_mask, irq_mask_bits;
    bool safe_reads, clock_enabled, pending;
    uint64_t remaining;
    uint32_t reset_mask;
} ListenAILUNA;

void listenai_luna_init(ListenAILUNA *s, Object *owner, MemoryRegion *memory,
                       hwaddr base, uint32_t reset_mask, bool safe_reads,
                       void *opaque,
                       void (*fatal)(void *, const char *));
void listenai_luna_reset(ListenAILUNA *s);
void listenai_luna_clock(ListenAILUNA *s, bool enabled);
#endif
