/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AW8737AQNR March 2025 V1.0, pp. 7-8, 18-19.
 * Digital SHDN protocol only. Audio uses normalized Codec PCM; analog gain,
 * NCN, charge pump, thermal protection and speaker acoustics are not modeled.
 */
#include "qemu/osdep.h"
#include "hw/audio/aw8737.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(AW8737State, AW8737)
struct AW8737State {
    SysBusDevice parent_obj;
    QEMUTimer *timer;
    int64_t edge_ns, start_ns;
    unsigned pulses, mode;
    bool high, latched, enabled, invalid;
};

/* Use the documented maximum latch/off delay, and typical startup time. */
#define CONTROL_NS 500000
#define STARTUP_NS 40000000

static void expire(void *opaque)
{
    AW8737State *s = opaque;
    if (!s->high) {
        s->pulses = s->mode = 0;
        s->latched = s->enabled = s->invalid = false;
    } else if (!s->invalid) {
        s->latched = true;
        s->mode = s->pulses;
        int64_t ready = s->start_ns + STARTUP_NS;
        if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= ready) {
            s->enabled = true;
        } else {
            timer_mod(s->timer, ready);
        }
    }
}

static void input(void *opaque, int pin, int level)
{
    AW8737State *s = opaque;
    bool high = !!level;
    if (high == s->high) { return; }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t duration = now - s->edge_ns;
    s->high = high;
    s->edge_ns = now;
    timer_del(s->timer);
    if (!high) {
        if (!s->latched && s->pulses &&
            (duration < 750 || duration > 10000)) {
            s->invalid = true;
        }
        timer_mod(s->timer, now + CONTROL_NS);
    } else if (s->latched) {
        /* Subsequent pulses cannot change the latched mode. */
        timer_mod(s->timer, MAX(now, s->start_ns + STARTUP_NS));
    } else {
        if (!s->pulses) {
            s->start_ns = now;
        } else if (duration < 750 || duration > 10000) {
            s->invalid = true;
        }
        if (++s->pulses > 4) { s->invalid = true; }
        if (s->invalid) {
            error_report("AW8737A unsupported SHDN pulse sequence");
        }
        timer_mod(s->timer, now + CONTROL_NS);
    }
}

bool aw8737_enabled(DeviceState *dev) { return AW8737(dev)->enabled; }
QDict *aw8737_snapshot(DeviceState *dev)
{
    AW8737State *s = AW8737(dev);
    QDict *result = qdict_new();
    qdict_put_bool(result, "enabled", s->enabled);
    qdict_put_bool(result, "shdn", s->high);
    qdict_put_bool(result, "latched", s->latched);
    qdict_put_bool(result, "invalid_sequence", s->invalid);
    qdict_put_int(result, "mode", s->mode);
    return result;
}

static void reset(DeviceState *dev)
{
    AW8737State *s = AW8737(dev);
    timer_del(s->timer);
    s->high = s->latched = s->enabled = s->invalid = false;
    s->pulses = s->mode = 0;
    s->edge_ns = s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}
static void init(Object *obj)
{
    AW8737State *s = AW8737(obj);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, expire, s);
    qdev_init_gpio_in_named(DEVICE(s), input, "shdn", 1);
}
static void finalize(Object *obj) { timer_free(AW8737(obj)->timer); }
static void class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), reset);
}
static const TypeInfo info = {
    .name = TYPE_AW8737, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AW8737State), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
