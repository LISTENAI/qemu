/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional SPIB/NOR and read-only OTP; no analog or flash busy timing. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/loader.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qapi/error.h"

#define NOR_SIZE 0x1000000
#define FLASH_BASE 0x47600000
#define OTP_BASE 0x48600000

void arcs_nor_init(ArcsNOR *chip, hwaddr address, const char *image, bool persist)
{
    listenai_nor_init(chip, NULL, "arcs-nor-128m", address, image, persist);
}

static void flash_irq(ArcsFlash *s)
{
    arcs_soc_irq(s->soc, 28, !!(s->regs[0x38 / 4] & s->regs[0x3c / 4]));
}

static ArcsNOR *selected_chip(ArcsFlash *s)
{
    uint32_t config = s->regs[0x58 / 4], range = s->regs[0x54 / 4];
    uint32_t mask = config >> 16, page = (s->regs[0x28 / 4] >> 12) & mask;
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

static void execute(ArcsFlash *s)
{
    uint32_t control = s->regs[0x20 / 4];
    unsigned mode = (control >> 24) & 15;
    if (mode != 1 && mode != 2 && mode != 3 && mode != 5 && mode != 7 && mode != 9) {
        arcs_soc_fail(s->soc, FLASH_BASE + 0x20, 4, true, control);
    }
    bool writes = mode == 1 || mode == 3 || mode == 5;
    bool reads = mode == 2 || mode == 3 || mode == 5 || mode == 9;
    unsigned count = writes ? ((control >> 12) & 511) + 1 : 0;
    unsigned length = reads ? (control & 511) + 1 : 0;
    if (s->tx_count < count) { return; }
    uint8_t opcode = s->regs[0x24 / 4], *data = s->tx;
    if (!(control & (1u << 30))) {
        if (!count) { arcs_soc_fail(s->soc, FLASH_BASE + 0x20, 4, true, control); }
        opcode = *data++; count--;
    }
    uint8_t result[512] = { 0 };
    unsigned received;
    if (length > sizeof(s->rx) - s->rx_count ||
        !listenai_nor_command(selected_chip(s), opcode, s->regs[0x28 / 4] & (NOR_SIZE - 1),
                     data, count, result, length, &received)) {
        arcs_soc_fail(s->soc, FLASH_BASE + 0x24, 4, true, opcode);
    }
    for (unsigned i = 0; i < received; i++) {
        s->rx[(s->rx_head + s->rx_count++) % sizeof(s->rx)] = result[i];
    }
    s->tx_count = 0; s->pending = false;
    s->regs[0x3c / 4] |= 16;
    flash_irq(s);
}

static bool flash_register(hwaddr off)
{
    switch (off) {
    case 0: case 0x10: case 0x14: case 0x20: case 0x24: case 0x28:
    case 0x2c: case 0x30: case 0x34: case 0x38: case 0x3c: case 0x40:
    case 0x50: case 0x54: case 0x58: case 0x80: return true;
    default: return false;
    }
}

static unsigned flash_fifo_bytes(ArcsFlash *s)
{
    unsigned bits = ((s->regs[0x10 / 4] >> 8) & 31) + 1;
    if (bits % 8) {
        arcs_soc_fail(s->soc, FLASH_BASE + 0x10, 4, true, s->regs[0x10 / 4]);
    }
    return s->regs[0x10 / 4] & 0x80 ? 4 : bits / 8;
}

static uint64_t flash_read(void *opaque, hwaddr off, unsigned size)
{
    ArcsFlash *s = opaque;
    if (size != 4 || !flash_register(off)) {
        arcs_soc_fail(s->soc, FLASH_BASE + off, size, false, 0);
    }
    if (off == 0x2c) {
        uint32_t value = 0;
        unsigned lanes = flash_fifo_bytes(s);
        for (unsigned i = 0; i < lanes && s->rx_count; i++) {
            value |= (uint32_t)s->rx[s->rx_head] << (8 * i);
            s->rx_head = (s->rx_head + 1) % sizeof(s->rx); s->rx_count--;
        }
        return value;
    }
    if (off == 0x34) {
        unsigned lanes = flash_fifo_bytes(s);
        return (s->tx_count ? 0 : 1u << 22) |
               MIN(31, (s->tx_count + lanes - 1) / lanes) << 16 |
               (s->rx_count ? 0 : 1u << 14) |
               MIN(31, (s->rx_count + lanes - 1) / lanes) << 8 |
               (s->pending || s->rx_count > 31 * lanes);
    }
    return s->regs[off / 4];
}

static void flash_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ArcsFlash *s = opaque;
    if (size != 4 || !flash_register(off)) { goto fail; }
    if (off == 0x2c) {
        unsigned lanes = flash_fifo_bytes(s);
        if (s->tx_count > sizeof(s->tx) - lanes) { goto fail; }
        for (unsigned i = 0; i < lanes; i++) {
            s->tx[s->tx_count++] = value >> (8 * i);
        }
        if (s->pending) { execute(s); }
        return;
    }
    if (off == 0x30) {
        if (value & 24) { goto fail; } /* DMA not connected. */
        if (value & 1) { s->pending = false; }
        if (value & 3) { s->rx_count = s->rx_head = 0; }
        if (value & 5) { s->tx_count = 0; }
        value &= ~7u;
    }
    if (off == 0x3c) { s->regs[off / 4] &= ~value; }
    else if (off != 0 && off != 0x34) { s->regs[off / 4] = value; }
    if (off == 0x24) {
        if (s->pending) { goto fail; }
        s->pending = true; execute(s);
    }
    flash_irq(s);
    return;
fail:
    arcs_soc_fail(s->soc, FLASH_BASE + off, size, true, value);
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
static const MemoryRegionOps flash_ops = STORAGE_OPS(flash_read, flash_write);
static const MemoryRegionOps otp_ops = STORAGE_OPS(otp_read, otp_write);

void arcs_storage_init(ArcsSoC *soc)
{
    ArcsFlash *s = &soc->flash;
    ArcsOTP *o = &soc->otp;
    assert(s->chips[0] != NULL || s->chips[1] != NULL);
    s->soc = o->soc = soc;
    memory_region_init_io(&s->io, OBJECT(soc), &flash_ops, s, "arcs-spib", 0x1000);
    memory_region_add_subregion(get_system_memory(), FLASH_BASE, &s->io);
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
    memset(s->regs, 0, sizeof(s->regs)); s->regs[0x10 / 4] = 0x20780;
    s->regs[0x54 / 4] = s->regs[0x58 / 4] = 0xffff0000;
    s->tx_count = s->rx_count = s->rx_head = 0; s->pending = false;
    /* Controller reset does not erase or reset the external NOR chip. */
    flash_irq(s);
}

void arcs_storage_reset(ArcsSoC *soc)
{
    arcs_flash_reset(soc);
    soc->otp.control = 0;
    soc->otp.timing = 0x10982611; soc->otp.divider = 2;
}
