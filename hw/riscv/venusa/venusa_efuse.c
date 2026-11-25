/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA read-only OTP shadow loading. Electrical programming is unsupported. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"

static bool clock_enabled(VenusASoC *s)
{
    return (s->aon[0x54 / 4] & (1u << 6)) != 0;
}

static void load_complete(void *opaque)
{
    VenusASoC *s = opaque;
    if (!s->efuse_busy || !clock_enabled(s)) { return; }
    memcpy(s->efuse_shadow, s->otp, sizeof(s->otp));
    s->efuse_busy = false;
}

void venusa_efuse_clock(VenusASoC *s)
{
    timer_del(s->efuse_load);
    if (s->efuse_busy && clock_enabled(s)) {
        /* Ideal digital transfer at the next virtual event; no analog delay
         * is inferred, and status reads never drive completion. */
        timer_mod(s->efuse_load, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

void venusa_efuse_init(VenusASoC *s)
{
    s->efuse_load = timer_new_ns(QEMU_CLOCK_VIRTUAL, load_complete, s);
}

void venusa_efuse_reset(VenusASoC *s)
{
    timer_del(s->efuse_load);
    s->efuse_busy = false;
    /* Power-on shadows are available to the mask ROM before application code. */
    memcpy(s->efuse_shadow, s->otp, sizeof(s->otp));
}

bool venusa_efuse_read(VenusASoC *s, hwaddr address, uint32_t *value)
{
    switch (address) {
    case 0x45500000: *value = s->efuse_busy ? 0x100 : 0x200; return true;
    case 0x45500020: *value = 0; return true; /* Self-clearing load request. */
    default: return false;
    }
}

bool venusa_efuse_write(VenusASoC *s, hwaddr address, uint32_t value)
{
    if (address != 0x45500020 || (value & ~1u)) { return false; }
    if (!value) { return true; }
    if (!clock_enabled(s) || s->efuse_busy) { return false; }
    s->efuse_busy = true;
    venusa_efuse_clock(s);
    return true;
}
