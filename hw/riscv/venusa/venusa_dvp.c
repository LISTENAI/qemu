/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA IMAGE_VIC digital receiver. External pixels enter only at PCLK edges. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/address-spaces.h"
#define R(o) d->regs[(o) / 4]
#define BASE 0x42000800
#define FIFO 0x42001000

static void signals(VenusADVP *d)
{
    venusa_irq(d->soc, 57, !!(R(0x34) & ~R(0x24) & 0xfff));
    venusa_dvp_dma_update(d->soc);
}
void venusa_dvp_dma_update(VenusASoC *s)
{
    VenusADVP *d = &s->dvp;
    uint32_t mux = s->syscfg[0xa0 / 4];
    if (!(mux & (1u << 3))) { return; }
    bool route = (mux & (1u << 15)) && (mux & (1u << 19));
    if (d->count && (R(0x20) & 0x80)) { d->request = true; }
    if (d->count >= MAX(1, R(0x20) & 0x7f) && (R(0x20) & 0x100)) {
        d->request = true;
    }
    if (!d->count) { d->request = false; }
    listenai_dma_request(&s->dma, 3, route && d->clock && (R(0x1c) & 1) && d->request);
}
static unsigned bytes_per_pixel(VenusADVP *d)
{
    switch (R(0x18)) {
    case 0: case 0xc: return 2;
    case 0x14: return 1;
    default: venusa_fail(d->soc, BASE + 0x18, 4, true, R(0x18));
    }
}
static void push(VenusADVP *d, uint8_t byte)
{
    d->word |= (uint32_t)byte << (d->packed++ * 8);
    if (d->packed != 4) { return; }
    if (d->count == 64) { R(0x34) |= 8; d->overflow++; }
    else {
        d->fifo[(d->head + d->count++) % 64] = d->word;
        R(0x34) |= 1;
        if (d->count >= MAX(1, R(0x20) & 0x7f)) { R(0x34) |= 2; }
        if (d->count == 64) { R(0x34) |= 0x20; }
    }
    d->word = d->packed = 0;
    signals(d);
}
static void sample(VenusADVP *d)
{
    if (!d->clock || !(R(0x1c) & 1)) { return; }
    bool vsync = !!d->input[0] ^ !!(R(0x10) & 8);
    bool hsync = !!d->input[1] ^ !!(R(0x10) & 4);
    if (vsync && !d->last_vsync) {
        if (d->frame) {
            R(0x34) |= 0x40;
            if (d->line < R(0xc) + R(4)) { R(0x34) |= 0x100; }
        }
        d->frame = true;
        d->line = d->byte = d->packed = d->word = 0;
        d->frames++;
        R(0x34) |= 0x80;
    }
    if (d->last_hsync && !hsync && d->frame) {
        unsigned bpp = bytes_per_pixel(d);
        if (d->byte < (R(8) + R(0)) * bpp) { R(0x34) |= 0x800; }
        d->line++; d->byte = 0;
        if (d->line == R(0xc) + R(4)) { R(0x34) |= 0x40; }
    }
    d->last_vsync = vsync;
    d->last_hsync = hsync;
    if (d->frame && !vsync && hsync) {
        unsigned bpp = bytes_per_pixel(d);
        unsigned first = R(8) * bpp, end = first + R(0) * bpp;
        if (d->line >= R(0xc) && d->line < R(0xc) + R(4) &&
            d->byte >= first && d->byte < end) {
            push(d, d->input[3]); d->samples++;
            if (d->line + 1 == R(0xc) + R(4) && d->byte + 1 == end) {
                R(0x34) |= 0x200;
            }
        }
        d->byte++;
    }
    signals(d);
}
static void input(void *opaque, int pin, int value)
{
    VenusADVP *d = &((VenusASoC *)opaque)->dvp;
    if ((pin == 3 && (value < 0 || value > 255)) || (pin != 3 && value != 0 && value != 1)) {
        venusa_fail(d->soc, BASE, 4, true, value);
    }
    int previous = d->input[pin]; d->input[pin] = value;
    if (pin == 2 && previous != value && value == !(R(0x10) & 1)) { sample(d); }
}
static uint64_t dvp_read(void *opaque, hwaddr off, unsigned size)
{
    VenusADVP *d = opaque;
    if (size != 4 || off % 4 || off > 0x40) { venusa_fail(d->soc, BASE + off, size, false, 0); }
    switch (off) {
    case 0x14: case 0x28: return 0;
    case 0x2c: return !!(R(0x34) & ~R(0x24) & 0xfff);
    case 0x30: return R(0x34) & ~R(0x24) & 0xfff;
    case 0x38: return ((d->byte / bytes_per_pixel(d)) & 0x1fff) << 16 | (d->line & 0x1fff);
    case 0x3c: if (d->frame) { venusa_fail(d->soc, BASE + off, size, false, 0); } return 1;
    case 0x40: return (d->byte & 0x1fff) << 16 | (d->line & 0x1fff);
    default: return R(off);
    }
}
static uint64_t fifo_read(void *opaque, hwaddr off, unsigned size)
{
    VenusADVP *d = opaque;
    if (size != 4 || off != 0) { venusa_fail(d->soc, FIFO + off, size, false, 0); }
    uint32_t value = 0;
    if (!d->count) { R(0x34) |= 0x10; }
    else {
        value = d->fifo[d->head]; d->head = (d->head + 1) % 64; d->count--;
        if (!d->count) { R(0x34) |= 0x10; }
    }
    signals(d); return value;
}
static void fifo_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusADVP *d = opaque; venusa_fail(d->soc, FIFO + off, size, true, value);
}
static void dvp_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusADVP *d = opaque;
    if (size != 4 || off % 4 || off > 0x28 || off == 0x2c) { goto unsupported; }
    if (off < 0x1c && off != 0x14 && (R(0x1c) & 1)) { return; }
    switch (off) {
    case 0: case 4: case 8: case 0xc:
        if (value & ~0x1fffu) { goto unsupported; } break;
    case 0x10:
        if (value & ~0x6fu) { goto unsupported; }
        if (value & 0x60) { d->head = d->count = d->word = d->packed = 0; R(0x34) |= 0x10; }
        value &= ~0x60u; break;
    case 0x14:
        if (value & ~1u) { goto unsupported; }
        if (value & 1) { d->head = d->count = d->word = d->packed = d->byte = d->line = 0; d->frame = d->request = false; R(0x34) = 0x10; signals(d); } return;
    case 0x18:
        if (value != 0 && value != 0xc && value != 0x14) { goto unsupported; } break;
    case 0x1c:
        if (value & ~1u) { goto unsupported; }
        if (value && (!R(0) || !R(4) || (R(0) * bytes_per_pixel(d)) % 4)) { goto unsupported; }
        if (!(value & 1)) { d->frame = false; }
        if ((value & 1) && !(R(0x1c) & 1)) { d->last_vsync = d->last_hsync = false; }
        break;
    case 0x20: if (value & ~0x1ffu || (value & 0x7f) > 63) { goto unsupported; } break;
    case 0x24: if (value & ~0xfffu) { goto unsupported; } break;
    case 0x28:
        if (value & ~0xfffu) { goto unsupported; }
        R(0x34) &= ~value; if (!d->count) { R(0x34) |= 0x10; } signals(d); return;
    default: goto unsupported;
    }
    R(off) = value; signals(d); return;
unsupported:
    venusa_fail(d->soc, BASE + off, size, true, value);
}
static const MemoryRegionOps ops = {
    .read = dvp_read, .write = dvp_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};
static const MemoryRegionOps fifo_ops = {
    .read = fifo_read, .write = fifo_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};
void venusa_dvp_reset(VenusASoC *s)
{
    VenusADVP *d = &s->dvp;
    memset(d->regs, 0, sizeof(d->regs)); R(4) = 720; R(0x20) = 1; R(0x24) = 0xfff; R(0x34) = 0x10;
    d->head = d->count = d->packed = d->word = d->byte = d->line = 0;
    d->frame = d->last_vsync = d->last_hsync = d->request = false;
    signals(d);
}
void venusa_dvp_xclk_ratio(VenusASoC *s, uint64_t *n, uint64_t *denominator)
{
    VenusADVP *d = &s->dvp;
    uint32_t cfg = s->syscfg[0x28 / 4];
    *n = 0; *denominator = 1;
    if (!(cfg & (1u << 17)) || !listenai_pinmux_function(&s->pinmux, 35, 13)) { return; }
    *n = 24000000; *denominator = d->xclk_div;
    if (cfg & (1u << 18)) {
        if (!(s->buscfg[2] & 1) || !(s->buscfg[2] & (1u << 26))) { *n = 0; return; }
        uint64_t mult = (uint64_t)(s->buscfg[6] & 0x1ff) << 27;
        if (s->buscfg[6] & (1u << 9)) { mult += s->buscfg[5] & 0x7ffffff; }
        *n *= mult;
        *denominator *= (UINT64_C(1) << 27) * (((s->buscfg[3] >> 12) & 15) + 6);
    }
}
void venusa_dvp_clock(VenusASoC *s)
{
    uint32_t cfg = s->syscfg[0x28 / 4];
    if (cfg & (1u << 16)) {
        s->dvp.xclk_div = (cfg >> 7) & 511;
        if (!s->dvp.xclk_div) { s->dvp.xclk_div = 512; }
        /* W1P generates a fresh load pulse on every write and reads zero. */
        s->syscfg[0x28 / 4] &= ~(1u << 16);
    }
    s->dvp.xclk_config = cfg;
    s->dvp.clock = !!(s->syscfg[0x34 / 4] & 0x800);
    signals(&s->dvp);
    if (s->dvp.camera_clock_changed) { s->dvp.camera_clock_changed(s->dvp.camera_opaque); }
}
void venusa_dvp_init(VenusASoC *s)
{
    VenusADVP *d = &s->dvp; d->soc = s; d->xclk_div = 1;
    memory_region_init_io(&d->io, OBJECT(s), &ops, d, "venusa-dvp", 0x100);
    memory_region_init_io(&d->fifo_io, OBJECT(s), &fifo_ops, d, "venusa-dvp-fifo", 4);
    d->fifo_io.disable_reentrancy_guard = true;
    memory_region_add_subregion(get_system_memory(), BASE, &d->io);
    memory_region_add_subregion(get_system_memory(), FIFO, &d->fifo_io);
    qdev_init_gpio_in_named(DEVICE(s), input, "dvp-input", 4);
}
