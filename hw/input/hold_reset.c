/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional long-press reset circuit; pulse width is not modeled. */
#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_HOLD_RESET "listenai-hold-reset"
OBJECT_DECLARE_SIMPLE_TYPE(HoldReset, HOLD_RESET)

struct HoldReset {
    DeviceState parent_obj;
    QEMUTimer *timer;
    qemu_irq reset;
    uint32_t hold_ms;
    bool pressed, active_low, fired;
};

static void expire(void *opaque)
{
    HoldReset *s = opaque;
    if (!s->pressed || s->fired) { return; }
    s->fired = true;
    /* A functional reset strobe, not an estimate of an analog pulse width. */
    qemu_set_irq(s->reset, 0);
    qemu_set_irq(s->reset, 1);
}

static void input(void *opaque, int pin, int level)
{
    HoldReset *s = opaque;
    bool pressed = !!level != s->active_low;
    if (pressed == s->pressed) { return; }
    s->pressed = pressed;
    timer_del(s->timer);
    if (!pressed) {
        s->fired = false;
    } else {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)s->hold_ms * 1000000);
    }
}

static void realize(DeviceState *dev, Error **errp)
{
    HoldReset *s = HOLD_RESET(dev);
    if (!s->hold_ms || s->hold_ms > 60000) {
        error_setg(errp, "Reset hold time must be 1..60000 ms");
    }
}

static void init(Object *obj)
{
    HoldReset *s = HOLD_RESET(obj);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, expire, s);
    qdev_init_gpio_in(DEVICE(s), input, 1);
    qdev_init_gpio_out(DEVICE(s), &s->reset, 1);
}

static void finalize(Object *obj)
{
    HoldReset *s = HOLD_RESET(obj);
    timer_del(s->timer);
    timer_free(s->timer);
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("hold-ms", HoldReset, hold_ms, 0),
    DEFINE_PROP_BOOL("active-low", HoldReset, active_low, true),
};

static void class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = realize;
    device_class_set_props(dc, properties);
    /* The board circuit stays powered across a chip reset. */
}

static const TypeInfo info = {
    .name = TYPE_HOLD_RESET, .parent = TYPE_DEVICE,
    .instance_size = sizeof(HoldReset), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_type(void) { type_register_static(&info); }
type_init(register_type)
