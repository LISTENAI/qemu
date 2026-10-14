/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional touch connector; the development board has no fitted panel. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/reset.h"
#define TYPE_VENUSA_TOUCH "venusa-ft6236-touch"
OBJECT_DECLARE_SIMPLE_TYPE(VenusATouch, VENUSA_TOUCH)
struct VenusATouch {
    DeviceState parent_obj;
    char *soc_path;
    uint32_t width, height;
    DeviceState *sensor;
};
static void contact(void *opaque, int index, int value)
{
    VenusATouch *s = opaque;
    qemu_set_irq(qdev_get_gpio_in_named(s->sensor, "contact", index), value);
}
static void realize(DeviceState *dev, Error **errp)
{
    VenusATouch *s = VENUSA_TOUCH(dev);
    Object *obj = s->soc_path ? object_resolve_path(s->soc_path, NULL) : NULL;
    if (!obj || !object_dynamic_cast(obj, TYPE_VENUSA_SOC)) {
        error_setg(errp, "FT6236 fixture requires a VenusA soc path"); return;
    }
    VenusASoC *soc = VENUSA_SOC(obj);
    s->sensor = qdev_new("ft6236");
    qdev_prop_set_uint8(s->sensor, "address", 0x38);
    qdev_prop_set_uint32(s->sensor, "width", s->width);
    qdev_prop_set_uint32(s->sensor, "height", s->height);
    if (!qdev_realize(s->sensor, BUS(soc->i2c[1].bus), errp)) { return; }
    object_unref(OBJECT(s->sensor));
    qdev_connect_gpio_out_named(s->sensor, "int", 0,
        qdev_get_gpio_in_named(DEVICE(soc), "pad-in", 1));
    qdev_init_gpio_in_named(dev, contact, "contact", 2);
    device_cold_reset(s->sensor);
}
static void unrealize(DeviceState *dev)
{
    VenusATouch *s = VENUSA_TOUCH(dev);
    qdev_unrealize(s->sensor); object_unparent(OBJECT(s->sensor));
}
static const Property props[] = {
    DEFINE_PROP_STRING("soc", VenusATouch, soc_path),
    DEFINE_PROP_UINT32("width", VenusATouch, width, 480),
    DEFINE_PROP_UINT32("height", VenusATouch, height, 800),
};
static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize; dc->unrealize = unrealize;
    device_class_set_props(dc, props);
    dc->desc = "Explicit FT6236 panel connected to VenusA I2C1 and PA1";
}
static const TypeInfo info = {
    .name = TYPE_VENUSA_TOUCH, .parent = TYPE_DEVICE,
    .instance_size = sizeof(VenusATouch), .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
