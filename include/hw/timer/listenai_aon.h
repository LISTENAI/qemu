/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_LISTENAI_AON_H
#define HW_TIMER_LISTENAI_AON_H
#include "hw/sysbus.h"
#include "qemu/timer.h"
typedef struct ListenaiAONCallbacks {
    void *opaque;
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
    void (*irq)(void *opaque, bool level);
    void (*reset)(void *opaque, bool reset_pmu);
} ListenaiAONCallbacks;
typedef struct ListenaiAONTimer {
    MemoryRegion io;
    hwaddr base;
    ListenaiAONCallbacks callbacks;
    QEMUTimer *event;
    int64_t epoch;
    uint32_t control, value, phase;
    bool loaded, pending, irq_enable, clock;
} ListenaiAONTimer;
typedef struct ListenaiAONWDT {
    MemoryRegion io;
    hwaddr base;
    ListenaiAONCallbacks callbacks;
    QEMUTimer *event;
    uint32_t control, load, cause;
    bool stop_locked, start_locked, reset_pmu, enabled;
} ListenaiAONWDT;
void listenai_aon_timer_init(ListenaiAONTimer *s, Object *owner,
                            const char *name, hwaddr base,
                            ListenaiAONCallbacks callbacks);
void listenai_aon_timer_reset(ListenaiAONTimer *s);
void listenai_aon_timer_clock(ListenaiAONTimer *s, bool enabled);
void listenai_aon_wdt_init(ListenaiAONWDT *s, Object *owner,
                          const char *name, hwaddr base,
                          ListenaiAONCallbacks callbacks);
void listenai_aon_wdt_reset(ListenaiAONWDT *s, uint32_t cause);
#endif
