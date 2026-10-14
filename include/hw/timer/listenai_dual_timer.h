/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_LISTENAI_DUAL_TIMER_H
#define HW_TIMER_LISTENAI_DUAL_TIMER_H
#include "system/memory.h"
#include "qemu/timer.h"

typedef void (*ListenAIDualTimerIRQ)(void *opaque, bool level);
typedef void (*ListenAIDualTimerFault)(void *opaque, hwaddr address, unsigned size,
                                     bool write, uint64_t value);
typedef struct ListenAIDualTimer ListenAIDualTimer;
typedef struct ListenAIDualChannel {
    ListenAIDualTimer *block;
    QEMUTimer *event;
    int64_t epoch;
    uint64_t phase;
    uint32_t control, load, value;
    bool pending;
} ListenAIDualChannel;
struct ListenAIDualTimer {
    MemoryRegion io;
    ListenAIDualChannel channel[2];
    hwaddr address;
    uint64_t source_period_ns;
    ListenAIDualTimerIRQ irq;
    ListenAIDualTimerFault fault;
    void *opaque;
};

void listenai_dual_timer_init(ListenAIDualTimer *s, Object *owner,
                              const char *name, hwaddr address,
                              uint64_t source_period_ns,
                              ListenAIDualTimerIRQ irq,
                              ListenAIDualTimerFault fault, void *opaque);
void listenai_dual_timer_reset(ListenAIDualTimer *s);
#endif
