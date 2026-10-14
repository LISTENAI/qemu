/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A pair of 16/32-bit countdown timers with a board-supplied source period. */
#include "qemu/osdep.h"
#include "hw/timer/listenai_dual_timer.h"
#include "system/address-spaces.h"

static void irq(ListenAIDualTimer *s)
{
    bool level = false;
    for (unsigned i = 0; i < 2; i++) {
        level |= s->channel[i].pending && (s->channel[i].control & 0x20);
    }
    s->irq(s->opaque, level);
}

static uint64_t tick_ns(ListenAIDualChannel *s)
{
    static const unsigned div[] = {1, 16, 256};
    return s->block->source_period_ns * div[(s->control >> 2) & 3];
}

static uint32_t mask(ListenAIDualChannel *s)
{
    return s->control & 2 ? UINT32_MAX : UINT16_MAX;
}

static void sync_time(ListenAIDualChannel *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->control & 0x80) {
        uint64_t tick = tick_ns(s);
        uint64_t progress = now - s->epoch + s->phase;
        uint64_t ticks = progress / tick;
        s->phase = progress % tick;
        if (ticks <= s->value) { s->value -= ticks; }
        else {
            ticks -= (uint64_t)s->value + 1;
            s->pending = true;
            if (s->control & 1) {
                s->control &= ~0x80u;
                s->value = s->phase = 0;
            } else {
                uint32_t reload = s->control & 0x40 ? s->load & mask(s) : mask(s);
                s->value = reload - ticks % ((uint64_t)reload + 1);
            }
            irq(s->block);
        }
    }
    s->epoch = now;
}

static void arm(ListenAIDualChannel *s)
{
    timer_del(s->event);
    if (s->control & 0x80) {
        uint64_t delay = ((uint64_t)s->value + 1) * tick_ns(s) - s->phase;
        if (delay <= INT64_MAX - s->epoch) { timer_mod(s->event, s->epoch + delay); }
    }
}

static void expire(void *opaque)
{
    ListenAIDualChannel *s = opaque;
    sync_time(s); arm(s);
}

static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    ListenAIDualTimer *s = opaque;
    if (size != 4 || (off & 3) || off >= 0x40) { goto invalid; }
    ListenAIDualChannel *c = &s->channel[off / 0x20];
    sync_time(c);
    switch (off & 0x1f) {
    case 0: case 0x18: return c->load;
    case 4: return c->value;
    case 8: return c->control;
    case 12: return 0;
    case 16: return c->pending;
    case 20: return c->pending && (c->control & 0x20);
    }
invalid:
    s->fault(s->opaque, s->address + off, size, false, 0);
    abort();
}

static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenAIDualTimer *s = opaque;
    if (size != 4 || (off & 3) || off >= 0x40) { goto invalid; }
    ListenAIDualChannel *c = &s->channel[off / 0x20];
    sync_time(c);
    switch (off & 0x1f) {
    case 0: c->load = value; c->value = value & mask(c); c->phase = 0; break;
    case 8:
        if (value & ~0xffu || ((value >> 2) & 3) == 3) { goto invalid; }
        c->control = value; c->value &= mask(c); c->phase %= tick_ns(c); break;
    case 12: c->pending = false; break;
    case 0x18: c->load = value; break; /* Do not alter the active countdown. */
    default: goto invalid;
    }
    irq(s); arm(c); return;
invalid:
    s->fault(s->opaque, s->address + off, size, true, value);
    abort();
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_dual_timer_reset(ListenAIDualTimer *s)
{
    for (unsigned j = 0; j < 2; j++) {
        ListenAIDualChannel *c = &s->channel[j];
        timer_del(c->event);
        c->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        c->control = c->phase = c->load = 0;
        c->value = UINT16_MAX;
        c->pending = false;
    }
    irq(s);
}

void listenai_dual_timer_init(ListenAIDualTimer *s, Object *owner,
                              const char *name, hwaddr address,
                              uint64_t source_period_ns,
                              ListenAIDualTimerIRQ irq,
                              ListenAIDualTimerFault fault, void *opaque)
{
    assert(source_period_ns > 0 && source_period_ns <= INT64_MAX / 256);
    s->address = address;
    s->source_period_ns = source_period_ns;
    s->irq = irq;
    s->fault = fault;
    s->opaque = opaque;
    for (unsigned j = 0; j < 2; j++) {
        ListenAIDualChannel *c = &s->channel[j];
        c->block = s;
        c->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, expire, c);
    }
    memory_region_init_io(&s->io, owner, &ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), address, &s->io);
}
