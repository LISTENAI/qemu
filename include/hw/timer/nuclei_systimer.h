/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_TIMER_NUCLEI_SYSTIMER_H
#define HW_TIMER_NUCLEI_SYSTIMER_H
#include "qemu/timer.h"

typedef void (*NucleiSysTimerIRQ)(void *opaque, unsigned irq, bool level);
typedef struct NucleiSysTimer {
    QEMUTimer *event;
    uint64_t value, compare, phase;
    int64_t epoch;
    uint32_t frequency, control;
    bool software, clock_enabled;
    NucleiSysTimerIRQ irq;
    void *opaque;
} NucleiSysTimer;

void nuclei_systimer_init(NucleiSysTimer *t, NucleiSysTimerIRQ irq,
                         void *opaque);
void nuclei_systimer_reset(NucleiSysTimer *t, uint32_t frequency);
uint64_t nuclei_systimer_value(void *opaque);
void nuclei_systimer_clock(NucleiSysTimer *t, uint32_t frequency, bool enabled);
bool nuclei_systimer_read(NucleiSysTimer *t, uint32_t offset, uint32_t *value);
bool nuclei_systimer_write(NucleiSysTimer *t, uint32_t offset, uint32_t value);
#endif
