/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional SPIB/NOR and read-only OTP; no analog or flash busy timing. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/loader.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qapi/error.h"

#define FLASH_BASE 0x47600000
#define OTP_BASE 0x48600000

void arcs_nor_init(ArcsNOR *chip, hwaddr address, const char *image, bool persist)
{
    listenai_nor_init(chip, NULL, "arcs-nor-128m", address, image, persist);
}

static void flash_irq(void *opaque, bool level)
{
    ArcsFlash *s = opaque;
    arcs_soc_irq(s->soc, 28, level);
}

static void flash_fault(void *opaque, hwaddr address, unsigned size,
                        bool write, uint64_t value)
{
    ArcsFlash *s = opaque;
    arcs_soc_fail(s->soc, address, size, write, value);
}

static ListenAINOR *selected_chip(void *opaque, uint32_t address)
{
    ArcsFlash *s = opaque;
    uint32_t config = s->core.regs[0x58 / 4], range = s->core.regs[0x54 / 4];
    uint32_t mask = config >> 16, page = (address >> 12) & mask;
    bool in_range = page >= (range & mask) && page <= ((range >> 16) & mask);
    ArcsNOR *selected = NULL;
    for (unsigned i = 0; i < 2; i++) {
        unsigned mode = (config >> (2 * i)) & 3;
        if (!(mode == 1 || (mode == 0 && in_range == (i == 0))) || !s->chips[i]) {
            continue;
        }
        if (selected) { arcs_soc_fail(s->soc, FLASH_BASE + 0x58, 4, true, config); }
        selected = s->chips[i];
    }
    return selected;
}

static bool flash_register(void *opaque, hwaddr off, bool write)
{
    switch (off) {
    case 0: case 0x10: case 0x14: case 0x20: case 0x24: case 0x28:
    case 0x2c: case 0x30: case 0x34: case 0x38: case 0x3c: case 0x40:
    case 0x50: case 0x54: case 0x58: case 0x80: return true;
    default: return false;
    }
}

static uint32_t otp_word(ArcsOTP *s, hwaddr off)
{
    if (off >= 0x200 && off <= 0x3fc) { return ldl_le_p(s->bytes + off - 0x200); }
    switch (off) {
    case 0: return 0x200; /* Ideal autoload has completed before boot. */
    case 8: return s->control;
    case 0x14: return s->timing;
    case 0x18: return s->divider;
    case 0x1c: return ldl_le_p(s->bytes + 4 * (s->control & 127));
    default: arcs_soc_fail(s->soc, OTP_BASE + off, 4, false, 0); return 0;
    }
}

static uint64_t otp_read(void *opaque, hwaddr off, unsigned size)
{
    ArcsOTP *s = opaque;
    if (off & (size - 1)) { arcs_soc_fail(s->soc, OTP_BASE + off, size, false, 0); }
    return otp_word(s, off & ~3u) >> (8 * (off & 3));
}

static void otp_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ArcsOTP *s = opaque;
    if (size != 4) { goto fail; }
    /* The normal row is available with redundancy enabled or explicitly
     * disabled. Selecting the redundancy row, margin reads and programming
     * remain unsupported; the 512-byte identity store is never modified.
     */
    if (off == 8 && !(value & ~0x41007fu)) {
        s->control = value & ~0x10000u; return;
    }
    if (off == 0x14) { s->timing = value; return; }
    if (off == 0x18 && !(value & ~15u)) { s->divider = value; return; }
fail:
    arcs_soc_fail(s->soc, OTP_BASE + off, size, true, value);
}

#define STORAGE_OPS(read_fn, write_fn) { \
    .read = read_fn, .write = write_fn, .endianness = DEVICE_LITTLE_ENDIAN, \
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true }, \
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true }, \
}
static const MemoryRegionOps otp_ops = STORAGE_OPS(otp_read, otp_write);

void arcs_storage_init(ArcsSoC *soc)
{
    ArcsFlash *s = &soc->flash;
    ArcsOTP *o = &soc->otp;
    assert(s->chips[0] != NULL || s->chips[1] != NULL);
    s->soc = o->soc = soc;
    listenai_spimem_init(&s->core, OBJECT(soc), "arcs-spib", FLASH_BASE,
                         selected_chip, flash_register, flash_irq, flash_fault, s);
    memory_region_init_io(&o->io, OBJECT(soc), &otp_ops, o, "arcs-otp", 0x1000);
    memory_region_add_subregion(get_system_memory(), OTP_BASE, &o->io);
    /* Stable simulated identity; no factory keys or cloud credentials. */
    stq_le_p(o->bytes + 8, UINT64_C(0x010045444f4e4552));
    const char *image = getenv("ARCS_QEMU_OTP_IMAGE");
    if (image && (get_image_size(image) != sizeof(o->bytes) ||
        load_image_size(image, o->bytes, sizeof(o->bytes)) != sizeof(o->bytes))) {
        error_report("OTP image must contain exactly 512 bytes"); exit(1);
    }
}

void arcs_flash_reset(ArcsSoC *soc)
{
    ArcsFlash *s = &soc->flash;
    listenai_spimem_reset(&s->core);
    s->core.regs[0x54 / 4] = s->core.regs[0x58 / 4] = 0xffff0000;
    /* Controller reset does not erase or reset the external NOR chip. */
}

void arcs_storage_reset(ArcsSoC *soc)
{
    arcs_flash_reset(soc);
    soc->otp.control = 0;
    soc->otp.timing = 0x10982611; soc->otp.divider = 2;
}
