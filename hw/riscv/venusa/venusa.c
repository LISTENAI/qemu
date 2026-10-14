/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA RD EVB board wiring. */
#include "qemu/osdep.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/venusa/venusa.h"
#include "qapi/error.h"
#include "system/blockdev.h"
#include "qemu/error-report.h"

static char *snapshot(Object *obj, Error **errp)
{
    return venusa_snapshot(VENUSA_SOC(object_resolve_path_component(obj, "soc")), "running");
}

static void venusa_board_init(MachineState *machine)
{
    DeviceState *soc = qdev_new(TYPE_VENUSA_SOC);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    /* RD EVB boot/UART pad PB5 is pulled high on the board. */
    VENUSA_SOC(soc)->board_inputs[1] = 1u << 5;
    if (machine->firmware || machine->kernel_filename) {
        error_report("VenusA uses a fixed mask ROM and a pflash image"); exit(1);
    }
    DriveInfo *flash = drive_get(IF_PFLASH, 0, 0);
    if (!flash) { error_report("VenusA requires a pflash image"); exit(1); }
    qdev_prop_set_drive_err(soc, "flash", blk_by_legacy_dinfo(flash), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);
    venusa_board_audio_connect(VENUSA_SOC(soc));
    object_property_add_str(OBJECT(machine), "x-lisa-snapshot", snapshot, NULL);
}

static void venusa_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "ListenAI VenusA RD EVB (CSK7004J8U)";
    mc->init = venusa_board_init;
    mc->default_cpu_type = TYPE_RISCV_CPU_RV32I;
    mc->min_cpus = mc->max_cpus = mc->default_cpus = 2;
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id = "venusa-unused";
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
}

static const TypeInfo venusa_board_type = {
    .name = MACHINE_TYPE_NAME("venusa-rd-evb"),
    .parent = TYPE_MACHINE,
    .class_init = venusa_board_class_init,
};

static void venusa_register_types(void)
{
    type_register_static(&venusa_board_type);
}
type_init(venusa_register_types)
