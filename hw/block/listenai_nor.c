/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Board-owned 128-Mbit serial NOR command engine. */
#include "qemu/osdep.h"
#include "hw/block/listenai_nor.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

#define NOR_SIZE LISTENAI_NOR_SIZE

/* JESD216 1.0 basic parameters for the functional NOR geometry.  Only
 * advertise the implemented baseline: 24-bit addressing, 256-byte pages
 * (implicit in this revision), and 4/32/64 KiB erase.  Unassigned parameter
 * space reads as erased bytes; it never aliases the mutable NOR contents. */
static const uint8_t nor_sfdp[] = {
    0x53, 0x46, 0x44, 0x50, 0x00, 0x01, 0x00, 0xff,
    0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0xff,
    0x01, 0x20, 0x00, 0x00, /* DW1: uniform 4 KiB erase, opcode 0x20. */
    0xff, 0xff, 0xff, 0x07, /* DW2: 128 Mbit minus one. */
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
    chip->address = address;
    memory_region_init_rom(&chip->rom, owner, name, NOR_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), address, &chip->rom);
    uint8_t *bytes = memory_region_get_ram_ptr(&chip->rom);
    memset(bytes, 0xff, NOR_SIZE);
    if (image) {
        chip->backing = fopen(image, persist ? "r+b" : "rb");
        if (!chip->backing || fseek(chip->backing, 0, SEEK_END) ||
            ftell(chip->backing) != NOR_SIZE || fseek(chip->backing, 0, SEEK_SET) ||
            fread(bytes, 1, NOR_SIZE, chip->backing) != NOR_SIZE) {
            error_report("NOR requires a readable exact 16 MiB Flash image"); exit(1);
        }
        if (!persist) { fclose(chip->backing); chip->backing = NULL; }
    } else if (persist) {
        error_report("Persistent NOR requires a Flash image"); exit(1);
    }
}

void listenai_nor_init_block(ListenAINOR *chip, Object *owner, const char *name,
                            hwaddr address, BlockBackend *block)
{
    if (!block || blk_getlength(block) != NOR_SIZE) {
        error_report("NOR requires an exact 16 MiB block image"); exit(1);
    }
    if (blk_set_perm(block, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_CONSISTENT_READ, &error_fatal) < 0) {
        error_report("NOR requires exclusive writable block access"); exit(1);
    }
    listenai_nor_init(chip, owner, name, address, NULL, false);
    chip->block = block;
    if (blk_pread(block, 0, NOR_SIZE, memory_region_get_ram_ptr(&chip->rom), 0) < 0) {
        error_report("NOR block read failed"); exit(1);
    }
}

static void nor_write(ListenAINOR *chip, uint32_t off, const uint8_t *data, unsigned count)
{
    assert(off <= NOR_SIZE && count <= NOR_SIZE - off);
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
    address &= NOR_SIZE - 1;
    uint8_t *bytes = memory_region_get_ram_ptr(&chip->rom);
    static const uint8_t jedec[] = { 0xef, 0x40, 0x18 };
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
        for (unsigned i = 0; i < length; i++) { rx[i] = ((address + i) & 1) ? 0x17 : 0xef; }
        break;
    case 0x4b:
        for (unsigned i = 0; i < length; i++) { rx[i] = uid[i % 8]; }
        break;
    case 0x5a:
        for (unsigned i = 0; i < length; i++) {
            uint32_t offset = (address + i) & 0xffffff;
            rx[i] = offset < sizeof(nor_sfdp) ? nor_sfdp[offset] : 0xff;
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
        for (unsigned i = 0; i < length; i++) { rx[i] = bytes[(address + i) & (NOR_SIZE - 1)]; }
        break;
    case 0x02: case 0x32:
        if (chip->wel) {
            if (chip->status[0] & 0x7c) { return false; }
            uint8_t page[256];
            uint32_t start = address & ~255u;
            memcpy(page, bytes + start, sizeof(page));
            for (unsigned i = 0; i < count; i++) { page[(address + i) & 255] &= tx[i]; }
            nor_write(chip, start, page, sizeof(page));
            chip->wel = false;
        }
        break;
    case 0x20: case 0x52: case 0xd8: case 0x60: case 0xc7:
        if (chip->wel) {
            if (chip->status[0] & 0x7c) { return false; }
            unsigned size = op == 0x20 ? 4096 : op == 0x52 ? 32768 :
                            op == 0xd8 ? 65536 : NOR_SIZE;
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

