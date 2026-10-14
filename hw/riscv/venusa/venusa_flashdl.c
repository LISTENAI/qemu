/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Flash sampling delay line in fixed-delay bypass mode. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/address-spaces.h"

static uint64_t read_register(void *opaque, hwaddr offset, unsigned size)
{
    VenusASoC *s = opaque;
    if (size != 4 || offset > 4 || (offset & 3)) {
        venusa_fail(s, 0x44300000 + offset, size, false, 0);
    }
    return s->flash_delay_regs[offset / 4];
}

static void write_register(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    VenusASoC *s = opaque;
    bool valid = size == 4 && ((offset == 0 && !(value & ~0x17fffu) &&
                               (value & 0x4000)) ||
                              (offset == 4 && !(value & ~0xffu)));
    /* Analog cycle calibration and lock observations require a delay model. */
    if (!valid) { venusa_fail(s, 0x44300000 + offset, size, true, value); }
    s->flash_delay_regs[offset / 4] = value;
}

static const MemoryRegionOps ops = {
    .read = read_register, .write = write_register,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void venusa_flashdl_init(VenusASoC *s)
{
    memory_region_init_io(&s->flash_delay_io, OBJECT(s), &ops, s,
                          "venusa-flash-delay", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x44300000,
                               &s->flash_delay_io);
}

void venusa_flashdl_reset(VenusASoC *s)
{
    /* Fixed delay configuration observed after the mask-ROM boot sequence. */
    s->flash_delay_regs[0] = 0x4004;
    s->flash_delay_regs[1] = 0;
}
