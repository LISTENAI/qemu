/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AON 24-bit watchdog with ideal RC32k timing and protected commands. */
#include "qemu/osdep.h"
#include "hw/timer/listenai_aon.h"
#include "system/address-spaces.h"


static void irq(ListenaiAONWDT *s)
{
    s->callbacks.irq(s->callbacks.opaque, (s->cause & 1) && (s->control & 2));
}

static void arm(ListenaiAONWDT *s)
{
    timer_del(s->event);
    if (s->enabled) {
        timer_mod(s->event, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  ((uint64_t)s->load + 1) * 31250);
    }
}

static void expire(void *opaque)
{
    ListenaiAONWDT *s = opaque;
    if (s->control & 8) {
        s->callbacks.reset(s->callbacks.opaque, s->reset_pmu);
        return;
    }
    s->cause |= 0x10001;
    irq(s);
}

static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    ListenaiAONWDT *s = opaque;
    if (size != 4 || (off & 3)) { goto invalid; }
    switch (off) {
    case 0: return s->control | (s->enabled ? 0x20 : 0) |
                   (s->start_locked ? 0x140 : 0) | (s->stop_locked ? 0x80 : 0);
    case 4: return s->stop_locked;
    case 8: return s->start_locked;
    case 12: return s->reset_pmu;
    case 16: return s->load;
    case 20: return 0;
    case 24: return s->cause;
    }
invalid:
    s->callbacks.fail(s->callbacks.opaque, s->base + off, size, false, 0);
    return 0;
}

static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenaiAONWDT *s = opaque;
    if (size != 4 || (off & 3)) { goto invalid; }
    switch (off) {
    case 0:
        s->control = value & 0xa;
        if ((value & 1) && !s->start_locked) { s->enabled = true; arm(s); }
        if ((value & 4) && !s->stop_locked) { s->enabled = false; arm(s); }
        if ((value & 16) && !s->start_locked) { arm(s); }
        irq(s); return;
    case 4:
        if (value == 0xdeadface) { s->stop_locked = true; }
        else if (value == 0xbabebeef) { s->stop_locked = false; }
        return;
    case 8:
        if (value == 0xbadbee01) { s->start_locked = true; }
        else if (value == 0xbadbee00) { s->start_locked = false; }
        return;
    case 12:
        if (value == 0x5856e201) { s->reset_pmu = true; }
        else if (value == 0x5856e200) { s->reset_pmu = false; }
        return;
    case 16:
        if (!s->start_locked) { s->load = value & 0xffffff; }
        return;
    case 20:
        if (value & 1) { s->cause = 0; irq(s); }
        return;
    case 24: return; /* Read-only cause. */
    }
invalid:
    s->callbacks.fail(s->callbacks.opaque, s->base + off, size, true, value);
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_aon_wdt_reset(ListenaiAONWDT *s, uint32_t cause)
{
    timer_del(s->event);
    s->control = s->load = 0;
    s->cause = cause;
    s->stop_locked = s->start_locked = s->reset_pmu = s->enabled = false;
    irq(s);
}

void listenai_aon_wdt_init(ListenaiAONWDT *s, Object *owner,
                          const char *name, hwaddr base,
                          ListenaiAONCallbacks callbacks)
{
    assert(callbacks.fail && callbacks.irq && callbacks.reset);
    s->base = base; s->callbacks = callbacks;
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, expire, s);
    memory_region_init_io(&s->io, owner, &ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), base, &s->io);
}
