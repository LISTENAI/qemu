/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Six-channel VenusA DMA2D generation: normal transfers and shadow queue. */
#include "qemu/osdep.h"
#include "hw/dma/venusa_dma2d.h"
#include "system/address-spaces.h"
#include "qemu/host-utils.h"

static void fail(VenusADMA2D *s, hwaddr off, bool write, uint32_t v)
{
    s->cb.fail(s->cb.opaque, s->base + off, 4, write, v);
}
static uint32_t status(VenusADMA2D *s, unsigned ch)
{
    uint32_t mask = (s->regs[(0x240 + ch * 4) / 4] & ~1u) |
                    s->channels[ch].working.block_mask;
    return s->channels[ch].raw & ~mask & 31;
}
static void irq(VenusADMA2D *s, unsigned ch)
{
    s->cb.irq(s->cb.opaque, ch,
              status(s, ch) && s->channels[ch].working.irq_enable);
}
static void activate(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    if (!c->active && c->pending && (s->regs[ch] & 1)) {
        c->working = c->queued;
        c->transferred = 0;
        c->src_cursor = c->working.src;
        c->dst_cursor = c->working.dst;
        c->pending = false;
        c->active = true;
        c->raw |= 4;
        irq(s, ch);
    }
}
static bool ready(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    if (!c->active || !(s->regs[ch] & 1)) { return false; }
    unsigned mode = (c->working.control >> 1) & 3;
    return mode == 2 || s->cb.request(s->cb.opaque, c->working.control >> 28,
                                     c->dst_cursor, true);
}
static Int128 multiply(uint64_t a, uint64_t b)
{
    uint64_t lo, hi;
    mulu64(&lo, &hi, a, b);
    return int128_make128(lo, hi);
}
static void sync_clock(VenusADMA2D *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->clock_enabled && s->clock_n && !int128_eq(s->clock_work, int128_zero())) {
        s->clock_work = int128_sub(s->clock_work,
                                  multiply(now - s->event_epoch, s->clock_n));
    }
    s->event_epoch = now;
}
static void arm(VenusADMA2D *s)
{
    Int128 work = int128_add(s->clock_work, int128_make64(s->clock_n - 1));
    Int128 wait = int128_divs(work, int128_make64(s->clock_n));
    if (!int128_le(wait, int128_make64(INT64_MAX - s->event_epoch))) {
        fail(s, 0, true, 0); return;
    }
    timer_mod(s->event, s->event_epoch + MAX(INT64_C(1), int128_get64(wait)));
}
void venusa_dma2d_request(VenusADMA2D *s)
{
    if (s->servicing || !s->clock_enabled || !s->clock_n || timer_pending(s->event)) {
        return;
    }
    for (unsigned i = 0; i < VENUSA_DMA2D_CHANNELS; i++) {
        activate(s, i);
        if (ready(s, i)) {
            if (int128_eq(s->clock_work, int128_zero())) {
                s->clock_work = multiply(UINT64_C(2000000000), s->clock_d);
            }
            s->event_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            arm(s);
            return;
        }
    }
}
static uint32_t next_address(uint32_t a, unsigned mode, unsigned bytes)
{
    return mode == 0 ? a + bytes : mode == 1 ? a - bytes : a;
}
static void transfer(void *opaque)
{
    VenusADMA2D *s = opaque;
    if (!s->clock_enabled) { return; }
    sync_clock(s);
    s->servicing = true;
    /* One granted burst owns the bus. Zero is the highest priority. */
    unsigned chosen = VENUSA_DMA2D_CHANNELS, priority = 4;
    for (unsigned ch = 0; ch < VENUSA_DMA2D_CHANNELS; ch++) {
        unsigned prio = (s->channels[ch].working.control >> 19) & 3;
        if (ready(s, ch) && prio < priority) { chosen = ch; priority = prio; }
    }
    if (chosen < VENUSA_DMA2D_CHANNELS) {
        unsigned ch = chosen;
        VenusADMA2DChannel *c = &s->channels[ch];
        VenusADMA2DDescriptor *w = &c->working;
        unsigned width = 1u << ((w->control >> 3) & 3);
        unsigned mode = (w->control >> 1) & 3;
        unsigned burst[] = { 1, 4, 8, 16 };
        unsigned beats = burst[(w->control >> 15) & 3];
        /* A granted burst remains granted after the watermark deasserts. */
        for (unsigned b = 0; b < beats && c->active; b++) {
            uint8_t bytes[4];
            if (!s->cb.memory_allowed(s->cb.opaque, c->src_cursor, width, false) ||
                (mode == 2 && !s->cb.memory_allowed(s->cb.opaque, c->dst_cursor, width, true))) {
                fail(s, 0xa0 + ch * 4, true, w->src); break;
            }
            if (address_space_read(&address_space_memory, c->src_cursor, MEMTXATTRS_UNSPECIFIED,
                                   bytes, width) != MEMTX_OK ||
                address_space_write(&address_space_memory, c->dst_cursor, MEMTXATTRS_UNSPECIFIED,
                                    bytes, width) != MEMTX_OK) {
                fail(s, 0xa0 + ch * 4, true, w->src); break;
            }
            c->src_cursor = next_address(c->src_cursor, (w->control >> 8) & 3, width);
            c->dst_cursor = next_address(c->dst_cursor, (w->control >> 10) & 3, width);
            c->transferred += width;
            if (c->transferred == w->length) {
                c->active = false;
                c->raw |= 1;
                activate(s, ch);
                irq(s, ch);
                /* Preserve a hardware-visible block boundary for callbacks. */
                break;
            }
        }
    }
    s->servicing = false;
    for (unsigned ch = 0; ch < VENUSA_DMA2D_CHANNELS; ch++) {
        if (ready(s, ch)) {
            /* Bounded bus service; preserve fractional HCLK deadlines. */
            s->clock_work = int128_add(s->clock_work,
                                      multiply(UINT64_C(32000000000), s->clock_d));
            arm(s);
            return;
        }
    }
    s->clock_work = int128_zero();
}
static void queue(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    uint32_t ctrl = s->regs[ch];
    unsigned mode = (ctrl >> 1) & 3, width = (ctrl >> 3) & 3;
    uint32_t src = s->regs[(0xa0 + 4 * ch) / 4];
    uint32_t dst = s->regs[(0xc0 + 4 * ch) / 4];
    uint32_t len = s->regs[(0xe0 + 4 * ch) / 4];
    /* Image, peripheral flow-control, gather/scatter and triggering require
     * distinct pipelines; never acknowledge them as a normal copy. */
    uint32_t unsupported = (1u << 12) | (1u << 17) | (1u << 18) | (0x7fu << 21);
    if (c->pending || width > 2 || width != ((ctrl >> 5) & 3) ||
        ((ctrl >> 8) & 3) == 3 || ((ctrl >> 10) & 3) == 3 ||
        (mode != 1 && mode != 2) || (ctrl & unsupported) ||
        !len || len > 0xffffff || ((src | dst | len) & ((1u << width) - 1)) ||
        (mode == 1 && (((ctrl >> 10) & 3) != 2 || width != 2)) ||
        s->regs[(0x100 + ch * 4) / 4] || s->regs[(0x300 + ch * 4) / 4]) {
        fail(s, ch * 4, true, ctrl); return;
    }
    c->queued = (VenusADMA2DDescriptor){ctrl, src, dst, len,
        s->regs[(0x220 + 4 * ch) / 4], s->regs[(0x240 + 4 * ch) / 4] & 1};
    c->pending = true;
    if (s->clock_enabled) { activate(s, ch); }
    venusa_dma2d_request(s);
}
static void cancel(VenusADMA2D *s, unsigned ch)
{
    VenusADMA2DChannel *c = &s->channels[ch];
    c->active = c->pending = false;
    c->raw |= 16;
    irq(s, ch);
    for (unsigned i = 0; i < VENUSA_DMA2D_CHANNELS; i++) {
        if (s->channels[i].active) { return; }
    }
    timer_del(s->event);
    s->clock_work = int128_zero();
}
static uint64_t read_reg32(void *opaque, hwaddr off, unsigned size)
{
    VenusADMA2D *s = opaque;
    unsigned ch = (off & 31) / 4, bank = off & ~31;
    if (size != 4 || (off & 3)) { fail(s, off, false, 0); return 0; }
    if (ch < 6 && (bank <= 0x160 || (bank >= 0x200 && bank <= 0x340))) {
        VenusADMA2DChannel *c = &s->channels[ch];
        switch (bank) {
        case 0x200: return c->pending;
        case 0x260: return 0;
        case 0x280: return c->raw;
        case 0x2a0: return status(s, ch);
        case 0x320: return c->active ? c->working.length - c->transferred : 0;
        case 0x340: return !c->active;
        default: return s->regs[off / 4];
        }
    }
    if (ch < 6 && bank >= 0x380 && bank <= 0x4a0) {
        VenusADMA2DChannel *c = &s->channels[ch];
        if (bank == 0x380) { return c->working.src; }
        if (bank == 0x3a0) { return c->working.dst; }
        if (bank == 0x460) { return ((c->working.control >> 8) & 3) << 24 |
                                   ((c->working.control >> 13) & 3) << 22; }
        if (bank == 0x480) { return ((c->working.control >> 10) & 3) << 24 |
                                   ((c->working.control >> 15) & 3) << 22; }
        if (bank == 0x440) { return c->working.length | c->working.block_mask << 30 |
                                  c->working.irq_enable << 31; }
        return 0;
    }
    if (off == 0x184 || off == 0x1a0 || off == 0x1c0) { return 0; }
    if (off == 0x360 || off == 0x364 || off == 0x4dc) { return 0; }
    if (off >= 0x4c0 && off <= 0x500) { return s->regs[off / 4]; }
    fail(s, off, false, 0); return 0;
}
static void write_reg32(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusADMA2D *s = opaque;
    uint32_t v = value;
    unsigned ch = (off & 31) / 4, bank = off & ~31;
    if (size != 4 || (off & 3)) { fail(s, off, true, v); return; }
    if (ch < 6 && (bank <= 0x160 || (bank >= 0x200 && bank <= 0x300))) {
        VenusADMA2DChannel *c = &s->channels[ch];
        bool shadow_write = bank == 0xa0 || bank == 0xc0 || bank == 0xe0 ||
                            bank == 0x220 || (bank == 0x240 &&
                            ((v ^ s->regs[off / 4]) & 1));
        if (c->pending && shadow_write) { fail(s, off, true, v); return; }
        switch (bank) {
        case 0x200:
            if (!v) { c->pending = false; return; }
            if (v != 1) { fail(s, off, true, v); return; }
            queue(s, ch); return;
        case 0x220:
            if (v > 1) { fail(s, off, true, v); return; }
            break;
        case 0x240:
            if (v & ~31u) { fail(s, off, true, v); return; }
            break;
        case 0x260:
            if (v & ~31u) { fail(s, off, true, v); return; }
            c->raw &= ~v; irq(s, ch); return;
        case 0x280: case 0x2a0: fail(s, off, true, v); return;
        default:
            if (bank >= 0x100 && bank != 0x200 && v) {
                fail(s, off, true, v); return;
            }
            break;
        }
        s->regs[off / 4] = v;
        irq(s, ch);
        venusa_dma2d_request(s);
        return;
    }
    if (off == 0x360) {
        if (v & ~63u) { fail(s, off, true, v); return; }
        for (unsigned i = 0; i < 6; i++) { if (v & (1u << i)) { cancel(s, i); } }
        return;
    }
    if (off == 0x364 && v <= 1) { if (v) { venusa_dma2d_reset(s); } return; }
    if ((off == 0x184 || off == 0x1a0 || off == 0x1c0) && !v) { return; }
    if ((off == 0x4c4 || off == 0x4c8) && !(v & ~0x100u)) { return; }
    if (off == 0x4dc && !(v & ~15u)) { return; }
    if ((off == 0x4c0 && !(v & ~3u)) || (off == 0x4e8 && !(v & ~0x33u)) ||
        (off == 0x4d4 && v <= 1) || (off == 0x4d8 && !(v & ~15u)) ||
        (off == 0x4ec && !v) || (off == 0x500 && !v)) {
        s->regs[off / 4] = v; return;
    }
    fail(s, off, true, v);
}
static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    VenusADMA2D *s = opaque;
    if ((size != 1 && size != 2 && size != 4) || (off & 3) + size > 4) {
        s->cb.fail(s->cb.opaque, s->base + off, size, false, 0); return 0;
    }
    uint64_t value = read_reg32(s, off & ~3, 4);
    return (value >> (8 * (off & 3))) & (UINT32_MAX >> (32 - size * 8));
}
static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusADMA2D *s = opaque;
    if ((size != 1 && size != 2 && size != 4) || (off & 3) + size > 4) {
        s->cb.fail(s->cb.opaque, s->base + off, size, true, value); return;
    }
    hwaddr aligned = off & ~3;
    unsigned bank = aligned & ~31;
    uint32_t mask = (UINT32_MAX >> (32 - size * 8)) << (8 * (off & 3));
    uint32_t v = (value << (8 * (off & 3))) & mask;
    bool pulse = bank == 0x200 || bank == 0x260 || aligned == 0x360 ||
                 aligned == 0x364 || aligned == 0x4dc;
    if (size != 4 && !pulse) { v |= read_reg32(s, aligned, 4) & ~mask; }
    write_reg32(s, aligned, v, 4);
}
static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};
void venusa_dma2d_reset(VenusADMA2D *s)
{
    timer_del(s->event);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->channels, 0, sizeof(s->channels));
    s->clock_work = int128_zero();
    s->event_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned ch = 0; ch < 6; ch++) {
        s->regs[ch] = 0x0019e040;
        s->regs[(0x240 + ch * 4) / 4] = 30;
        irq(s, ch);
    }
    s->regs[0x4c0 / 4] = 1;
    s->regs[0x4e8 / 4] = 0x11;
    s->regs[0x4d8 / 4] = 15;
    s->cb.irq(s->cb.opaque, 6, false);
}
void venusa_dma2d_clock(VenusADMA2D *s, bool enabled, uint64_t n, uint64_t d)
{
    sync_clock(s);
    timer_del(s->event);
    if (!d) { fail(s, 0, true, 0); return; }
    if (s->clock_d && s->clock_d != d) {
        Int128 old = int128_make64(s->clock_d);
        uint64_t whole = int128_get64(int128_divu(s->clock_work, old));
        uint64_t rest = int128_get64(int128_remu(s->clock_work, old));
        s->clock_work = int128_add(multiply(whole, d),
                                  int128_divu(multiply(rest, d), old));
    }
    s->clock_enabled = enabled; s->clock_n = n; s->clock_d = d;
    venusa_dma2d_request(s);
}
void venusa_dma2d_init(VenusADMA2D *s, Object *owner, hwaddr base, VenusADMA2DCallbacks cb)
{
    s->base = base; s->cb = cb;
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, transfer, s);
    memory_region_init_io(&s->io, owner, &ops, s, "venusa-dma2d", 0x1000);
    venusa_dma2d_reset(s);
}
