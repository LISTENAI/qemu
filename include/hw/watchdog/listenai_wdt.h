/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_WATCHDOG_LISTENAI_WDT_H
#define HW_WATCHDOG_LISTENAI_WDT_H
#include "hw/sysbus.h"
#include "qemu/timer.h"
typedef struct ListenaiWDT {
    MemoryRegion io;
    hwaddr base;
    QEMUTimer *event;
    uint32_t control, clock_hz;
    bool unlocked, expired, reset_stage, clock, armed;
    bool ignore_locked_writes;
    int64_t remaining_ns;
    void *opaque;
    void (*irq)(void *opaque, bool level);
    void (*reset)(void *opaque);
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
} ListenaiWDT;
void listenai_wdt_init(ListenaiWDT *s, Object *owner, const char *name,
                      hwaddr base, uint32_t clock_hz);
void listenai_wdt_reset(ListenaiWDT *s);
void listenai_wdt_clock(ListenaiWDT *s, bool enabled);
#endif
