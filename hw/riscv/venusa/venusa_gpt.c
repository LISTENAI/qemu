/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA two-channel 16-bit / split 8-bit general-purpose timer. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/address-spaces.h"
#include "qemu/host-utils.h"

#define BASE 0x44000000
#define IRQ_MASK 0x7bf0u

static Int128 multiply(uint64_t a, uint64_t b)
{
    uint64_t lo, hi;
    mulu64(&lo, &hi, a, b);
    return int128_make128(lo, hi);
}

static void irq(VenusAGPT *s)
{
    venusa_irq(s->soc, 34, (s->pending & ~s->mask) != 0);
}

static unsigned mode(VenusAGPTChannel *c)
{
    return (c->control >> 3) & 7;
}

static unsigned run_mode(VenusAGPTChannel *c, unsigned timer)
{
    return (c->gpt->count_control >> (14 + c->index * 4 + timer * 2)) & 3;
}

static unsigned direction(VenusAGPTChannel *c, unsigned timer)
{
    return (c->gpt->count_control >> (6 + c->index * 4 + timer * 2)) & 3;
}

static unsigned limit(VenusAGPTChannel *c, unsigned timer)
{
    if (run_mode(c, timer) == 2) { return mode(c) == 1 ? 65535 : 255; }
    return mode(c) == 1 ? c->shadow : (c->shadow >> (8 * timer)) & 255;
}

static bool enabled(VenusAGPTChannel *c, unsigned timer)
{
    return c->gpt->clock_enabled && c->channel_on && c->counter_on &&
           !(c->clock & (1u << 21)) && !(c->control & (1u << 13)) &&
           !c->done[timer] && (mode(c) == 2 || (mode(c) == 1 && !timer)) &&
           (c->gpt->count_control & (1u << (24 + timer * 2 + c->index)));
}

static void rate(VenusAGPTChannel *c)
{
    uint64_t n = 24000000, d = c->gpt->soc->gpt_t0_div;
    unsigned select = (c->clock >> 19) & 3;
    if (select == 2) {
        VenusASoC *soc = c->gpt->soc;
        venusa_hclk_ratio(soc, &n, &d);
        n *= soc->pclk_n;
        d *= soc->pclk_m;
    } else if (select) {
        if (c->channel_on && c->counter_on) {
            venusa_fail(c->gpt->soc, BASE + 4 + c->index * 4, 4, true, c->clock);
        }
        n = 0;
    }
    uint64_t a = n, b = d;
    while (b) { uint64_t r = a % b; a = b; b = r; }
    if (a) { n /= a; d /= a; }
    d *= (c->prediv + 1u) << c->divider;
    if (c->rate_d && c->rate_d != d) {
        /* Preserve the fractional counter tick across source-clock changes. */
        Int128 old = int128_make64(c->rate_d);
        uint64_t whole = int128_get64(int128_divu(c->phase, old));
        uint64_t rest = int128_get64(int128_remu(c->phase, old));
        c->phase = int128_add(multiply(whole, d),
                             int128_divu(multiply(rest, d), old));
    }
    c->rate_n = n;
    c->rate_d = d;
}

static uint64_t terminal_ticks(VenusAGPTChannel *c, unsigned timer)
{
    unsigned top = limit(c, timer);
    return c->descending[timer] ? (uint64_t)c->count[timer] + 1 :
                                 (uint64_t)top - MIN(c->count[timer], top) + 1;
}

static void advance(VenusAGPTChannel *c, unsigned timer, uint64_t ticks)
{
    unsigned top = limit(c, timer);
    while (ticks && !c->done[timer]) {
        uint64_t terminal = terminal_ticks(c, timer);
        if (ticks < terminal) {
            if (c->descending[timer]) { c->count[timer] -= ticks; }
            else { c->count[timer] += ticks; }
            return;
        }
        ticks -= terminal;
        c->gpt->pending |= 1u << (6 + 2 * timer + c->index);
        if (run_mode(c, timer) == 0) {
            c->count[timer] = c->descending[timer] ? 0 : top;
            c->done[timer] = true;
        } else {
            c->descending[timer] = direction(c, timer) == 1;
            c->count[timer] = c->descending[timer] ? top : 0;
            /* Repeated full cycles latch the same raw interrupt. */
            uint64_t period = (uint64_t)top + 1;
            ticks %= period;
        }
    }
}

static void sync_time(VenusAGPTChannel *c)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (c->rate_n && (enabled(c, 0) || enabled(c, 1))) {
        Int128 den = multiply(c->rate_d, UINT64_C(1000000000));
        Int128 work = int128_add(c->phase, multiply(now - c->epoch, c->rate_n));
        uint64_t ticks = int128_get64(int128_divu(work, den));
        c->phase = int128_remu(work, den);
        for (unsigned timer = 0; timer < 2; timer++) {
            if (enabled(c, timer)) { advance(c, timer, ticks); }
        }
        irq(c->gpt);
    }
    c->epoch = now;
}

static void arm(VenusAGPTChannel *c)
{
    uint64_t ticks = UINT64_MAX;
    timer_del(c->event);
    for (unsigned timer = 0; timer < 2; timer++) {
        if (enabled(c, timer)) { ticks = MIN(ticks, terminal_ticks(c, timer)); }
    }
    if (ticks != UINT64_MAX && c->rate_n) {
        Int128 work = int128_sub(multiply(ticks * UINT64_C(1000000000), c->rate_d), c->phase);
        work = int128_add(work, int128_make64(c->rate_n - 1));
        Int128 delay = int128_divu(work, int128_make64(c->rate_n));
        if (int128_le(delay, int128_make64(INT64_MAX - c->epoch))) {
            timer_mod(c->event, c->epoch + int128_get64(delay));
        }
    }
}

static void expire(void *opaque)
{
    VenusAGPTChannel *c = opaque;
    sync_time(c);
    arm(c);
}

static void start(VenusAGPTChannel *c)
{
    if (mode(c) != 1 && mode(c) != 2) {
        venusa_fail(c->gpt->soc, BASE + 12 + c->index * 4, 4, true, c->control);
    }
    if (c->control & 7) {
        venusa_fail(c->gpt->soc, BASE + 12 + c->index * 4, 4, true, c->control);
    }
    for (unsigned timer = 0; timer < 2; timer++) {
        if (direction(c, timer) >= 2) {
            venusa_fail(c->gpt->soc, BASE + 0x1c, 4, true, c->gpt->count_control);
        }
        if (run_mode(c, timer) != 3 || !c->initialized[timer]) {
            c->descending[timer] = direction(c, timer) == 1;
            c->count[timer] = c->descending[timer] ? limit(c, timer) : 0;
            c->done[timer] = false;
            c->initialized[timer] = true;
        }
    }
    c->phase = int128_zero();
    c->counter_on = true;
    rate(c);
}

static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    VenusAGPT *s = opaque;
    if (size != 4 || (off & 3)) { goto fail; }
    for (unsigned i = 0; i < 2; i++) { sync_time(&s->channel[i]); }
    if (off == 0) { return 0x10; }
    if (off == 4 || off == 8) { return s->channel[off / 4 - 1].clock; }
    if (off == 12 || off == 16) { return s->channel[off / 4 - 3].control; }
    if (off == 20 || off == 24) {
        VenusAGPTChannel *c = &s->channel[off / 4 - 5];
        uint32_t count = c->count[0] | (mode(c) == 2 ? c->count[1] << 8 : 0);
        return (count << 16) | c->reload;
    }
    if (off == 0x1c) { return s->count_control; }
    if (off == 0x80) { return s->mask | ((s->pending & ~s->mask) << 16); }
    if (off == 0x84) { return s->pending; }
    if (off == 0xa0) { return 0; }
fail:
    venusa_fail(s->soc, BASE + off, size, false, 0);
}

static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusAGPT *s = opaque;
    if (size != 4 || (off & 3)) { goto fail; }
    for (unsigned i = 0; i < 2; i++) { sync_time(&s->channel[i]); }
    if (off == 4 || off == 8) {
        if (value & ~0x1ffffffu) { goto fail; }
        VenusAGPTChannel *c = &s->channel[off / 4 - 1];
        c->clock = value & 0x7fffff;
        if (value & (1u << 23)) { c->prediv = value & 65535; }
        if (value & (1u << 24)) { c->divider = (value >> 16) & 7; }
        if (value & (3u << 23)) { c->phase = int128_zero(); }
        rate(c);
    } else if (off == 12 || off == 16) {
        if (value & ~0x0fffe03fu) { goto fail; }
        VenusAGPTChannel *c = &s->channel[off / 4 - 3];
        c->control = value & ~0xc000u;
        if (value & 0x4000) { c->channel_on = false; }
        if (value & 0x8000) { c->channel_on = true; }
    } else if (off == 20 || off == 24) {
        s->channel[off / 4 - 5].reload = value;
    } else if (off == 0x1c) {
        /* Synchronized channels require pad-level triggering support. */
        if (value & 0xf0000000u) { goto fail; }
        s->count_control = value & 0x0f3fffc0;
        for (unsigned i = 0; i < 2; i++) {
            VenusAGPTChannel *c = &s->channel[i];
            if (value & (1u << (4 + i))) {
                c->count[0] = c->count[1] = 0;
                c->phase = int128_zero();
            }
            if (value & (1u << (2 + i))) { c->counter_on = false; }
            if (value & (1u << i)) { start(c); }
        }
    } else if (off == 0x80) {
        s->mask = value & IRQ_MASK;
    } else if (off == 0x84) {
        s->pending &= ~(value >> 16);
    } else if (off == 0xa0) {
        if (value & ~15u) { goto fail; }
        for (unsigned i = 0; i < 2; i++) {
            if (value & (5u << i)) {
                /* Deferred live shadow transfer requires PWM boundary wiring. */
                if ((value & (1u << i)) &&
                    (enabled(&s->channel[i], 0) || enabled(&s->channel[i], 1))) {
                    goto fail;
                }
                s->channel[i].shadow = s->channel[i].reload;
            }
        }
    } else { goto fail; }
    irq(s);
    for (unsigned i = 0; i < 2; i++) { arm(&s->channel[i]); }
    return;
fail:
    venusa_fail(s->soc, BASE + off, size, true, value);
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void venusa_gpt_clock(VenusASoC *soc, bool enabled)
{
    VenusAGPT *s = &soc->gpt;
    for (unsigned i = 0; i < 2; i++) { sync_time(&s->channel[i]); }
    s->clock_enabled = enabled;
    for (unsigned i = 0; i < 2; i++) { rate(&s->channel[i]); arm(&s->channel[i]); }
}

void venusa_gpt_reset(VenusASoC *soc)
{
    VenusAGPT *s = &soc->gpt;
    s->count_control = s->pending = 0;
    s->mask = IRQ_MASK;
    for (unsigned i = 0; i < 2; i++) {
        VenusAGPTChannel *c = &s->channel[i];
        timer_del(c->event);
        c->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        c->phase = int128_zero();
        c->clock = 1u << 21;
        c->control = 1u << 13;
        c->reload = c->shadow = c->count[0] = c->count[1] = c->prediv = 0;
        c->divider = 0;
        c->channel_on = c->counter_on = false;
        c->done[0] = c->done[1] = c->descending[0] = c->descending[1] = false;
        c->initialized[0] = c->initialized[1] = false;
        rate(c);
    }
    irq(s);
}

void venusa_gpt_init(VenusASoC *soc)
{
    VenusAGPT *s = &soc->gpt;
    s->soc = soc;
    for (unsigned i = 0; i < 2; i++) {
        s->channel[i].gpt = s;
        s->channel[i].index = i;
        s->channel[i].event = timer_new_ns(QEMU_CLOCK_VIRTUAL, expire, &s->channel[i]);
    }
    memory_region_init_io(&s->io, OBJECT(soc), &ops, s, "venusa-gpt", 0x1000);
    memory_region_add_subregion(get_system_memory(), BASE, &s->io);
}
