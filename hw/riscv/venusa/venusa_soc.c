/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CSK70xx shared address space and Nuclei interrupt integration. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/loader.h"
#include "system/address-spaces.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "venusa_rom.inc"

static unsigned active_hart(void)
{
    return current_cpu ? RISCV_CPU(current_cpu)->env.mhartid : 0;
}

G_NORETURN void venusa_fail(VenusASoC *s, hwaddr address, unsigned size,
                           bool write, uint64_t value)
{
    unsigned hart = active_hart();
    error_report("VenusA unsupported %s hart=%u pc=0x%08x address=0x%08"
                 HWADDR_PRIx " size=%u value=0x%" PRIx64,
                 write ? "write" : "read", hart,
                 (uint32_t)s->cpu[hart].env.pc, address, size, value);
    venusa_report(s, "unsupported");
    exit(1);
}

void venusa_irq(VenusASoC *s, unsigned irq, bool value)
{
    /* Until CIDU routing is configured, external interrupts target boot core. */
    nuclei_n300_irq(&s->cpu[0].env, irq, value);
}

static void timer_irq(void *opaque, unsigned irq, bool level)
{
    RISCVCPU *cpu = opaque;
    nuclei_n300_irq(&cpu->env, irq, level);
}

static uint64_t io_read(void *opaque, hwaddr address, unsigned size)
{
    VenusASoC *s = opaque;
    unsigned hart = active_hart();
    if (address >= 0xe0020000 && address < 0xe0022000) {
        return nuclei_n300_eclic_read(&s->cpu[hart].env, address - 0xe0020000, size);
    }
    for (unsigned i = 0; i < 3; i++) {
        hwaddr base = 0x43800000 + i * 0x100000;
        if (address >= base && address <= base + 32 &&
            (size == 4 || (address == base + 8 && size <= 2))) {
            return venusa_uart_read(s, i, address - base);
        }
    }
    if ((address >= 0x45500080 && address + size <= 0x455000c0) ||
        (address >= 0x45501000 && address + size <= 0x45501040)) {
        unsigned offset = address - (address >= 0x45501000 ? 0x45501000 : 0x45500080);
        uint64_t value = 0;
        for (unsigned i = 0; i < size; i++) { value |= (uint64_t)s->otp[offset + i] << (8 * i); }
        return value;
    }
    if (size != 4 || (address & 3)) {
        venusa_fail(s, address, size, false, 0);
    }
    if (address >= 0xe0030000 && address < 0xe0031000) {
        uint32_t value;
        if (nuclei_systimer_read(&s->timer[hart], address & 0xfff, &value)) {
            return value;
        }
    }
    uint32_t value;
    if (venusa_sysctl_read(s, address, &value)) { return value; }
    venusa_fail(s, address, size, false, 0);
}

static void io_write(void *opaque, hwaddr address, uint64_t value, unsigned size)
{
    VenusASoC *s = opaque;
    unsigned hart = active_hart();
    if (address >= 0xe0020000 && address < 0xe0022000) {
        nuclei_n300_eclic_write(&s->cpu[hart].env, address - 0xe0020000, value, size);
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        hwaddr base = 0x43800000 + i * 0x100000;
        if (address >= base && address <= base + 32 &&
            (size == 4 || (address == base + 8 && size <= 2))) {
            venusa_uart_write(s, i, address - base, value);
            return;
        }
    }
    if (size != 4 || (address & 3)) {
        venusa_fail(s, address, size, true, value);
    }
    if (address >= 0xe0030000 && address < 0xe0031000) {
        if (nuclei_systimer_write(&s->timer[hart], address & 0xfff, value)) {
            return;
        }
    }
    if (venusa_sysctl_write(s, address, value)) { return; }
    venusa_fail(s, address, size, true, value);
}

static const MemoryRegionOps io_ops = {
    .read = io_read, .write = io_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void reset(DeviceState *dev)
{
    VenusASoC *s = VENUSA_SOC(dev);
    for (unsigned i = 0; i < 2; i++) {
        cpu_reset(CPU(&s->cpu[i]));
        nuclei_n300_reset(&s->cpu[i]);
        s->cpu[i].env.pc = 0;
        CPU(&s->cpu[i])->halted = i != 0;
        nuclei_systimer_reset(&s->timer[i], 1000000);
    }
    venusa_sysctl_reset(s);
    venusa_storage_reset(s);
    listenai_dma_reset(&s->dma);
    venusa_peripherals_reset(s);
    listenai_psram_reset(&s->psram_ctrl);
    for (unsigned i = 0; i < 3; i++) {
        venusa_uart_reset(s, i);
    }
}

static void realize(DeviceState *dev, Error **errp)
{
    VenusASoC *s = VENUSA_SOC(dev);
    static const struct { const char *name; hwaddr base, size; } regions[] = {
        { "venusa-core0-ilm", 0x80000, 0x2000 },
        { "venusa-core0-dlm", 0x100000, 0x2000 },
        { "venusa-core1-ilm", 0x200000, 0x2000 },
        { "venusa-core1-dlm", 0x300000, 0x2000 },
        { "venusa-sram", 0x20000000, 0x80000 },
        { "venusa-luna-private", 0x20080000, 0x6000 },
    };
    memory_region_init_io(&s->io, OBJECT(s), &io_ops, s, "venusa-strict-mmio", UINT64_C(0x100000000));
    /* Peripheral DMA can synchronously access UART FIFO registers. */
    s->io.disable_reentrancy_guard = true;
    memory_region_add_subregion_overlap(get_system_memory(), 0, &s->io, -1);
    for (unsigned i = 0; i < G_N_ELEMENTS(regions); i++) {
        memory_region_init_ram(&s->ram[i], OBJECT(s), regions[i].name, regions[i].size, &error_fatal);
        memory_region_add_subregion(get_system_memory(), regions[i].base, &s->ram[i]);
    }
    memory_region_init_rom(&s->rom, OBJECT(s), "venusa-mask-rom", 0x8000, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0, &s->rom);
    memcpy(memory_region_get_ram_ptr(&s->rom), venusa_rom_bytes, sizeof(venusa_rom_bytes));
    const char *otp_path = getenv("LISEM_QEMU_OTP_IMAGE");
    if (otp_path) {
        g_autofree char *contents = NULL;
        gsize size;
        if (!g_file_get_contents(otp_path, &contents, &size, NULL) || size != sizeof(s->otp)) {
            error_setg(errp, "VenusA requires an exact 64-byte OTP image"); return;
        }
        memcpy(s->otp, contents, size);
    }
    venusa_storage_init(s);
    venusa_psram_init(s);
    for (unsigned i = 0; i < 2; i++) {
        object_initialize_child(OBJECT(s), i ? "core1" : "core0", &s->cpu[i], TYPE_RISCV_CPU_RV32I);
        static const char *exts[] = { "m", "a", "c", "f", "zicsr", "zicntr", "zifencei", "zba", "zbb", "zbc", "zbs" };
        if (!qtest_enabled()) {
            for (unsigned e = 0; e < G_N_ELEMENTS(exts); e++) {
                object_property_set_bool(OBJECT(&s->cpu[i]), exts[e], true, &error_fatal);
            }
        }
        object_property_set_bool(OBJECT(&s->cpu[i]), "mmu", false, &error_fatal);
        object_property_set_bool(OBJECT(&s->cpu[i]), "pmp", true, &error_fatal);
        s->cpu[i].cfg.max_satp_mode = VM_1_10_MBARE;
        s->cpu[i].satp_modes.init = s->cpu[i].satp_modes.map = 1;
        s->cpu[i].env.mhartid = i;
        qdev_prop_set_uint64(DEVICE(&s->cpu[i]), "resetvec", 0);
        qdev_realize(DEVICE(&s->cpu[i]), NULL, &error_fatal);
        nuclei_n300_init(&s->cpu[i], true, 83, 3, 1);
        nuclei_systimer_init(&s->timer[i], timer_irq, &s->cpu[i]);
        riscv_cpu_set_rdtime_fn(&s->cpu[i].env, nuclei_systimer_value, &s->timer[i]);
    }
    venusa_gpio_init(s);
    venusa_dma_init(s);
    venusa_uart_init(s);
    venusa_peripherals_init(s);
    venusa_runtime_init(s);
}

static const Property properties[] = {
    DEFINE_PROP_DRIVE("flash", VenusASoC, flash),
};

static void class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = realize;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, properties);
}

static const TypeInfo soc_type = {
    .name = TYPE_VENUSA_SOC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VenusASoC), .class_init = class_init,
};
static void register_types(void) { type_register_static(&soc_type); }
type_init(register_types)
