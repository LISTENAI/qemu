/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/timer/nuclei_systimer.h"

uint64_t nuclei_systimer_value(void *opaque)
{
    NucleiSysTimer *t = opaque;
    uint64_t delta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->epoch;
    __uint128_t progress = t->phase;
    if (!(t->control & 1) && t->clock_enabled) {
        progress += (__uint128_t)delta * t->frequency;
    }
    return t->value + progress / 1000000000;
}

static void timer_sync(NucleiSysTimer *t)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!(t->control & 1) && t->clock_enabled) {
        __uint128_t progress = t->phase + (__uint128_t)(now - t->epoch) * t->frequency;
        t->value += progress / 1000000000;
        t->phase = progress % 1000000000;
    }
    t->epoch = now;
}

static void timer_update(NucleiSysTimer *t)
{
    timer_sync(t);
    uint64_t now = t->value;
    t->irq(t->opaque, 7, now >= t->compare);
    timer_del(t->event);
    if (!(t->control & 1) && t->clock_enabled && t->compare > now) {
        __uint128_t needed = (__uint128_t)(t->compare - now) * 1000000000 - t->phase;
        __uint128_t ns = (needed + t->frequency - 1) / t->frequency;
        if (ns <= INT64_MAX - t->epoch) {
            timer_mod(t->event, t->epoch + (int64_t)ns);
        }
    }
}

static void timer_fire(void *opaque) { timer_update(opaque); }

void nuclei_systimer_clock(NucleiSysTimer *t, uint32_t frequency, bool enabled)
{
    assert(frequency != 0);
    timer_sync(t);
    t->frequency = frequency;
    t->clock_enabled = enabled;
    timer_update(t);
}

bool nuclei_systimer_read(NucleiSysTimer *t, uint32_t offset, uint32_t *value)
{
    switch (offset) {
    case 0: *value = nuclei_systimer_value(t); break;
    case 4: *value = nuclei_systimer_value(t) >> 32; break;
    case 8: *value = t->compare; break;
    case 12: *value = t->compare >> 32; break;
    case 0xff8: *value = t->control; break;
    case 0xffc: *value = t->software; break;
    default: return false;
    }
    return true;
}

bool nuclei_systimer_write(NucleiSysTimer *t, uint32_t offset, uint32_t value)
{
    timer_sync(t);
    uint64_t now = t->value;
    switch (offset) {
    case 0: t->value = (now & ~UINT64_C(0xffffffff)) | value; break;
    case 4: t->value = (now & UINT32_MAX) | ((uint64_t)value << 32); break;
    case 8: t->compare = (t->compare & ~UINT64_C(0xffffffff)) | value; break;
    case 12: t->compare = (t->compare & UINT32_MAX) | ((uint64_t)value << 32); break;
    case 0xff8:
        if (value & ~1u) { return false; }
        t->control = value;
        break;
    case 0xffc:
        t->software = value & 1;
        t->irq(t->opaque, 3, t->software);
        break;
    default: return false;
    }
    timer_update(t);
    return true;
}

void nuclei_systimer_reset(NucleiSysTimer *t, uint32_t frequency)
{
    assert(frequency != 0);
    timer_del(t->event);
    t->value = 0;
    t->compare = UINT64_MAX;
    t->control = 0;
    t->software = false;
    t->frequency = frequency;
    t->phase = 0;
    t->clock_enabled = true;
    t->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

void nuclei_systimer_init(NucleiSysTimer *t, NucleiSysTimerIRQ irq, void *opaque)
{
    t->irq = irq;
    t->opaque = opaque;
    t->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer_fire, t);
}
