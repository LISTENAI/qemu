/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CSK7004J8U SIP Xccela memory and controller integration. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/address-spaces.h"

static bool extra_register(void *opaque, hwaddr offset)
{
    switch (offset) {
    case 0x34: case 0x48: case 0x4c: case 0x54: case 0x7c:
        return true;
    default: return false;
    }
}

static void psram_fault(void *opaque, hwaddr address, unsigned size,
                        bool write, uint64_t value)
{
    venusa_fail(opaque, address, size, write, value);
}

void venusa_psram_init(VenusASoC *s)
{
    /* Read after a real reset command on CSK7004J8U. MR2 identifies 64 Mbit. */
    static const uint8_t modes[10] = {
        0x09, 0x8d, 0x93, 0xa0, 0x40, 0, 0, 0, 0x05, 0x01,
    };
    listenai_xccela_init(&s->psram_chip, OBJECT(s), "venusa-sip-psram",
                        8 * MiB, modes);
    s->psram_chip.mode_read_pairs = true;
    memory_region_add_subregion(get_system_memory(), 0x38000000, &s->psram_chip.ram);
    memory_region_init_alias(&s->psram_alias, OBJECT(s), "venusa-psram-alias",
                             &s->psram_chip.ram, 0, 8 * MiB);
    memory_region_add_subregion(get_system_memory(), 0x38800000, &s->psram_alias);
    listenai_psram_init(&s->psram_ctrl, OBJECT(s), "venusa-psram-controller",
                       0x44600000, &s->psram_chip, 8, extra_register, psram_fault, s);
}
