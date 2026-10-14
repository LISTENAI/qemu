/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/watchdog/listenai_wdt.h"
#include "system/address-spaces.h"

static void wdt_irq(ListenaiWDT *s)
{
    s->irq(s->opaque, s->expired && (s->control & 4));
}

static void schedule(ListenaiWDT *s)
{
    if (s->clock && s->armed) {
        timer_mod(s->event, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->remaining_ns);
    }
}

void listenai_wdt_clock(ListenaiWDT *s, bool enabled)
{
    if (s->clock == enabled) { return; }
    if (!enabled && timer_pending(s->event)) {
        s->remaining_ns = MAX(0, timer_expire_time_ns(s->event) -
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        timer_del(s->event);
    }
    s->clock = enabled;
    schedule(s);
}

static void wdt_arm(ListenaiWDT *s)
{
    static const unsigned powers[] = { 6, 8, 10, 11, 12, 13, 14, 15,
                                      17, 19, 21, 23, 25, 27, 29, 31 };
    timer_del(s->event);
    s->reset_stage = false;
    s->armed = !!(s->control & 1);
    if (s->armed) {
        uint64_t ticks = UINT64_C(1) << powers[(s->control >> 4) & 15];
        /* The SoC supplies the selected functional clock frequency. */
        s->remaining_ns = (ticks * 1000000000 + s->clock_hz - 1) / s->clock_hz;
        schedule(s);
    }
    wdt_irq(s);
}

static void wdt_expire(void *opaque)
{
    ListenaiWDT *s = opaque;
    s->armed = false;
    if (s->reset_stage) {
        s->reset(s->opaque);
        return;
    }
    s->expired = true;
    wdt_irq(s);
    if (s->control & 8) {
        uint64_t ticks = UINT64_C(1) << (7 + ((s->control >> 8) & 7));
        s->reset_stage = s->armed = true;
        s->remaining_ns = (ticks * 1000000000 + s->clock_hz - 1) / s->clock_hz;
        schedule(s);
    }
}

static uint64_t wdt_read(void *opaque, hwaddr off, unsigned size)
{
    ListenaiWDT *s = opaque;
    if (size == 4 && off == 0x10) { return s->control; }
    if (size == 4 && off == 0x1c) { return s->expired; }
    if (size == 4 && (off == 0x14 || off == 0x18)) { return 0; }
    s->fail(s->opaque, s->base + off, size, false, 0);
    return 0;
}

static void wdt_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenaiWDT *s = opaque;
    if (size != 4) { goto fail; }
    if (off == 0x18 && value == 0x5aa5) { s->unlocked = true; return; }
    if (off == 0x1c && !(value & ~1u)) {
        if (value & 1) { s->expired = false; }
        wdt_irq(s); return;
    }
    /* Require a fresh unlock; other key/write-protection sequences fail. */
    if (!s->unlocked) {
        if (s->ignore_locked_writes && (off == 0x10 || off == 0x14)) {
            return;
        }
        goto fail;
    }
    if (off == 0x10 && !(value & ~0x7fdu)) {
        s->control = value; s->unlocked = false;
        wdt_arm(s); return;
    }
    if (off == 0x14 && value == 0xcafe) {
        s->unlocked = false; wdt_arm(s); return;
    }
fail:
    s->fail(s->opaque, s->base + off, size, true, value);
}

static const MemoryRegionOps ops = {
    .read = wdt_read, .write = wdt_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};
void listenai_wdt_reset(ListenaiWDT *s)
{
    timer_del(s->event);
    s->control = 0;
    s->remaining_ns = 0;
    s->unlocked = s->expired = s->reset_stage = s->armed = false;
    wdt_irq(s);
}
void listenai_wdt_init(ListenaiWDT *s, Object *owner, const char *name,
                      hwaddr base, uint32_t clock_hz)
{
    assert(clock_hz && s->irq && s->reset && s->fail);
    s->base = base; s->clock_hz = clock_hz;
    s->clock = true;
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_expire, s);
    memory_region_init_io(&s->io, owner, &ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), base, &s->io);
}
