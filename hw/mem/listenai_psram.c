/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional PSRAM controller; board-selected Xccela geometry and identity. */
#include "qemu/osdep.h"
#include "hw/mem/listenai_psram.h"
#include "qapi/error.h"
#include "system/address-spaces.h"

static G_NORETURN void psram_fault(ListenAIPSRAM *s, hwaddr off,
                                  unsigned size, bool write, uint64_t value)
{
    s->fault(s->opaque, s->address + off, size, write, value);
    abort();
}

static void xccela_reset_modes(ListenAIXccela *chip)
{
    memcpy(chip->modes, chip->reset_modes, sizeof(chip->modes));
}

void listenai_xccela_init(ListenAIXccela *chip, Object *owner,
                          const char *name, uint64_t size,
                          const uint8_t reset_modes[10])
{
    memory_region_init_ram(&chip->ram, owner, name, size, &error_fatal);
    memcpy(chip->reset_modes, reset_modes, sizeof(chip->reset_modes));
    xccela_reset_modes(chip);
}

static bool valid(ListenAIPSRAM *s, hwaddr off)
{
    if ((off & 3) || off >= sizeof(s->regs)) { return false; }
    if (off <= 0x30 || (off >= 0x100 && off <= 0x19c) ||
        (off >= 0x800 && off <= 0x824)) { return true; }
    switch (off) {
    case 0x38: case 0x3c: case 0x40: case 0x44: case 0x50:
    case 0x80: case 0x84: case 0x88: case 0x8c: case 0x90: case 0x94:
    case 0xa0: case 0xa4: case 0xa8: case 0xac: case 0xc00:
        return true;
    default: return s->extra_valid && s->extra_valid(s->opaque, off);
    }
}

static uint32_t read_word(ListenAIPSRAM *s, hwaddr off)
{
    if (!valid(s, off)) { psram_fault(s, off, 4, false, 0); }
    bool locked = s->regs[0x10 / 4] & 0x10000;
    if (off == 0x20) { return locked; }
    if (off == 0x18) { return locked ? 0x4001 : 0; }
    if (off == 0x1c) { return s->regs[0x14 / 4]; }
    if (off >= 0x800 && off <= 0x824) { return s->chip->modes[(off - 0x800) / 4]; }
    return s->regs[off / 4];
}

static uint64_t psram_read(void *opaque, hwaddr off, unsigned size)
{
    ListenAIPSRAM *s = opaque;
    if (off & (size - 1)) { psram_fault(s, off, size, false, 0); }
    return read_word(s, off & ~3u) >> (8 * (off & 3));
}

static void psram_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenAIPSRAM *s = opaque;
    hwaddr original_off = off;
    uint64_t original_value = value;
    if ((off & (size - 1)) || !valid(s, off & ~3u)) { goto fail; }
    if (size != 4) {
        uint32_t shift = 8 * (off & 3), mask = size == 1 ? 255 : 65535;
        value = (read_word(s, off & ~3u) & ~(mask << shift)) | ((uint32_t)value << shift);
        off &= ~3u;
    }
    if (off >= 0x800 && off <= 0x824) {
        unsigned index = (off - 0x800) / 4;
        if (index != 1 && index != 2) { s->chip->modes[index] = value; }
        return;
    }
    if (off == 0xc00) {
        if (value > 9) { goto fail; }
        uint32_t command = s->regs[(0x100 + value * 16) / 4] & 0xffff;
        if (command == 0x1ff) { xccela_reset_modes(s->chip); }
        else if (command == 0x30c) { s->chip->modes[6] = 0; }
        else if (command) { goto fail; }
        return;
    }
    if (off == 0x18 || off == 0x1c || off == 0x20 || off == 0x24 || off == 0x28) {
        return; /* Status or instantaneous ideal DLL resynchronization. */
    }
    if (off == 0xa0 || off == 0xa4) { s->regs[off / 4] &= ~value; return; }
    s->regs[off / 4] = value;
    return;
fail:
    psram_fault(s, original_off, size, true, original_value);
}

static const MemoryRegionOps ops = {
    .read = psram_read, .write = psram_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_psram_init(ListenAIPSRAM *s, Object *owner, const char *name,
                         hwaddr address, ListenAIXccela *chip,
                         ListenAIPSRAMExtraValid extra_valid,
                         ListenAIPSRAMFault fault, void *opaque)
{
    assert(chip != NULL);
    s->chip = chip;
    s->address = address;
    s->extra_valid = extra_valid;
    s->fault = fault;
    s->opaque = opaque;
    memory_region_init_io(&s->io, owner, &ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), address, &s->io);
}

void listenai_psram_reset(ListenAIPSRAM *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    xccela_reset_modes(s->chip);
}
