/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GXA574 eight-bit quasi-bidirectional I2C GPIO expander. */
#include "qemu/osdep.h"
#include "hw/i2c/gxa574.h"
#include "hw/irq.h"
#include "qemu/module.h"

struct GXA574State {
    I2CSlave parent_obj;
    uint8_t latch, external, external_valid;
    qemu_irq outputs[8];
    void (*observe)(void *, unsigned, uint32_t, uint32_t);
    void *observe_opaque;
};

static bool level(GXA574State *s, unsigned pin, bool *driven)
{
    unsigned mask = 1u << pin;
    *driven = !(s->latch & mask);
    if (*driven) { return false; }
    return s->external_valid & mask ? !!(s->external & mask) : true;
}

static void publish(GXA574State *s)
{
    unsigned driven, levels;
    for (unsigned pin = 0; pin < 8; pin++) {
        bool pin_driven;
        qemu_set_irq(s->outputs[pin], level(s, pin, &pin_driven));
    }
    if (s->observe) {
        gxa574_outputs(DEVICE(s), &driven, &levels);
        s->observe(s->observe_opaque, 0, driven, levels);
    }
}

uint32_t gxa574_valid_pins(void) { return 0xff; }

void gxa574_observe(DeviceState *dev,
                    void (*observe)(void *, unsigned, uint32_t, uint32_t),
                    void *opaque)
{
    GXA574State *s = GXA574(dev);
    s->observe = observe;
    s->observe_opaque = opaque;
    if (observe) {
        unsigned driven, levels;
        gxa574_outputs(dev, &driven, &levels);
        observe(opaque, 0, driven, levels);
    }
}

void gxa574_outputs(DeviceState *dev, unsigned *driven, unsigned *levels)
{
    GXA574State *s = GXA574(dev);
    *driven = *levels = 0;
    for (unsigned pin = 0; pin < 8; pin++) {
        bool output, value = level(s, pin, &output);
        if (output) { *driven |= 1u << pin; }
        if (value) { *levels |= 1u << pin; }
    }
}

static int gxa574_send(I2CSlave *dev, uint8_t value)
{
    GXA574State *s = GXA574(dev);
    /* Datasheet 7.2.3: every data byte updates P7..P0, with no command byte. */
    s->latch = value;
    publish(s);
    return 0;
}

static uint8_t gxa574_recv(I2CSlave *dev)
{
    GXA574State *s = GXA574(dev);
    uint8_t value = 0;
    for (unsigned pin = 0; pin < 8; pin++) {
        bool driven;
        if (level(s, pin, &driven)) { value |= 1u << pin; }
    }
    return value;
}

static void input(void *opaque, int pin, int value)
{
    GXA574State *s = opaque;
    uint8_t mask = 1u << pin;
    s->external_valid |= mask;
    if (value) { s->external |= mask; } else { s->external &= ~mask; }
    publish(s);
}

static void reset(DeviceState *dev)
{
    GXA574State *s = GXA574(dev);
    s->latch = 0xff; s->external = 0; s->external_valid = 0;
    publish(s);
}

static void init(Object *obj)
{
    GXA574State *s = GXA574(obj);
    qdev_init_gpio_in_named(DEVICE(s), input, "pin-in", 8);
    qdev_init_gpio_out_named(DEVICE(s), s->outputs, "pin-out", 8);
}

static void class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->send = gxa574_send; ic->recv = gxa574_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), reset);
}

static const TypeInfo info = {
    .name = TYPE_GXA574, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(GXA574State), .instance_init = init, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
