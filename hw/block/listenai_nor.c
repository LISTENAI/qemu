/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Board-owned serial NOR command engine with per-device capacity. */
#include "qemu/osdep.h"
#include "hw/block/listenai_nor.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/host-utils.h"

#define NOR_SIZE LISTENAI_NOR_SIZE

/* JESD216 1.0 basic parameters for the functional NOR geometry.  Only
 * advertise the implemented baseline: 24-bit addressing, 256-byte pages
 * (implicit in this revision), and 4/32/64 KiB erase.  Unassigned parameter
 * space reads as erased bytes; it never aliases the mutable NOR contents. */
static const uint8_t nor_sfdp[] = {
    0x53, 0x46, 0x44, 0x50, 0x00, 0x01, 0x00, 0xff,
    0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0xff,
    0x01, 0x20, 0x00, 0x00, /* DW1: uniform 4 KiB erase, opcode 0x20. */
    0xff, 0xff, 0xff, 0x07, /* DW2: replaced with the declared density. */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x0c, 0x20, 0x0f, 0x52, /* DW8: 4 KiB and 32 KiB erase. */
    0x10, 0xd8, 0x00, 0x00, /* DW9: 64 KiB erase. */
};

void listenai_nor_init(ListenAINOR *chip, Object *owner, const char *name,
                       hwaddr address, const char *image, bool persist)
{
    listenai_nor_init_capacity(chip, owner, name, address, image, persist, NOR_SIZE);
}

void listenai_nor_init_capacity(ListenAINOR *chip, Object *owner, const char *name,
                             hwaddr address, const char *image, bool persist,
                             uint32_t size)
{
    if (size < 65536 || size > LISTENAI_NOR_SIZE || !is_power_of_2(size)) {
        error_report("Unsupported NOR capacity: %u", size); exit(1);
    }
    chip->size = size;
    chip->address = address;
    memory_region_init_rom(&chip->rom, owner, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), address, &chip->rom);
    uint8_t *bytes = memory_region_get_ram_ptr(&chip->rom);
    memset(bytes, 0xff, size);
    if (image) {
        chip->backing = fopen(image, persist ? "r+b" : "rb");
        if (!chip->backing || fseek(chip->backing, 0, SEEK_END) ||
            ftell(chip->backing) != size || fseek(chip->backing, 0, SEEK_SET) ||
            fread(bytes, 1, size, chip->backing) != size) {
            error_report("NOR backing image does not match the declared storage layout"); exit(1);
        }
        if (!persist) { fclose(chip->backing); chip->backing = NULL; }
    } else if (persist) {
        error_report("Persistent NOR requires a Flash image"); exit(1);
    }
}

void listenai_nor_init_block(ListenAINOR *chip, Object *owner, const char *name,
                            hwaddr address, BlockBackend *block, uint32_t size)
{
    if (!block || blk_getlength(block) != size) {
        error_report("NOR block image must match its declared capacity"); exit(1);
    }
    if (blk_set_perm(block, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_CONSISTENT_READ, &error_fatal) < 0) {
        error_report("NOR requires exclusive writable block access"); exit(1);
    }
    listenai_nor_init_capacity(chip, owner, name, address, NULL, false, size);
    chip->block = block;
    if (blk_pread(block, 0, size, memory_region_get_ram_ptr(&chip->rom), 0) < 0) {
        error_report("NOR block read failed"); exit(1);
    }
}

static void nor_write(ListenAINOR *chip, uint32_t off, const uint8_t *data, unsigned count)
{
    assert(off <= chip->size && count <= chip->size - off);
    /* QEMU's ROM writer invalidates translated code on both CPUs. No ROM
     * loader blob is registered: a warm reset must never reload old Flash. */
    if (address_space_write_rom(&address_space_memory, chip->address + off,
                               MEMTXATTRS_UNSPECIFIED, data, count) != MEMTX_OK) {
        error_report("NOR writeback failed"); exit(1);
    }
    if (chip->block && (blk_pwrite(chip->block, off, count, data, 0) < 0 ||
                        blk_flush(chip->block) < 0)) {
        error_report("NOR block persistence failed"); exit(1);
    }
    if (chip->backing && (fseek(chip->backing, off, SEEK_SET) ||
        fwrite(data, 1, count, chip->backing) != count || fflush(chip->backing) ||
        fsync(fileno(chip->backing)))) {
        error_report("NOR persistence failed: %s", strerror(errno)); exit(1);
    }
}

/* W25Q status-register array protection (WPS=0). BP selects a range,
 * TB its end, SEC sector granularity, and CMP the complement. Commands
 * touching a protected byte are ignored, including whole-chip erases. */
static int nor_protected(ListenAINOR *chip, uint32_t start, uint32_t length)
{
    unsigned bp = (chip->status[0] >> 2) & 7;
    bool sectors = chip->status[0] & 0x40;
    if ((chip->status[2] & 4) || (sectors && bp == 6)) {
        return -1; /* Individual locks and reserved SEC/BP are unsupported. */
    }
    uint32_t size = !bp ? 0 : bp == 7 ? chip->size : sectors
        ? 4096u << MIN(bp - 1, 3)
        : MIN(chip->size, MAX(65536u, chip->size / 64) << (bp - 1));
    bool bottom = chip->status[0] & 0x20;
    if (chip->status[1] & 0x40) {
        size = chip->size - size;
        bottom = !bottom;
    }
    if (!size) {
        return 0;
    }
    uint32_t first = bottom ? 0 : chip->size - size;
    return start < first + size && start + length > first;
}

bool listenai_nor_command(ListenAINOR *chip, uint8_t op, uint32_t address,
                        const uint8_t *tx, unsigned count, uint8_t *rx,
                        unsigned length, unsigned *received)
{
    if (!chip) {
        /* No selected device drives MISO. Writes have no side effects. */
        memset(rx, 0xff, length);
        *received = length;
        return true;
    }
    address &= op == 0x5a ? 0xffffff : chip->size - 1;
    uint8_t *bytes = memory_region_get_ram_ptr(&chip->rom);
    const uint8_t density = ctz32(chip->size);
    const uint8_t jedec[] = { 0xef, 0x40, density };
    static const uint8_t uid[] = { 0x52, 0x45, 0x4e, 0x4f, 0x44, 0x45, 0, 1 };
    *received = 0;
    switch (op) {
    case 0x05: case 0x35: case 0x15: case 0x9f: case 0x90: case 0x4b: case 0x5a:
    case 0x03: case 0x0b: case 0x3b: case 0xbb: case 0x6b: case 0xeb:
        *received = length; break;
    }
    if (chip->asleep && op != 0xab) { return false; }
    switch (op) {
    case 0x06: chip->wel = true; break;
    case 0x04: chip->wel = false; break;
    case 0x05: memset(rx, (chip->status[0] & 0xfc) | (chip->wel ? 2 : 0), length); break;
    case 0x35: memset(rx, chip->status[1], length); break;
    case 0x15: memset(rx, (chip->status[2] & 0xfe) | chip->four_byte, length); break;
    case 0xe9: chip->four_byte = false; break;
    case 0xb7: chip->four_byte = true; break;
    case 0x9f:
        for (unsigned i = 0; i < length; i++) { rx[i] = jedec[i % 3]; }
        break;
    case 0x90:
        for (unsigned i = 0; i < length; i++) { rx[i] = ((address + i) & 1) ? density - 1 : 0xef; }
        break;
    case 0x4b:
        for (unsigned i = 0; i < length; i++) { rx[i] = uid[i % 8]; }
        break;
    case 0x5a:
        for (unsigned i = 0; i < length; i++) {
            uint32_t offset = (address + i) & 0xffffff;
            rx[i] = offset >= 20 && offset < 24
                ? ((chip->size * 8u - 1) >> (8 * (offset - 20))) & 255
                : offset < sizeof(nor_sfdp) ? nor_sfdp[offset] : 0xff;
        }
        break;
    case 0x01: case 0x31: case 0x11:
        if (chip->wel) {
            unsigned first = op == 1 ? 0 : op == 0x31 ? 1 : 2;
            if (count > 3 - first) { return false; }
            memcpy(chip->status + first, tx, count);
            chip->wel = false;
        }
        break;
    case 0x03: case 0x0b: case 0x3b: case 0xbb: case 0x6b: case 0xeb:
        for (unsigned i = 0; i < length; i++) { rx[i] = bytes[(address + i) & (chip->size - 1)]; }
        break;
    case 0x02: case 0x32:
        if (chip->wel) {
            uint8_t page[256];
            uint32_t start = address & ~255u;
            int protection = nor_protected(chip, start, sizeof(page));
            if (protection < 0) { return false; }
            if (protection) { chip->wel = false; break; }
            memcpy(page, bytes + start, sizeof(page));
            for (unsigned i = 0; i < count; i++) { page[(address + i) & 255] &= tx[i]; }
            nor_write(chip, start, page, sizeof(page));
            chip->wel = false;
        }
        break;
    case 0x20: case 0x52: case 0xd8: case 0x60: case 0xc7:
        if (chip->wel) {
            unsigned size = op == 0x20 ? 4096 : op == 0x52 ? 32768 :
                            op == 0xd8 ? 65536 : chip->size;
            int protection = nor_protected(chip, address & ~(size - 1), size);
            if (protection < 0) { return false; }
            if (protection) { chip->wel = false; break; }
            g_autofree uint8_t *erased = g_malloc(size);
            memset(erased, 0xff, size);
            nor_write(chip, address & ~(size - 1), erased, size);
            chip->wel = false;
        }
        break;
    case 0x66: chip->reset_enabled = true; break;
    case 0x99:
        if (chip->reset_enabled) {
            chip->wel = chip->asleep = chip->reset_enabled = false;
        }
        break;
    case 0xb9: chip->asleep = true; break;
    case 0xab: chip->asleep = false; break;
    default: return false;
    }
    return true;
}

