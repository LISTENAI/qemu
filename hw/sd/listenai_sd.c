/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared FTSDC021-compatible host glue; board routing stays in each SoC. */
#include "qemu/osdep.h"
#include "hw/sd/listenai_sd.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

static G_NORETURN void sd_fail(ListenAISD *s, hwaddr off, unsigned size,
                               bool write, uint64_t value)
{
    s->fault(s->opaque, s->address + off, size, write, value);
    abort(); /* A fault callback must terminate the unsupported transaction. */
}

/* Standard SDHCI registers plus the ListenAI vendor register bank. */
static bool sd_byte_valid(hwaddr off)
{
    return off < 0x180 && !(off >= 0x70 && off < 0xfc) && !(off >= 0x12c && off < 0x178);
}

static uint64_t sd_read(void *opaque, hwaddr off, unsigned size)
{
    ListenAISD *s = opaque;
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        if (!sd_byte_valid(off + i)) { sd_fail(s, off, size, false, 0); }
    }
    if (off < 0x100) {
        if (memory_region_dispatch_read(sysbus_mmio_get_region(SYS_BUS_DEVICE(s->host), 0),
                off, &value, ctz32(size) | MO_LE, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            sd_fail(s, off, size, false, 0);
        }
    } else {
        for (unsigned i = 0; i < size; i++) { value |= (uint64_t)s->regs[off + i] << (8 * i); }
    }
    return value;
}

static void sd_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenAISD *s = opaque;
    for (unsigned i = 0; i < size; i++) {
        if (!sd_byte_valid(off + i)) { sd_fail(s, off, size, true, value); }
    }
    if (off < 0x100) {
        if (memory_region_dispatch_write(sysbus_mmio_get_region(SYS_BUS_DEVICE(s->host), 0),
                off, value, ctz32(size) | MO_LE, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            sd_fail(s, off, size, true, value);
        }
    } else {
        for (unsigned i = 0; i < size; i++) { s->regs[off + i] = value >> (8 * i); }
    }
}

static const MemoryRegionOps sd_ops = {
    .read = sd_read, .write = sd_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_sd_reset(ListenAISD *sd)
{
    memset(sd->regs, 0, sizeof(sd->regs));
    if (sd->host) { device_cold_reset(sd->host); }
}

void listenai_sd_init(ListenAISD *sd, Object *owner, const char *name,
                      hwaddr address, uint64_t capabilities, bool no_card_detect,
                      BlockBackend *backend, qemu_irq irq,
                      ListenAISDFault fault, void *opaque)
{
    sd->address = address;
    sd->fault = fault;
    sd->opaque = opaque;
    sd->host = qdev_new(TYPE_SYSBUS_SDHCI);
    object_property_add_child(owner, "sd-host", OBJECT(sd->host));
    qdev_prop_set_uint8(sd->host, "sd-spec-version", 3);
    qdev_prop_set_uint64(sd->host, "capareg", capabilities);
    qdev_prop_set_bit(sd->host, "no-card-detect", no_card_detect);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sd->host), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(sd->host), 0, irq);
    DeviceState *card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", backend, &error_fatal);
    qdev_realize_and_unref(card, qdev_get_child_bus(sd->host, "sd-bus"), &error_fatal);
    if (backend) {
        blk_set_perm(backend, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_CONSISTENT_READ, &error_fatal);
    }
    memory_region_init_io(&sd->io, owner, &sd_ops, sd, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), address, &sd->io);
}

void listenai_sd_enable_unadvertised_sdma(ListenAISD *sd)
{
    SYSBUS_SDHCI(sd->host)->quirks |= SDHCI_QUIRK_SDMA_CAP_MISSING;
}
