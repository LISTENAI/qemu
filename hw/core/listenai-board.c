/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Board components and connections, independent of the SoC address map. */
#include "qemu/osdep.h"
#include "hw/core/listenai-board.h"
#include "hw/irq.h"
#include "hw/display/st7789.h"
#include "hw/i2c/listenai_extio.h"
#include "hw/i2c/husb320.h"
#include "hw/sd/sdhci.h"
#include "hw/core/split-irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qbool.h"
#include "qobject/qlist.h"
#include "qemu/error-report.h"

static G_NORETURN void invalid(const char *field)
{
    error_report("Invalid board assembly field: %s", field);
    exit(1);
}

static unsigned number(QDict *dict, const char *field, unsigned maximum)
{
    uint64_t n;
    QNum *value = dict ? qobject_to(QNum, qdict_get(dict, field)) : NULL;
    if (!value || !qnum_get_try_uint(value, &n) || n > maximum) {
        invalid(field);
    }
    return n;
}

static bool boolean(QDict *dict, const char *field)
{
    QBool *value = dict ? qobject_to(QBool, qdict_get(dict, field)) : NULL;
    if (!value) { invalid(field); }
    return qbool_get_bool(value);
}

static unsigned pad(QDict *dict, const char *field, unsigned pa, unsigned pb)
{
    unsigned n = number(dict, field, 63);
    if ((n < 32 && n >= pa) || (n >= 32 && n >= 32 + pb)) { invalid(field); }
    return n;
}

static unsigned gpio_endpoint(ListenaiBoard *b, QDict *dict, const char *field, unsigned pa, unsigned pb)
{
    unsigned n = number(dict, field, 95);
    if (n < 64) { return pad(dict, field, pa, pb); }
    unsigned pin = n - 64;
    static const unsigned masks[] = {6, 0, 0x9f, 0x7f};
    if (!b->expander || !(masks[pin / 8] & (1u << (pin % 8)))) { invalid(field); }
    return n;
}

void listenai_board_load(ListenaiBoard *b, const char *path, const char *chip,
                         unsigned pa_pads, unsigned pb_pads)
{
    g_autofree char *text = NULL;
    gsize length;
    if (!g_file_get_contents(path, &text, &length, NULL) || length > 65536) {
        invalid("file");
    }
    QObject *root = qobject_from_json(text, &error_fatal);
    QDict *dict = qobject_to(QDict, root);
    if (!dict || number(dict, "version", 1) != 1) { invalid("version"); }
    const char *model = qdict_get_try_str(dict, "chip");
    if (!model || strcmp(model, chip)) { invalid("chip"); }
    b->expander = qdict_haskey(dict, "extio");
    if (b->expander) {
        QDict *p = qdict_get_qdict(dict, "extio");
        const char *id = p ? qdict_get_try_str(p, "id") : NULL;
        if (!id || !*id || strlen(id) > 64) { invalid("extio ID"); }
        b->extio_id = g_strdup(id);
        b->extio_scl = pad(p, "SCL", pa_pads, pb_pads);
        b->extio_sda = pad(p, "SDA", pa_pads, pb_pads);
        b->extio_address = number(p, "address", 0x7f);
        if (b->extio_address != 0x6c && b->extio_address != 0x15) { invalid("extio address strap"); }
    }
    b->configured = true;
    b->typec = qdict_haskey(dict, "typec");
    if (b->typec) {
        QDict *p = qdict_get_qdict(dict, "typec");
        b->typec_scl = pad(p, "SCL", pa_pads, pb_pads);
        b->typec_sda = pad(p, "SDA", pa_pads, pb_pads);
        b->typec_address = number(p, "address", 0x7f);
        if (b->typec_address != 0x31 && b->typec_address != 0x21) { invalid("HUSB320 address strap"); }
        b->typec_host = boolean(p, "usb_host");
        b->typec_irq = qdict_haskey(p, "INT_N") ? gpio_endpoint(b, p, "INT_N", pa_pads, pb_pads) : UINT_MAX;
        b->typec_id_pin = qdict_haskey(p, "ID") ? gpio_endpoint(b, p, "ID", pa_pads, pb_pads) : UINT_MAX;
    }
    b->panel = qdict_haskey(dict, "panel");
    b->camera = qdict_haskey(dict, "camera");
    b->sd = qdict_haskey(dict, "sd");
    b->amplifier = qdict_haskey(dict, "amplifier");
    if (b->panel) {
        QDict *p = qdict_get_qdict(dict, "panel");
        QDict *pins = p ? qdict_get_qdict(p, "pads") : NULL;
        b->lcd_sck = pad(pins, "SCK", pa_pads, pb_pads); b->lcd_sdi = pad(pins, "SDI", pa_pads, pb_pads);
        b->lcd_cs = gpio_endpoint(b, pins, "CS", pa_pads, pb_pads); b->lcd_dc = gpio_endpoint(b, pins, "DC", pa_pads, pb_pads);
        b->lcd_reset = gpio_endpoint(b, pins, "RESET", pa_pads, pb_pads); b->lcd_te = pad(pins, "TE", pa_pads, pb_pads);
        b->lcd_width = number(p, "width", 240);
        b->lcd_height = number(p, "height", 320);
        b->lcd_x = number(p, "x_offset", 239); b->lcd_y = number(p, "y_offset", 319);
        b->lcd_rotation = number(p, "rotation", 270);
        if (!b->lcd_width || !b->lcd_height || b->lcd_rotation % 90 ||
            b->lcd_x + b->lcd_width > 240 || b->lcd_y + b->lcd_height > 320) {
            invalid("panel viewport");
        }
        b->lcd_inverted = boolean(p, "inverted");
        b->backlight_pad = gpio_endpoint(b, p, "backlight_pad", pa_pads, pb_pads);
        b->backlight_active = number(p, "backlight_active", 1);
    }
    if (b->camera) {
        QDict *p = qdict_get_qdict(dict, "camera");
        QDict *pins = p ? qdict_get_qdict(p, "pads") : NULL;
        static const char *names[] = {"SCL", "SDA", "MCLK", "PCLK", "HSYNC",
            "VSYNC", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};
        for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
            b->camera_pads[i] = pad(pins, names[i], pa_pads, pb_pads);
        }
        b->camera_pads[LISTENAI_CAM_RESET] = qdict_haskey(pins, "RESET") ? gpio_endpoint(b, pins, "RESET", pa_pads, pb_pads) : UINT_MAX;
        b->camera_pads[LISTENAI_CAM_PWDN] = qdict_haskey(pins, "PWDN") ? gpio_endpoint(b, pins, "PWDN", pa_pads, pb_pads) : UINT_MAX;
    }
    if (b->amplifier) {
        QDict *p = qdict_get_qdict(dict, "amplifier");
        b->amplifier_pad = gpio_endpoint(b, p, "pad", pa_pads, pb_pads);
        b->amplifier_active = number(p, "active_level", 1);
    }
    if (b->sd) {
        QDict *p = qdict_get_qdict(dict, "sd");
        QDict *pins = p ? qdict_get_qdict(p, "pads") : NULL;
        b->sd_detect = qdict_haskey(p, "card_detect") && boolean(p, "card_detect");
        b->sd_external_detect = qdict_haskey(p, "detect_endpoint");
        if (b->sd_external_detect) {
            b->sd_detect_endpoint = gpio_endpoint(b, p, "detect_endpoint", pa_pads, pb_pads);
            b->sd_detect_active = number(p, "detect_active_level", 1);
        }
        static const char *names[] = {"CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3"};
        for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
            pad(pins, names[i], pa_pads, pb_pads);
        }
    }
    b->touch = qdict_haskey(dict, "touch");
    if (b->touch) {
        QDict *p = qdict_get_qdict(dict, "touch");
        b->touch_scl = pad(p, "SCL", pa_pads, pb_pads);
        b->touch_sda = pad(p, "SDA", pa_pads, pb_pads);
        b->touch_irq = pad(p, "INT", pa_pads, pb_pads);
    }
    b->keypad = qdict_haskey(dict, "keypad");
    if (b->keypad) {
        QDict *p = qdict_get_qdict(dict, "keypad");
        b->keypad_channel = number(p, "channel", 15);
        b->keypad_supply_uv = number(p, "supply_uv", 5000000);
        b->keypad_pull_up_ohms = number(p, "pull_up_ohms", 1000000);
        if (!b->keypad_supply_uv || !b->keypad_pull_up_ohms) { invalid("keypad supply/pull-up"); }
    }
    QList *pulls = qdict_get_qlist(dict, "pulls");
    const QListEntry *pull_entry;
    if (pulls) {
        QLIST_FOREACH_ENTRY(pulls, pull_entry) {
            QDict *p = qobject_to(QDict, qlist_entry_obj(pull_entry));
            unsigned n = gpio_endpoint(b, p, "pad", pa_pads, pb_pads);
            b->pull_mask |= UINT64_C(1) << n;
            if (boolean(p, "level")) { b->pull_levels |= UINT64_C(1) << n; }
        }
    }
    QList *buttons = qdict_get_qlist(dict, "buttons");
    if (!buttons || qlist_size(buttons) > LISTENAI_BOARD_BUTTONS) { invalid("buttons"); }
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(buttons, entry) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(entry));
        const char *id = p ? qdict_get_try_str(p, "id") : NULL;
        if (!id || !*id || strlen(id) > 64) { invalid("button ID"); }
        ListenaiBoardButton *button = &b->buttons[b->button_count++];
        button->id = g_strdup(id);
        button->pad = gpio_endpoint(b, p, "pad", pa_pads, pb_pads);
        button->analog = qdict_haskey(p, "resistance_ohms");
        if (button->analog) {
            if (!b->keypad) { invalid("button ADC receiver"); }
            button->resistance_ohms = number(p, "resistance_ohms", 1000000);
        }
        button->pressed_level = number(p, "pressed_level", 1);
        button->released_level = number(p, "released_level", 1);
        if (button->pressed_level == button->released_level) { invalid("button polarity"); }
    }
    qobject_unref(root);
}

static SSIBus *panel_bus(void *opaque)
{
    typeof(((ListenaiBoard *)0)->spi_routes[0]) *route = opaque;
    ListenaiBoard *b = route->board;
    ListenaiSPI *spi = route->spi;
    /* CS/DC/RESET are actual digital nets and may be driven as GPIO. */
    return route->selected(route->opaque, LISTENAI_SPI_CLOCK, b->lcd_sck) &&
           route->selected(route->opaque, LISTENAI_SPI_MOSI, b->lcd_sdi)
        ? b->panel_bus : spi->bus;
}

void listenai_board_attach_spi(ListenaiBoard *b, DeviceState *soc,
                              DeviceState *panel, ListenaiSPI **controllers,
                              void **opaque, unsigned count,
                              bool (*selected)(void *, unsigned, unsigned))
{
    assert(count <= ARRAY_SIZE(b->spi_routes));
    b->panel_bus = ssi_create_bus(soc, "board-lcd-spi");
    ssi_realize_and_unref(panel, b->panel_bus, &error_fatal);
    for (unsigned i = 0; i < count; i++) {
        b->spi_routes[i].board = b;
        b->spi_routes[i].spi = controllers[i];
        b->spi_routes[i].opaque = opaque[i];
        b->spi_routes[i].selected = selected;
        controllers[i]->board_bus = panel_bus;
        controllers[i]->board_opaque = &b->spi_routes[i];
    }
}

void listenai_board_connect_pad(DeviceState *soc, unsigned pad, qemu_irq input)
{
    qemu_irq previous = qdev_get_gpio_out_connector(DEVICE(soc), "pad-out", pad);
    if (previous) {
        DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);
        qdev_prop_set_uint16(split, "num-lines", 2);
        qdev_realize_and_unref(split, NULL, &error_fatal);
        qdev_connect_gpio_out(split, 0, previous);
        qdev_connect_gpio_out(split, 1, input);
        input = qdev_get_gpio_in(split, 0);
    }
    qdev_connect_gpio_out_named(DEVICE(soc), "pad-out", pad, input);
}

void listenai_board_cleanup(ListenaiBoard *b)
{
    listenai_signal_trace_destroy(&b->signal_trace);
    for (unsigned i = 0; i < b->button_count; i++) { g_free(b->buttons[i].id); }
    g_free(b->extio_id);
}

void listenai_board_attach_typec(ListenaiBoard *b, Object *owner, DeviceState *soc, I2CBus *bus)
{
    if (!b->typec) { return; }
    DeviceState *dev = qdev_new(TYPE_HUSB320_BA);
    b->typec_device = dev;
    object_property_add_child(owner, "typec", OBJECT(dev));
    qdev_prop_set_uint8(dev, "address", b->typec_address);
    qdev_realize_and_unref(dev, BUS(bus), &error_fatal);
    if (b->typec_irq != UINT_MAX) {
        qdev_connect_gpio_out_named(dev, "int-n", 0, b->typec_irq < 64 ?
            qdev_get_gpio_in_named(soc, "pad-in", b->typec_irq) :
            qdev_get_gpio_in_named(b->extio, "pin-in", b->typec_irq - 64));
    }
    if (b->typec_id_pin != UINT_MAX) {
        qdev_connect_gpio_out_named(dev, "id", 0, b->typec_id_pin < 64 ?
            qdev_get_gpio_in_named(soc, "pad-in", b->typec_id_pin) :
            qdev_get_gpio_in_named(b->extio, "pin-in", b->typec_id_pin - 64));
    }
    if (b->typec_host) {
        qdev_connect_gpio_out_named(soc, "usb-host-connected", 0,
            qdev_get_gpio_in_named(dev, "host-connected", 0));
    }
}

DeviceState *listenai_board_panel_new(ListenaiBoard *b, Object *owner)
{
    DeviceState *panel = qdev_new(TYPE_ST7789);
    b->panel_device = panel;
    object_property_add_child(owner, "screen", OBJECT(panel));
    qdev_prop_set_uint32(panel, "width", b->lcd_width);
    qdev_prop_set_uint32(panel, "height", b->lcd_height);
    qdev_prop_set_uint32(panel, "x-offset", b->lcd_x);
    qdev_prop_set_uint32(panel, "y-offset", b->lcd_y);
    qdev_prop_set_uint32(panel, "rotation", b->lcd_rotation);
    qdev_prop_set_bit(panel, "panel-inverted", b->lcd_inverted);
    return panel;
}

void listenai_board_panel_wire(ListenaiBoard *b, DeviceState *soc, DeviceState *panel)
{
    listenai_board_connect_gpio(b, soc, b->lcd_cs, qdev_get_gpio_in_named(panel, SSI_GPIO_CS, 0));
    listenai_board_connect_gpio(b, soc, b->lcd_dc, qdev_get_gpio_in(panel, 0));
    listenai_board_connect_gpio(b, soc, b->lcd_reset, qdev_get_gpio_in(panel, 1));
    qdev_connect_gpio_out_named(panel, "te", 0, qdev_get_gpio_in_named(soc, "pad-in", b->lcd_te));
}

void listenai_board_camera_wire(ListenaiBoard *b, DeviceState *soc, DeviceState *sensor)
{
    static const unsigned controls[] = {LISTENAI_CAM_RESET, LISTENAI_CAM_PWDN};
    for (unsigned i = 0; i < ARRAY_SIZE(controls); i++) {
        unsigned pad = b->camera_pads[controls[i]];
        if (pad != UINT_MAX) {
            listenai_board_connect_gpio(b, soc, pad, qdev_get_gpio_in_named(sensor, "control", i));
        }
    }
}

void listenai_board_attach_extio(ListenaiBoard *b, Object *owner, I2CBus *bus)
{
    if (!b->expander) { return; }
    I2CSlave *dev = i2c_slave_new(TYPE_LISTENAI_EXTIO, b->extio_address);
    b->extio = DEVICE(dev);
    object_property_add_child(owner, "extio", OBJECT(dev));
    i2c_slave_realize_and_unref(dev, bus, &error_fatal);
}

void listenai_board_connect_gpio(ListenaiBoard *b, DeviceState *soc, unsigned endpoint, qemu_irq input)
{
    if (endpoint < 64) { listenai_board_connect_pad(soc, endpoint, input); return; }
    assert(b->extio);
    unsigned pin = endpoint - 64;
    qemu_irq previous = qdev_get_gpio_out_connector(b->extio, "pin-out", pin);
    if (previous) {
        DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);
        qdev_prop_set_uint16(split, "num-lines", 2);
        qdev_realize_and_unref(split, NULL, &error_fatal);
        qdev_connect_gpio_out(split, 0, previous); qdev_connect_gpio_out(split, 1, input);
        input = qdev_get_gpio_in(split, 0);
    }
    qdev_connect_gpio_out_named(b->extio, "pin-out", pin, input);
}

void listenai_board_gpio_input(ListenaiBoard *b, DeviceState *soc, unsigned endpoint, bool level)
{
    assert(endpoint < 64 || b->extio);
    qemu_set_irq(endpoint < 64 ? qdev_get_gpio_in_named(soc, "pad-in", endpoint) :
                 qdev_get_gpio_in_named(b->extio, "pin-in", endpoint - 64), level);
}

QDict *listenai_board_devices_snapshot(ListenaiBoard *b)
{
    QDict *devices = qdict_new();
    if (b->extio) {
        QDict *device = qdict_new(), *pads = qdict_new();
        for (unsigned i = 0; i < 4; i++) {
            uint32_t driven, levels;
            listenai_extio_outputs(b->extio, i, &driven, &levels);
            QDict *bank = qdict_new(); char name[] = {'A' + i, 0};
            qdict_put_int(bank, "driven", driven); qdict_put_int(bank, "levels", levels);
            qdict_put_obj(pads, name, QOBJECT(bank));
        }
        qdict_put_obj(device, "pads", QOBJECT(pads));
        qdict_put_obj(devices, b->extio_id, QOBJECT(device));
    }
    return devices;
}

void listenai_board_sd_wire(ListenaiBoard *b, DeviceState *soc, DeviceState *sd_host)
{
    if (!b->sd_external_detect) { return; }
    unsigned endpoint = b->sd_detect_endpoint;
    qemu_irq input = endpoint < 64 ? qdev_get_gpio_in_named(soc, "pad-in", endpoint) :
        qdev_get_gpio_in_named(b->extio, "pin-in", endpoint - 64);
    if (!b->sd_detect_active) { input = qemu_irq_invert(input); }
    qdev_connect_gpio_out_named(sd_host, "media-present", 0, input);
    qemu_set_irq(input, sdbus_get_inserted(&SYSBUS_SDHCI(sd_host)->sdbus));
}

void listenai_board_pulls_apply(ListenaiBoard *b, DeviceState *soc)
{
    for (unsigned i = 0; i < 64; i++) {
        if (b->pull_mask & (UINT64_C(1) << i)) {
            listenai_board_gpio_input(b, soc, i, !!(b->pull_levels & (UINT64_C(1) << i)));
        }
    }
    for (unsigned i = 0; i < 32; i++) {
        if (b->extio_pull_mask & (1u << i)) {
            listenai_board_gpio_input(b, soc, 64 + i, !!(b->extio_pull_levels & (1u << i)));
        }
    }
    /* Peripheral reset can precede the receiving GPIO controller's reset. */
    if (b->panel_device) { st7789_redrive(b->panel_device); }
}
