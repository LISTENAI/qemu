/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_INTC_NUCLEI_CIDU_H
#define HW_INTC_NUCLEI_CIDU_H
#include "system/memory.h"

typedef struct NucleiCIDU {
    MemoryRegion io;
    hwaddr base;
    unsigned cores, interrupts, semaphores, irq_offset, ici_irq;
    uint32_t status[2], semaphore[32], targets[80], claims[80];
    bool levels[80];
    void *opaque;
    void (*irq)(void *opaque, unsigned core, unsigned irq, bool level);
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
} NucleiCIDU;
void nuclei_cidu_init(NucleiCIDU *s, Object *owner, const char *name,
                     hwaddr base, unsigned cores, unsigned interrupts,
                     unsigned semaphores, unsigned irq_offset, unsigned ici_irq);
void nuclei_cidu_reset(NucleiCIDU *s);
void nuclei_cidu_input(NucleiCIDU *s, unsigned interrupt, bool level);
void nuclei_cidu_redrive(NucleiCIDU *s);
#endif
