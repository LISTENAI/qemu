/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS GPT PWM registers and board-level duty observation. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "system/address-spaces.h"

double arcs_gpt_duty(ArcsGPT *s, unsigned ch)
{
    assert(ch < 8);
    uint32_t control = s->regs[9 + ch], reload = s->regs[17 + ch];
    bool invert = control & 64;
    if (!s->running[ch]) { return invert ? 1 : 0; }
    unsigned high = (reload >> 16) + 1, low = (reload & 65535) + 1;
    double duty = (double)high / (high + low);
    return invert ? 1 - duty : duty;
}

static uint64_t gpt_read(void *opaque, hwaddr off, unsigned size)
{
    ArcsGPT *s = opaque;
    if (size != 4 || (off & 3) || off > 0x144) {
        arcs_soc_fail(s->soc, 0x47300000 + off, size, false, 0);
    }
    if (off == 0x98) { return 8; }
    if (off == 0x128) { return 0xffff0000; }
    if (off == 0x94 || off == 0x134) { return 0; }
    return s->regs[off / 4];
}

static void gpt_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ArcsGPT *s = opaque;
    if (size != 4 || (off & 3) || off > 0x144 ||
        (off >= 0xdc && off <= 0x11c && value) || (off == 0xbc && value) ||
        (off == 0xd8 && (value & 0xffffff))) { goto fail; }
    if (off == 0x94) {
        for (unsigned i = 0; i < 8; i++) { if (value & (1u << (8 + i))) { s->running[i] = false; } }
    } else if (off != 0xd0 && off != 0xd4 && off != 0x140 && off != 0x134) {
        if (off >= 0x24 && off <= 0x40) {
            unsigned ch = (off - 0x24) / 4;
            if (value & (1 << 18)) { s->running[ch] = false; }
            if (value & (1 << 19)) {
                if ((value & 7) != 3 || (value & 128)) { goto fail; }
                s->running[ch] = true;
            }
            value &= ~0xc0000u;
        }
        s->regs[off / 4] = value;
    }
    if (s->changed) { s->changed(s->opaque); }
    return;
fail:
    arcs_soc_fail(s->soc, 0x47300000 + off, size, true, value);
}

#define OPS(read_fn, write_fn, safe_fn) { \
    .read = read_fn, .write = write_fn, .icount_read_safe = safe_fn, \
    .endianness = DEVICE_LITTLE_ENDIAN, \
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true }, \
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true }, \
}
static const MemoryRegionOps gpt_ops = OPS(gpt_read, gpt_write, NULL);

void arcs_gpt_init(ArcsSoC *soc)
{
    ArcsGPT *g = &soc->gpt;
    g->soc = soc;
    memory_region_init_io(&g->io, OBJECT(soc), &gpt_ops, g, "arcs-gpt", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x47300000, &g->io);
}

void arcs_gpt_reset(ArcsSoC *soc)
{
    ArcsGPT *s = &soc->gpt;
    memset(s->regs, 0, sizeof(s->regs)); memset(s->running, 0, sizeof(s->running));
    s->regs[0xc0 / 4] = s->regs[0xc4 / 4] = UINT32_MAX;
    if (s->changed) { s->changed(s->opaque); }
}
