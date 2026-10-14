/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit one-way board wire between digital pads, with no protocol logic. */
#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "hw/gpio/listenai_gpio_wire.h"
#include "qapi/error.h"
#include "qom/object.h"
#define TYPE_LISTENAI_GPIO_WIRE "listenai-gpio-wire"
OBJECT_DECLARE_SIMPLE_TYPE(ListenaiGPIOWire, LISTENAI_GPIO_WIRE)
struct ListenaiGPIOWire {
    DeviceState parent_obj;
    DeviceState *gpio;
    uint32_t source, destination;
    qemu_irq target;
    bool valid, level, registered;
    QLIST_ENTRY(ListenaiGPIOWire) node;
};
static QLIST_HEAD(, ListenaiGPIOWire) wires = QLIST_HEAD_INITIALIZER(wires);
bool listenai_gpio_wire_connected(DeviceState *gpio, unsigned source)
{
    ListenaiGPIOWire *s;
    QLIST_FOREACH(s, &wires, node) {
        if (s->gpio == gpio && s->source == source) { return true; }
    }
    return false;
}
static void input(void *opaque, int n, int level)
{
    ListenaiGPIOWire *s = opaque;
    if (s->valid && s->level == !!level) { return; }
    s->valid = true; s->level = !!level;
    qemu_set_irq(s->target, level);
}
static void realize(DeviceState *dev, Error **errp)
{
    ListenaiGPIOWire *s = LISTENAI_GPIO_WIRE(dev);
    unsigned ins = 0, outs = 0;
    NamedGPIOList *list;
    if (!s->gpio) { error_setg(errp, "Digital wire requires a GPIO device"); return; }
    QLIST_FOREACH(list, &s->gpio->gpios, node) {
        if (list->name && !strcmp(list->name, "pad-in")) { ins = list->num_in; }
        if (list->name && !strcmp(list->name, "pad-out")) { outs = list->num_out; }
    }
    if (s->source >= outs || s->destination >= ins || s->source == s->destination ||
        qdev_get_gpio_out_connector(s->gpio, "pad-out", s->source)) {
        error_setg(errp, "Digital wire requires valid distinct pads and an unused output");
        return;
    }
    s->target = qdev_get_gpio_in_named(s->gpio, "pad-in", s->destination);
    qdev_connect_gpio_out_named(s->gpio, "pad-out", s->source, qdev_get_gpio_in(dev, 0));
    QLIST_INSERT_HEAD(&wires, s, node);
    s->registered = true;
}
static void unrealize(DeviceState *dev)
{
    ListenaiGPIOWire *s = LISTENAI_GPIO_WIRE(dev);
    if (s->registered) { QLIST_REMOVE(s, node); s->registered = false; }
}
static const Property properties[] = {
    DEFINE_PROP_LINK("gpio", ListenaiGPIOWire, gpio, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_UINT32("source", ListenaiGPIOWire, source, 0),
    DEFINE_PROP_UINT32("destination", ListenaiGPIOWire, destination, 1),
};
static void instance_init(Object *obj)
{
    qdev_init_gpio_in(DEVICE(obj), input, 1);
}
static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize;
    dc->unrealize = unrealize;
    dc->hotpluggable = false;
    device_class_set_props(dc, properties);
}
static const TypeInfo wire_info = {
    .name = TYPE_LISTENAI_GPIO_WIRE, .parent = TYPE_DEVICE,
    .instance_size = sizeof(ListenaiGPIOWire), .instance_init = instance_init,
    .class_init = class_init,
};
static void register_types(void) { type_register_static(&wire_info); }
type_init(register_types)
