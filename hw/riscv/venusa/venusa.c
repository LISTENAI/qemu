/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA RD EVB board wiring. */
#include "qemu/osdep.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/riscv/venusa/venusa.h"
#include "qapi/error.h"
#include "hw/core/listenai-board.h"
#include "hw/core/listenai-board-inputs.h"
#include "hw/i2c/listenai_extio.h"
#include "hw/display/st7789.h"
#include "hw/i2c/cst328.h"
#include "hw/irq.h"
#include "ui/lisa_display.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "system/reset.h"
#include "qapi/visitor.h"
#include "qobject/qbool.h"
#include "qemu/cutils.h"
#define TYPE_VENUSA_BOARD MACHINE_TYPE_NAME("venusa-board")
#define TYPE_VENUSA_RD_BOARD MACHINE_TYPE_NAME("venusa-rd-evb")
OBJECT_DECLARE_SIMPLE_TYPE(VenusABoard, VENUSA_RD_BOARD)
struct VenusABoard {
    MachineState parent_obj;
    ListenaiBoard components;
    VenusASoC *soc;
    DeviceState *screen, *camera;
    ListenaiBoardInputs inputs;
};

static void backlight(void *opaque, int unused, int level)
{
    VenusABoard *s = opaque;
    unsigned pad = s->components.backlight_pad;
    if (pad >= 64) {
        double duty = listenai_extio_duty(s->components.extio, pad - 64);
        st7789_backlight(s->screen, s->components.backlight_active ? duty : 1 - duty);
        return;
    }
    uint32_t driven, levels;
    listenai_gpio_outputs_snapshot(&s->soc->gpio[pad / 32], &driven, &levels);
    double duty = (driven & (1u << (pad % 32))) ? !!(levels & (1u << (pad % 32))) : 0;
    if (pad == 14 && listenai_pinmux_function(&s->soc->pinmux, pad, 11)) {
        duty = venusa_gpt_duty(&s->soc->gpt, 0);
    }
    st7789_backlight(s->screen, s->components.backlight_active ? duty : 1 - duty);
}

static bool extio_route(void *opaque)
{
    VenusABoard *s = opaque;
    return listenai_pinmux_function(&s->soc->pinmux, s->components.extio_scl, 8) &&
           listenai_pinmux_function(&s->soc->pinmux, s->components.extio_sda, 8);
}

static bool typec_route(void *opaque)
{
    VenusABoard *s = opaque;
    return listenai_pinmux_function(&s->soc->pinmux, s->components.typec_scl, 8) &&
           listenai_pinmux_function(&s->soc->pinmux, s->components.typec_sda, 8);
}

static void panel_connect(VenusABoard *s)
{
    ListenaiBoard *b = &s->components;
    if (!b->panel) { return; }
    s->screen = listenai_board_panel_new(b, OBJECT(s));
    ListenaiSPI *controllers[] = {&s->soc->spi[0].device, &s->soc->spi[1].device};
    void *opaque[] = {&s->soc->spi[0], &s->soc->spi[1]};
    listenai_board_attach_spi(b, DEVICE(s->soc), s->screen, controllers, opaque, 2,
                              venusa_spi_pad_selected);
    listenai_board_panel_wire(b, DEVICE(s->soc), s->screen);
    listenai_board_connect_gpio(b, DEVICE(s->soc), b->backlight_pad,
                               qemu_allocate_irq(backlight, s, 0));
    const char *directory = getenv("LISEM_QEMU_DESKTOP");
    if (directory) {
        lisa_display_init(qemu_console_lookup_by_device(s->screen, 0), directory,
                          getenv("LISEM_QEMU_DISPLAY_SHM"));
    }
}
#include "system/blockdev.h"
#include "qemu/error-report.h"

static bool touch_route(void *opaque)
{
    VenusABoard *s = opaque;
    return listenai_pinmux_function(&s->soc->pinmux, s->components.touch_scl, 8) &&
           listenai_pinmux_function(&s->soc->pinmux, s->components.touch_sda, 8);
}

static void touch_connect(VenusABoard *s)
{
    ListenaiBoard *b = &s->components;
    if (!b->touch) { return; }
    if (b->touch_scl != 34 || b->touch_sda != 33) {
        error_report("Unsupported VenusA touch I2C route"); exit(1);
    }
    DeviceState *dev = DEVICE(i2c_slave_new(TYPE_CST328, 0x1d));
    object_property_add_child(OBJECT(s), "touch", OBJECT(dev));
    i2c_slave_realize_and_unref(I2C_SLAVE(dev), s->soc->i2c[1].bus, &error_fatal);
    qdev_connect_gpio_out_named(dev, "int", 0,
        qdev_get_gpio_in_named(DEVICE(s->soc), "pad-in", b->touch_irq));
    cst328_add_input(OBJECT(s), dev, "x-lisa-touch");
    if (!b->camera) {
        s->soc->i2c[1].route_valid = touch_route;
        s->soc->i2c[1].route_opaque = s;
    }
}

static char *snapshot(Object *obj, Error **errp)
{
    VenusABoard *s = VENUSA_RD_BOARD(obj);
    g_autofree char *raw = venusa_snapshot(s->soc, "running");
    QObject *root = qobject_from_json(raw, errp);
    if (!root) { return NULL; }
    QDict *dict = qobject_to(QDict, root), *banks = qdict_new(), *buttons = qdict_new();
    for (unsigned i = 0; i < 2; i++) {
        uint32_t driven, levels;
        listenai_gpio_outputs_snapshot(&s->soc->gpio[i], &driven, &levels);
        QDict *bank = qdict_new();
        qdict_put_int(bank, "driven", driven); qdict_put_int(bank, "levels", levels);
        qdict_put_obj(banks, i ? "B" : "A", QOBJECT(bank));
    }
    for (unsigned i = 0; i < s->components.button_count; i++) {
        ListenaiBoardButton *b = &s->components.buttons[i];
        qdict_put_bool(buttons, b->id, b->pressed);
    }
    QDict *controls = qdict_new();
    qdict_put_obj(controls, "buttons", QOBJECT(buttons));
    g_autofree char *camera = venusa_board_camera_status(s->camera);
    qdict_put_obj(dict, "camera_input", qobject_from_json(camera, &error_abort));
    qdict_put_obj(dict, "devices", QOBJECT(listenai_board_devices_snapshot(&s->components)));
    qdict_put_obj(dict, "pads", QOBJECT(banks));
    qdict_put_obj(dict, "controls", QOBJECT(controls));
    GString *text = qobject_to_json(root);
    qobject_unref(root);
    return g_string_free(text, false);
}

static void button_get(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    ListenaiBoardButton *b = opaque;
    visit_type_bool(v, name, &b->pressed, errp);
}

static void button_set(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    ListenaiBoardButton *b = opaque;
    VenusABoard *s = b->owner;
    bool pressed;
    if (!visit_type_bool(v, name, &pressed, errp)) { return; }
    b->pressed = pressed;
    listenai_board_gpio_input(&s->components, DEVICE(s->soc), b->pad,
                              pressed ? b->pressed_level : b->released_level);
}

static void signals_set(Object *obj, const char *value, Error **errp)
{
    VenusABoard *s = VENUSA_RD_BOARD(obj);
    if (listenai_board_inputs_parse(&s->components, &s->inputs, value, 32, 22, 0, 6, errp)) {
        listenai_board_inputs_apply(&s->components, &s->inputs, DEVICE(s->soc));
        for (unsigned i = 0; i < 6; i++) {
            if (s->inputs.adc_mask & (1u << i)) { venusa_gpadc_set_voltage(&s->soc->gpadc, i, s->inputs.adc_uv[i]); }
        }
    }
}

static void board_reset(void *opaque)
{
    VenusABoard *s = opaque;
    ListenaiBoard *b = &s->components;
    listenai_board_pulls_apply(b, DEVICE(s->soc));
    for (unsigned i = 0; i < b->button_count; i++) {
        listenai_board_gpio_input(b, DEVICE(s->soc), b->buttons[i].pad,
                                  b->buttons[i].pressed ? b->buttons[i].pressed_level : b->buttons[i].released_level);
    }
    listenai_board_inputs_apply(b, &s->inputs, DEVICE(s->soc));
    if (s->screen) { backlight(s, 0, 0); }
    listenai_signal_trace_stop(&b->signal_trace, "reset");
}

static void venusa_board_init(MachineState *machine)
{
    VenusABoard *board = VENUSA_RD_BOARD(machine);
    ListenaiBoard *b = &board->components;
    const char *path = getenv("LISEM_QEMU_BOARD");
    if (path) { listenai_board_load(b, path, "venusa", 32, 22); }
    if (!strcmp(object_get_typename(OBJECT(machine)), TYPE_VENUSA_BOARD) && !b->configured) {
        error_report("venusa-board requires a checked board assembly file"); exit(1);
    }
    DeviceState *soc = qdev_new(TYPE_VENUSA_SOC);
    board->soc = VENUSA_SOC(soc);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    /* RD EVB boot/UART pad PB5 is pulled high on the board. */
    VENUSA_SOC(soc)->board_inputs[1] = b->configured ? b->pull_levels >> 32 : 1u << 5;
    VENUSA_SOC(soc)->board_inputs[0] = b->pull_levels;
    if (machine->firmware || machine->kernel_filename) {
        error_report("VenusA uses a fixed mask ROM and a pflash image"); exit(1);
    }
    DriveInfo *flash = drive_get(IF_PFLASH, 0, 0);
    if (!flash) { error_report("VenusA requires a pflash image"); exit(1); }
    qdev_prop_set_drive_err(soc, "flash", blk_by_legacy_dinfo(flash), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);
    if (b->expander) {
        if (b->extio_scl != 34 || b->extio_sda != 33) {
            error_report("Unsupported VenusA EXTIO I2C route"); exit(1);
        }
        listenai_board_attach_extio(b, OBJECT(board), board->soc->i2c[1].bus);
        board->soc->i2c[1].route_valid = extio_route;
        board->soc->i2c[1].route_opaque = board;
    }
    listenai_signal_trace_init(&b->signal_trace, OBJECT(board), OBJECT(board->soc),
                              board->soc->gpio,
                              32, 22, b->extio);
    board->soc->signal_trace = &b->signal_trace;
    if (b->typec) {
        if (b->typec_scl != 34 || b->typec_sda != 33 || b->typec_host) {
            error_report("Unsupported VenusA HUSB320 route or USB host transport"); exit(1);
        }
        listenai_board_attach_typec(b, OBJECT(board), soc, board->soc->i2c[1].bus);
        board->soc->i2c[1].route_valid = typec_route;
        board->soc->i2c[1].route_opaque = board;
    }
    listenai_board_sd_wire(b, DEVICE(board->soc), board->soc->sd.host);
    panel_connect(board);
    touch_connect(board);
    board->camera = venusa_board_camera_connect(board->soc, OBJECT(board), b);
    for (unsigned i = 0; i < b->button_count; i++) {
        b->buttons[i].owner = board;
        g_autofree char *name = g_strdup_printf("x-lisa-button-%u", i);
        object_property_add(OBJECT(machine), name, "bool", button_get, button_set,
                            NULL, &b->buttons[i]);
    }
    qemu_register_reset(board_reset, board);
    venusa_board_audio_connect(VENUSA_SOC(soc),
        b->configured ? (b->amplifier ? b->amplifier_pad : UINT_MAX) : 0,
        b->configured ? b->amplifier_active : true, b->extio);
    object_property_add_str(OBJECT(machine), "x-lisa-snapshot", snapshot, NULL);
    object_property_add_str(OBJECT(machine), "x-lisa-signals", NULL, signals_set);
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

static void board_finalize(Object *obj)
{
    listenai_board_cleanup(&VENUSA_RD_BOARD(obj)->components);
}

static const TypeInfo venusa_board_type = {
    .name = MACHINE_TYPE_NAME("venusa-rd-evb"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(VenusABoard),
    .instance_finalize = board_finalize,
    .class_init = venusa_board_class_init,
};

static const TypeInfo configured_board_type = {
    .name = TYPE_VENUSA_BOARD,
    .parent = MACHINE_TYPE_NAME("venusa-rd-evb"),
};

static void venusa_register_types(void)
{
    type_register_static(&venusa_board_type);
    type_register_static(&configured_board_type);
}
type_init(venusa_register_types)
