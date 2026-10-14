/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared serial-memory controller, independent of board chip-select wiring. */
#include "qemu/osdep.h"
#include "hw/ssi/listenai_spimem.h"
#include "system/address-spaces.h"

static G_NORETURN void spimem_fault(ListenAISpiMem *s, hwaddr offset,
                                   unsigned size, bool write, uint64_t value)
{
    s->fault(s->opaque, s->address + offset, size, write, value);
    abort();
}

static void spimem_irq(ListenAISpiMem *s)
{
    s->irq(s->opaque, !!(s->regs[0x38 / 4] & s->regs[0x3c / 4]));
}

static void execute(ListenAISpiMem *s)
{
    uint32_t control = s->regs[0x20 / 4];
    unsigned mode = (control >> 24) & 15;
    if (mode != 1 && mode != 2 && mode != 3 && mode != 5 && mode != 7 && mode != 9) {
        spimem_fault(s, 0x20, 4, true, control);
    }
    bool writes = mode == 1 || mode == 3 || mode == 5;
    bool reads = mode == 2 || mode == 3 || mode == 5 || mode == 9;
    unsigned count = writes ? ((control >> 12) & 511) + 1 : 0;
    unsigned length = reads ? (control & 511) + 1 : 0;
    if (s->tx_count < count) { return; }
    uint8_t opcode = s->regs[0x24 / 4], *data = s->tx;
    if (!(control & (1u << 30))) {
        if (!count) { spimem_fault(s, 0x20, 4, true, control); }
        opcode = *data++; count--;
    }
    uint8_t result[512] = { 0 };
    unsigned received;
    if (length > sizeof(s->rx) - s->rx_count ||
        !listenai_nor_command(s->select(s->opaque, s->regs[0x28 / 4]), opcode, s->regs[0x28 / 4] & (LISTENAI_NOR_SIZE - 1),
                     data, count, result, length, &received)) {
        spimem_fault(s, 0x24, 4, true, opcode);
    }
    for (unsigned i = 0; i < received; i++) {
        s->rx[(s->rx_head + s->rx_count++) % sizeof(s->rx)] = result[i];
    }
    s->tx_count = 0; s->pending = false;
    s->regs[0x3c / 4] |= 16;
    spimem_irq(s);
}

static unsigned flash_fifo_bytes(ListenAISpiMem *s)
{
    unsigned bits = ((s->regs[0x10 / 4] >> 8) & 31) + 1;
    if (bits % 8) {
        spimem_fault(s, 0x10, 4, true, s->regs[0x10 / 4]);
    }
    return s->regs[0x10 / 4] & 0x80 ? 4 : bits / 8;
}

static uint64_t flash_read(void *opaque, hwaddr off, unsigned size)
{
    ListenAISpiMem *s = opaque;
    if (size != 4 || (off & 3) || off >= sizeof(s->regs) || !s->valid(s->opaque, off, false)) {
        spimem_fault(s, off, size, false, 0);
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
    ListenAISpiMem *s = opaque;
    if (size != 4 || (off & 3) || off >= sizeof(s->regs) || !s->valid(s->opaque, off, true)) { goto fail; }
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
    spimem_irq(s);
    return;
fail:
    spimem_fault(s, off, size, true, value);
}

static const MemoryRegionOps spimem_ops = {
    .read = flash_read, .write = flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_spimem_init(ListenAISpiMem *s, Object *owner, const char *name,
                          hwaddr address, ListenAISpiMemSelect select,
                          ListenAISpiMemValid valid, ListenAISpiMemIrq irq,
                          ListenAISpiMemFault fault, void *opaque)
{
    s->address = address;
    s->select = select;
    s->valid = valid;
    s->irq = irq;
    s->fault = fault;
    s->opaque = opaque;
    memory_region_init_io(&s->io, owner, &spimem_ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), address, &s->io);
}

void listenai_spimem_reset(ListenAISpiMem *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x10 / 4] = 0x20780;
    s->tx_count = s->rx_count = s->rx_head = 0;
    s->pending = false;
    spimem_irq(s);
}
