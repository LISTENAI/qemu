/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CST836U Application Note: normal single-contact reporting and level IRQ.
 * Scan/analog filtering and event_flg encodings are not calibrated. The host
 * supplies an ideal contact; event_flg stays zero, touch_num defines validity.
 * Identity bytes are optional board data, never inferred from the part name. */
#include "qemu/osdep.h"
#include "hw/i2c/cst836u.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(CST836UState, CST836U)
struct CST836UState {
    I2CSlave parent_obj;
    qemu_irq irq;
    QEMUTimer *ready;
    uint32_t width, height, chip_type;
    unsigned x, y, pointer;
    uint8_t snapshot[9];
    bool pressed, reset_low, asleep, address_byte, selected;
    int64_t ready_ns, reset_ns;
};

static bool available(CST836UState *s)
{
    return !s->reset_low && !s->asleep &&
           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->ready_ns;
}

static void update_irq(void *opaque)
{
    CST836UState *s = opaque;
    qemu_set_irq(s->irq, !(available(s) && s->pressed));
}

static void reset(DeviceState *dev)
{
    CST836UState *s = CST836U(dev);
    s->asleep = s->selected = false;
    s->pointer = 0;
    s->address_byte = true;
    memset(s->snapshot, 0, sizeof(s->snapshot));
    s->ready_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000000;
    timer_del(s->ready);
    if (!s->reset_low) { timer_mod(s->ready, s->ready_ns); }
    update_irq(s);
}

static void reset_pin(void *opaque, int pin, int level)
{
    CST836UState *s = opaque;
    if (s->reset_low == !level) { return; }
    s->reset_low = !level;
    if (!level) {
        s->reset_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_del(s->ready);
    } else if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->reset_ns >= 100000) {
        reset(DEVICE(s));
    } else if (s->ready_ns > qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        timer_mod(s->ready, s->ready_ns);
    }
    update_irq(s);
}

static bool readable(CST836UState *s, unsigned reg)
{
    return reg <= 8 || ((reg == 0xaa || reg == 0xab) && s->chip_type <= 65535);
}

static int event(I2CSlave *dev, enum i2c_event ev)
{
    CST836UState *s = CST836U(dev);
    if ((ev == I2C_START_SEND || ev == I2C_START_RECV) && !available(s)) {
        return 1;
    }
    if (ev == I2C_START_SEND) {
        s->address_byte = true;
    } else if (ev == I2C_START_RECV) {
        if (!s->selected || !readable(s, s->pointer)) { return 1; }
        memset(s->snapshot, 0, sizeof(s->snapshot));
        s->snapshot[2] = s->pressed;
        s->snapshot[3] = s->x >> 8;
        s->snapshot[4] = s->x;
        s->snapshot[5] = s->y >> 8;
        s->snapshot[6] = s->y;
    }
    return 0;
}

static int touch_send(I2CSlave *dev, uint8_t value)
{
    CST836UState *s = CST836U(dev);
    if (!available(s)) { return 1; }
    if (s->address_byte) {
        s->pointer = value;
        s->selected = true;
        s->address_byte = false;
        return 0;
    }
    if (s->pointer == 0 && value == 0) {
        s->pointer++;
        return 0;
    }
    if (s->pointer == 0xa5 && value == 3) {
        s->asleep = true;
        s->selected = false;
        timer_del(s->ready);
        update_irq(s);
        return 0;
    }
    /* Factory modes, gestures, proximity and unknown writes are NACKed. */
    return 1;
}

static uint8_t touch_recv(I2CSlave *dev)
{
    CST836UState *s = CST836U(dev);
    unsigned reg = s->pointer++;
    if (!available(s) || !readable(s, reg)) {
        error_report("CST836U unsupported sequential read at register 0x%x", reg);
        exit(1);
    }
    return reg <= 8 ? s->snapshot[reg] : s->chip_type >> ((reg - 0xaa) * 8);
}

static void contact_get(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    CST836UState *s = opaque;
    g_autofree char *value = g_strdup_printf("%u,%u,%s", s->x, s->y,
                                            s->pressed ? "press" : "release");
    visit_type_str(v, name, &value, errp);
}

static void contact_set(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    CST836UState *s = opaque;
    char *value = NULL, extra, state[8];
    unsigned x, y;
    if (!visit_type_str(v, name, &value, errp)) { return; }
    if (!strcmp(value, "release")) {
        s->pressed = false;
    } else if (sscanf(value, "%u,%u,%7s%c", &x, &y, state, &extra) == 3 &&
               !strcmp(state, "press") && x < s->width && y < s->height) {
        s->x = x; s->y = y; s->pressed = true;
    } else {
        error_setg(errp, "Touch input must be x,y,press within sensor bounds or release");
    }
    g_free(value);
    update_irq(s);
}

void cst836u_add_input(Object *owner, DeviceState *dev, const char *name)
{
    object_property_add(owner, name, "str", contact_get, contact_set, NULL, dev);
}

static void realize(DeviceState *dev, Error **errp)
{
    CST836UState *s = CST836U(dev);
    if (!s->width || s->width > 4096 || !s->height || s->height > 4096 ||
        (s->chip_type > 65535 && s->chip_type != UINT32_MAX)) {
        error_setg(errp, "Invalid CST836U geometry or identity");
    }
}

static const Property props[] = {
    DEFINE_PROP_UINT32("width", CST836UState, width, 240),
    DEFINE_PROP_UINT32("height", CST836UState, height, 320),
    DEFINE_PROP_UINT32("chip-type", CST836UState, chip_type, UINT32_MAX),
};

static void init(Object *obj)
{
    CST836UState *s = CST836U(obj);
    s->ready = timer_new_ns(QEMU_CLOCK_VIRTUAL, update_irq, s);
    qdev_init_gpio_out_named(DEVICE(s), &s->irq, "int", 1);
    qdev_init_gpio_in_named(DEVICE(s), reset_pin, "reset", 1);
}

static void finalize(Object *obj)
{
    timer_free(CST836U(obj)->ready);
}

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->event = event; ic->send = touch_send; ic->recv = touch_recv;
    dc->realize = realize;
    device_class_set_props(dc, props);
    device_class_set_legacy_reset(dc, reset);
}

static const TypeInfo info = {
    .name = TYPE_CST836U, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(CST836UState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
