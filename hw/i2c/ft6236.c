/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FT6236 digital host interface; contacts are explicit calibrated input. */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define TYPE_FT6236 "ft6236"
OBJECT_DECLARE_SIMPLE_TYPE(FT6236State, FT6236)
struct FT6236State {
    I2CSlave parent_obj;
    uint8_t regs[256], report[15], address;
    bool pointer, pressed[2], reset_low;
    uint16_t x[2], y[2];
    uint8_t event[2];
    uint32_t width, height;
    uint64_t pulse_ns;
    QEMUTimer *pulse;
    qemu_irq irq;
};
static bool active(FT6236State *s) { return s->pressed[0] || s->pressed[1]; }
static void pulse_end(void *opaque)
{
    FT6236State *s = opaque;
    qemu_set_irq(s->irq, s->regs[0xa4] || !active(s));
}
static void notify(FT6236State *s)
{
    if (!s->regs[0xa4]) {
        timer_del(s->pulse); qemu_set_irq(s->irq, !active(s));
    } else {
        /* Explicit digital pulse profile; not a capacitive scan-time model. */
        qemu_set_irq(s->irq, 0);
        timer_mod(s->pulse, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->pulse_ns);
    }
}
static void contact(void *opaque, int index, int level)
{
    FT6236State *s = opaque;
    uint32_t v = level;
    bool down = v & (1u << 24);
    unsigned x = (v >> 12) & 0xfff, y = v & 0xfff;
    if ((v & ~0x1ffffffu) || (down && (x >= s->width || y >= s->height))) {
        error_report("FT6236 contact is outside the configured panel");
        exit(EXIT_FAILURE);
    }
    if (s->reset_low || (s->pressed[index] == down &&
        (!down || (s->x[index] == x && s->y[index] == y)))) { return; }
    s->event[index] = down ? (s->pressed[index] ? 2 : 0) : 1;
    s->pressed[index] = down;
    if (down) { s->x[index] = x; s->y[index] = y; }
    notify(s);
}
static void snapshot(FT6236State *s)
{
    memset(s->report, 0, sizeof(s->report));
    s->report[3] = s->report[9] = 0xc0;
    s->report[5] = s->report[11] = 0xf0;
    unsigned n = 0;
    for (unsigned i = 0; i < 2; i++) {
        if (!s->pressed[i]) { continue; }
        unsigned p = 3 + n++ * 6;
        s->report[p] = (s->event[i] << 6) | (s->x[i] >> 8);
        s->report[p + 1] = s->x[i];
        s->report[p + 2] = (i << 4) | (s->y[i] >> 8);
        s->report[p + 3] = s->y[i];
        /* Input supplies positions, not measured pressure/contact area. */
        s->event[i] = 2;
    }
    s->report[2] = n;
}
static void reset(DeviceState *dev)
{
    FT6236State *s = FT6236(dev);
    timer_del(s->pulse);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0xa3] = 0x36; /* FT6236 chip family identifier. */
    s->regs[0xa4] = 1;
    s->pointer = true; s->address = 0;
    memset(s->pressed, 0, sizeof(s->pressed));
    snapshot(s); qemu_set_irq(s->irq, 1);
}
static void reset_pin(void *opaque, int unused, int level)
{
    FT6236State *s = opaque;
    if (!level) { reset(DEVICE(s)); }
    s->reset_low = !level;
}
static int bus_event(I2CSlave *dev, enum i2c_event e)
{
    FT6236State *s = FT6236(dev);
    if (s->reset_low) { return 1; }
    if (e == I2C_START_SEND) { s->pointer = true; }
    if (e == I2C_START_RECV) { snapshot(s); }
    return 0;
}
static int sensor_send(I2CSlave *dev, uint8_t v)
{
    FT6236State *s = FT6236(dev);
    if (s->pointer) { s->address = v; s->pointer = false; return 0; }
    unsigned reg = s->address++;
    switch (reg) {
    case 0x00: if (v) { return 1; } break; /* Working mode only. */
    case 0x80: break; /* Threshold, ideal contacts already satisfy it. */
    case 0x86: if (v > 1) { return 1; } break; /* Active/auto-monitor. */
    case 0x88: if (!v) { return 1; } break;
    case 0xa4: if (v > 1) { return 1; } break;
    default: return 1;
    }
    s->regs[reg] = v;
    if (reg == 0xa4) { timer_del(s->pulse); pulse_end(s); }
    return 0;
}
static uint8_t sensor_recv(I2CSlave *dev)
{
    FT6236State *s = FT6236(dev);
    unsigned reg = s->address++;
    if (reg < sizeof(s->report)) { return s->report[reg]; }
    switch (reg) {
    case 0x80: case 0x86: case 0x88: case 0xa3: case 0xa4:
        return s->regs[reg];
    default:
        error_report("FT6236 unsupported register read 0x%02x", reg);
        exit(EXIT_FAILURE);
    }
}
static void realize(DeviceState *dev, Error **errp)
{
    FT6236State *s = FT6236(dev);
    if (!s->width || s->width > 4096 || !s->height || s->height > 4096 ||
        !s->pulse_ns || s->pulse_ns > 100000000) {
        error_setg(errp, "FT6236 requires a bounded panel and interrupt pulse");
    }
}
static void finalize(Object *obj)
{
    FT6236State *s = FT6236(obj);
    timer_free(s->pulse);
}
static void init(Object *obj)
{
    FT6236State *s = FT6236(obj);
    s->pulse = timer_new_ns(QEMU_CLOCK_VIRTUAL, pulse_end, s);
    qdev_init_gpio_out_named(DEVICE(s), &s->irq, "int", 1);
    qdev_init_gpio_in_named(DEVICE(s), contact, "contact", 2);
    qdev_init_gpio_in_named(DEVICE(s), reset_pin, "reset-n", 1);
}
static const Property props[] = {
    DEFINE_PROP_UINT32("width", FT6236State, width, 480),
    DEFINE_PROP_UINT32("height", FT6236State, height, 800),
    DEFINE_PROP_UINT64("irq-pulse-ns", FT6236State, pulse_ns, 100000),
};
static void class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    sc->event = bus_event; sc->send = sensor_send; sc->recv = sensor_recv;
    dc->realize = realize;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, props);
    dc->desc = "FT6236 two-contact digital touch controller";
}
static const TypeInfo info = {
    .name = TYPE_FT6236, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(FT6236State), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
