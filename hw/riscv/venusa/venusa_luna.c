/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"

static G_NORETURN void fatal(void *opaque, const char *message)
{
    VenusASoC *s = opaque;
    error_report("%s", message);
    venusa_report(s, "unsupported-luna");
    exit(1);
}
static void irq(void *opaque, bool level)
{
    venusa_irq(opaque, 42, level);
}
static bool read_extra(void *opaque, hwaddr offset, uint64_t *value)
{
    VenusASoC *s = opaque;
    switch (offset) {
    case 0xe0: *value = s->luna_busrd; return true;
    case 0x100: *value = s->luna_mtq_control; return true;
    case 0x1d8: *value = s->luna_mtq_mask; return true;
    case 0x1d4: case 0x1dc: case 0x2e0: *value = 0; return true;
    default: return false;
    }
}
static bool write_extra(void *opaque, hwaddr offset, uint32_t value)
{
    VenusASoC *s = opaque;
    switch (offset) {
    case 0xe0:
        s->luna_busrd = value & 0x7ffff;
        return true;
    case 0x100:
        /* The task scheduler is disabled; queued microprogram execution
         * requires a separate backend contract. Do not accept EN/START. */
        if (value & ~0x14u) {
            fatal(s, "VenusA LUNA task scheduler execution unsupported");
        }
        s->luna_mtq_control = value & 4;
        return true;
    case 0x1d8: s->luna_mtq_mask = value & 0x1fffffff; return true;
    case 0x1d4: return true; /* No task scheduler events while disabled. */
    default: return false;
    }
}
static void reset_extra(void *opaque)
{
    VenusASoC *s = opaque;
    s->luna_mtq_control = 0;
    s->luna_mtq_mask = 0;
    s->luna_busrd = 0;
}
void venusa_luna_init(VenusASoC *s)
{
    bool safe = g_strcmp0(getenv("LISEM_QEMU_LUNA_SAFE_READS"), "1") == 0;
    listenai_luna_init(&s->luna, OBJECT(s), get_system_memory(),
                       0x46000000, 0x7fff, safe, s, fatal);
    s->luna.irq = irq;
    s->luna.irq_mask_bits = 0xffffff;
    s->luna.reset_extra = reset_extra;
    s->luna.read_extra = read_extra;
    s->luna.write_extra = write_extra;
}
void venusa_luna_reset(VenusASoC *s)
{
    listenai_luna_reset(&s->luna);
    listenai_luna_clock(&s->luna, (s->syscfg[0x34 / 4] & 0x80) != 0);
}
