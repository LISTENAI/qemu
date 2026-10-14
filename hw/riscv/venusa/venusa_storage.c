/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA single-chip SPIB integration. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/block-backend.h"
#include "hw/irq.h"

static ListenAINOR *select_flash(void *opaque, uint32_t address)
{
    VenusASoC *s = opaque;
    return &s->nor;
}

static bool valid_register(void *opaque, hwaddr offset, bool write)
{
    switch (offset) {
    case 0: case 0x10: case 0x14: case 0x20: case 0x24: case 0x28:
    case 0x2c: case 0x30: case 0x34: case 0x38: case 0x3c: case 0x40:
    case 0x50: case 0x7c: case 0x80: case 0x84: case 0x88:
    case 0x8c: case 0x90: case 0x94:
        return !write || (offset != 0 && offset != 0x34 && offset != 0x7c);
    default: return false;
    }
}

static void flash_irq(void *opaque, bool level)
{
    venusa_irq(opaque, 35, level);
}

static void flash_fault(void *opaque, hwaddr address, unsigned size,
                        bool write, uint64_t value)
{
    venusa_fail(opaque, address, size, write, value);
}

static void sd_irq(void *opaque, int unused, int level)
{
    venusa_irq(opaque, 39, level);
}

void venusa_storage_init(VenusASoC *s)
{
    venusa_flashdl_init(s);
    listenai_sd_init(&s->sd, OBJECT(s), "venusa-sd", 0x44a00000,
                    UINT64_C(0x00002f7727200080), true, blk_by_name("sdcard"),
                    qemu_allocate_irq(sd_irq, s, 0), flash_fault, s);
    listenai_sd_enable_unadvertised_sdma(&s->sd);
    listenai_nor_init_block(&s->nor, OBJECT(s), "venusa-nor", 0x30000000, s->flash);
    listenai_spimem_init(&s->flash_ctrl, OBJECT(s), "venusa-spib", 0x44200000,
                        select_flash, valid_register, flash_irq, flash_fault, s);
}

void venusa_storage_reset(VenusASoC *s)
{
    venusa_flashdl_reset(s);
    listenai_sd_reset(&s->sd);
    listenai_spimem_reset(&s->flash_ctrl);
    /* SPIB CONFIG: observed on CSK7004J8U, FIFO depth and memory mode. */
    s->flash_ctrl.regs[0x7c / 4] = 0x1311;
}
