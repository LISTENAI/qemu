/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Ideal mechanical quadrature contacts. Positive rotation: A leads B.
 * No contact bounce or analog RC waveform is synthesized. */
#include "qemu/osdep.h"
#include "hw/input/quadrature.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qobject/qnum.h"

OBJECT_DECLARE_SIMPLE_TYPE(Quadrature, QUADRATURE)
struct Quadrature {
    SysBusDevice parent_obj;
    qemu_irq output[2];
    QEMUTimer *timer;
    uint32_t edges_per_detent;
    unsigned phase, remaining, delivered;
    int direction;
    int64_t interval_ns, next_ns;
    uint64_t sequence;
    const char *status;
};

void quadrature_drive(DeviceState *dev)
{
    Quadrature *s = QUADRATURE(dev);
    /* Resting contacts open at phase zero, with external pull-ups. */
    static const unsigned levels[] = {3, 1, 0, 2};
    qemu_set_irq(s->output[0], (levels[s->phase] >> 1) & 1);
    qemu_set_irq(s->output[1], levels[s->phase] & 1);
}

static void step(void *opaque)
{
    Quadrature *s = opaque;
    if (!s->remaining) { return; }
    s->phase = (s->phase + s->direction + 4) % 4;
    quadrature_drive(DEVICE(s));
    s->delivered++;
    if (--s->remaining) {
        s->next_ns += s->interval_ns;
        timer_mod(s->timer, s->next_ns);
    } else {
        s->status = "completed";
    }
}

QDict *quadrature_snapshot(DeviceState *dev)
{
    Quadrature *s = QUADRATURE(dev);
    QDict *d = qdict_new();
    qdict_put_int(d, "sequence", s->sequence);
    qdict_put_str(d, "status", s->status);
    qdict_put_int(d, "phase", s->phase);
    qdict_put_int(d, "delivered_edges", s->delivered);
    qdict_put_int(d, "remaining_edges", s->remaining);
    qdict_put_int(d, "sampled_virtual_ns", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return d;
}

void quadrature_command(DeviceState *dev, QDict *command, Error **errp)
{
    Quadrature *s = QUADRATURE(dev);
    const char *action = qdict_get_try_str(command, "action");
    if (action && !strcmp(action, "cancel")) {
        int64_t sequence = qdict_get_try_int(command, "sequence", 0);
        if (sequence <= 0 || sequence != s->sequence) {
            error_setg(errp, "Stale rotary sequence"); return;
        }
        if (s->remaining) {
            timer_del(s->timer); s->remaining = 0; s->status = "cancelled";
        }
        return;
    }
    int64_t detents = qdict_get_try_int(command, "detents", 0);
    int64_t edge_ms = qdict_get_try_int(command, "edge_ms", 0);
    if (!action || strcmp(action, "rotate") || detents == 0 ||
        detents < -32 || detents > 32 || edge_ms < 1 || edge_ms > 100) {
        error_setg(errp, "Rotation requires detents in -32..32 except zero and edge_ms in 1..100");
        return;
    }
    if (s->remaining || s->sequence == INT64_MAX) {
        error_setg(errp, "Rotary encoder is busy or sequence limit reached"); return;
    }
    s->sequence++;
    s->delivered = 0;
    s->remaining = llabs(detents) * s->edges_per_detent;
    s->direction = detents > 0 ? 1 : -1;
    s->interval_ns = edge_ms * 1000000;
    s->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->interval_ns;
    s->status = "running";
    timer_mod(s->timer, s->next_ns);
}

static void reset(DeviceState *dev)
{
    Quadrature *s = QUADRATURE(dev);
    timer_del(s->timer);
    if (s->remaining) { s->status = "reset"; }
    s->remaining = 0;
    /* Warm reset cancels motion, but cannot turn the physical shaft. */
}

static void realize(DeviceState *dev, Error **errp)
{
    Quadrature *s = QUADRATURE(dev);
    if (s->edges_per_detent != 2 && s->edges_per_detent != 4) {
        error_setg(errp, "Quadrature edges-per-detent must be 2 or 4");
    }
}

static void instance_init(Object *obj)
{
    Quadrature *s = QUADRATURE(obj);
    s->status = "idle";
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, step, s);
    qdev_init_gpio_out(DEVICE(s), s->output, 2);
}

static void finalize(Object *obj)
{
    timer_free(QUADRATURE(obj)->timer);
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("edges-per-detent", Quadrature, edges_per_detent, 4),
};

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize;
    device_class_set_props(dc, properties);
    device_class_set_legacy_reset(dc, reset);
}

static const TypeInfo info = {
    .name = TYPE_QUADRATURE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Quadrature),
    .instance_init = instance_init,
    .instance_finalize = finalize,
    .class_init = class_init,
};

static void register_types(void) { type_register_static(&info); }
type_init(register_types)
