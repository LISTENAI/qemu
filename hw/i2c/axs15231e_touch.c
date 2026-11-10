/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AXS15231E vE4 host reporting protocol. Coordinate/ESD packets and SFR
 * access follow the host driver. This models the running touch firmware,
 * not its instruction set or external Flash programmer. Scan and heartbeat
 * cadence are estimates; reset recovery follows the V0.5 datasheet 5.1.2. */
#include "qemu/osdep.h"
#include "hw/i2c/axs15231e_touch.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(AXSTouch, AXS15231E_TOUCH)
struct AXSTouch {
    I2CSlave parent_obj;
    QEMUTimer *scan, *irq_end;
    qemu_irq irq;
    uint32_t width, height;
    uint8_t command[13], frame[15], reading[15];
    unsigned sent, received, read_size, request;
    unsigned x, y;
    bool pressed, reported_pressed, reset_low, pending, esd;
    int64_t ready_ns, reset_ns, heartbeat_ns;
    uint64_t frames, reads, heartbeats, skipped, rejected;
};

#define SCAN_NS 8000000
#define HEARTBEAT_NS 500000000
#define IRQ_NS 1000000
#define BOOT_NS 200000000

enum { READ_NONE, READ_POINT, READ_VERSION };

static const uint8_t point_command[13] = {
    0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 15, 0, 0, 0, 0, 0
};
static const uint8_t version_command[11] = {
    0x5a, 0xa5, 0xab, 0xb5, 0, 0, 0, 1, 0, 0x80, 0x89
};
static const uint8_t esd_command[13] = {
    0xb5, 0xab, 0x5a, 0xa5, 0, 2, 0, 0, 0, 0, 0, 0xf1, 0
};

static void schedule(AXSTouch *s)
{
    if (!s->reset_low) {
        timer_mod(s->scan, MAX(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCAN_NS,
                               s->ready_ns));
    }
}

static void irq_end(void *opaque)
{
    AXSTouch *s = opaque;
    qemu_set_irq(s->irq, 1);
}

static void scan(void *opaque)
{
    AXSTouch *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->reset_low) {
        return;
    }
    bool heartbeat = s->esd && now >= s->heartbeat_ns;
    if (s->pending) {
        s->skipped++;
    } else {
        memset(s->frame, 0, sizeof(s->frame));
        /* The host debounces release across successive no-contact scans. */
        if (!heartbeat || s->pressed || s->reported_pressed) {
            s->frame[1] = s->pressed ? 1 : 0;
            s->frame[2] =
                (s->pressed ? (s->reported_pressed ? 0x80 : 0) : 0x40) |
                (s->x >> 8);
            s->frame[3] = s->x;
            s->frame[4] = s->y >> 8;
            s->frame[5] = s->y;
            s->reported_pressed = s->pressed;
        } else {
            /* An idle scan has no contacts; the vE4 driver distinguishes
             * this report from fault codes by byte 0 and the FF payload. */
            memset(s->frame + 1, 0xff, 13);
            s->heartbeats++;
            s->heartbeat_ns = now + HEARTBEAT_NS;
        }
        for (unsigned i = 0; i < 14; i++) {
            s->frame[14] += s->frame[i];
        }
        s->frames++;
        s->pending = true;
    }
    /* Periodic scan notification must recover a missed falling edge. The
     * unread report stays latched; pulse width is an estimated digital timing.
     */
    qemu_set_irq(s->irq, 0);
    timer_mod(s->irq_end, now + IRQ_NS);
    schedule(s);
}

static void reset(DeviceState *dev)
{
    AXSTouch *s = AXS15231E_TOUCH(dev);
    timer_del(s->scan);
    timer_del(s->irq_end);
    s->sent = s->received = s->read_size = s->request = 0;
    s->pending = s->esd = s->reported_pressed = false;
    memset(s->frame, 0, sizeof(s->frame));
    s->ready_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BOOT_NS;
    s->heartbeat_ns = s->ready_ns + HEARTBEAT_NS;
    qemu_set_irq(s->irq, 1);
    schedule(s);
}

static void reset_pin(void *opaque, int pin, int level)
{
    AXSTouch *s = opaque;
    if (s->reset_low == !level) {
        return;
    }
    s->reset_low = !level;
    if (!level) {
        s->reset_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_del(s->scan);
        timer_del(s->irq_end);
        qemu_set_irq(s->irq, 1);
    } else if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->reset_ns >= 5000000) {
        reset(DEVICE(s));
    } else {
        qemu_set_irq(s->irq, !s->pending);
        schedule(s);
    }
}

static int event(I2CSlave *dev, enum i2c_event event)
{
    AXSTouch *s = AXS15231E_TOUCH(dev);
    if ((event == I2C_START_SEND || event == I2C_START_RECV) &&
        (s->reset_low || qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->ready_ns)) {
        return 1;
    }
    if (event == I2C_START_SEND) {
        s->sent = s->request = 0;
    } else if (event == I2C_START_RECV) {
        if (!s->request) {
            s->rejected++;
            return 1;
        }
        s->received = 0;
        s->read_size = s->request == READ_POINT ? 15 : 1;
        if (s->request == READ_POINT) {
            memcpy(s->reading, s->frame, 15);
        } else {
            s->reading[0] = 0xe4;
        }
    }
    return 0;
}

static int touch_send(I2CSlave *dev, uint8_t value)
{
    AXSTouch *s = AXS15231E_TOUCH(dev);
    if (s->sent >= sizeof(s->command)) {
        s->request = 0;
        s->rejected++;
        return 1;
    }
    s->command[s->sent++] = value;
    bool point = !memcmp(s->command, point_command, s->sent);
    bool version = s->sent <= sizeof(version_command) &&
                   !memcmp(s->command, version_command, s->sent);
    bool esd = !memcmp(s->command, esd_command, s->sent);
    if (!point && !version && !esd) {
        s->request = 0;
        s->rejected++;
        return 1;
    }
    if (version && s->sent == sizeof(version_command)) {
        s->request = READ_VERSION;
    }
    if (point && s->sent == sizeof(point_command)) {
        s->request = READ_POINT;
    }
    if (esd && s->sent == sizeof(esd_command)) {
        if (!s->esd) {
            s->heartbeat_ns =
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + HEARTBEAT_NS;
        }
        s->esd = true;
    }
    return 0;
}

static uint8_t touch_recv(I2CSlave *dev)
{
    AXSTouch *s = AXS15231E_TOUCH(dev);
    if (s->received >= s->read_size) {
        s->rejected++;
        return 0xff;
    }
    uint8_t value = s->reading[s->received++];
    if (s->received == s->read_size) {
        if (s->request == READ_POINT) {
            s->pending = false;
            s->reads++;
            qemu_set_irq(s->irq, 1);
        }
        s->request = READ_NONE;
    }
    return value;
}

static void contact_get(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    AXSTouch *s = opaque;
    g_autofree char *value = g_strdup_printf("%u,%u,%s", s->x, s->y,
                                             s->pressed ? "press" : "release");
    visit_type_str(v, name, &value, errp);
}

static void contact_set(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    AXSTouch *s = opaque;
    char *value = NULL, extra, state[8];
    unsigned x, y;
    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    if (!strcmp(value, "release")) {
        s->pressed = false;
    } else if (sscanf(value, "%u,%u,%7s%c", &x, &y, state, &extra) == 3 &&
               !strcmp(state, "press") && x < s->width && y < s->height) {
        s->x = x;
        s->y = y;
        s->pressed = true;
    } else {
        error_setg(
            errp,
            "Touch input must be x,y,press within sensor bounds or release");
    }
    g_free(value);
}

void axs15231e_touch_add_input(Object *owner, DeviceState *dev,
                               const char *name)
{
    object_property_add(owner, name, "str", contact_get, contact_set, NULL,
                        dev);
}

static char *diagnostic(Object *obj, Error **errp)
{
    AXSTouch *s = AXS15231E_TOUCH(obj);
    return g_strdup_printf(
        "{\"frames\":%" PRIu64 ",\"reads\":%" PRIu64 ",\"heartbeats\":%" PRIu64
        ",\"skipped\":%" PRIu64 ",\"rejected\":%" PRIu64
        ",\"pending\":%s,\"esd\":%s}",
        s->frames, s->reads, s->heartbeats, s->skipped, s->rejected,
        s->pending ? "true" : "false", s->esd ? "true" : "false");
}

static void realize(DeviceState *dev, Error **errp)
{
    AXSTouch *s = AXS15231E_TOUCH(dev);
    if (!s->width || s->width > 4096 || !s->height || s->height > 4096) {
        error_setg(errp, "AXS15231E touch bounds must fit 12-bit coordinates");
    }
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("width", AXSTouch, width, 180),
    DEFINE_PROP_UINT32("height", AXSTouch, height, 720),
};

static void instance_init(Object *obj)
{
    AXSTouch *s = AXS15231E_TOUCH(obj);
    s->scan = timer_new_ns(QEMU_CLOCK_VIRTUAL, scan, s);
    s->irq_end = timer_new_ns(QEMU_CLOCK_VIRTUAL, irq_end, s);
    qdev_init_gpio_out_named(DEVICE(s), &s->irq, "int", 1);
    qdev_init_gpio_in_named(DEVICE(s), reset_pin, "reset", 1);
    object_property_add_str(obj, "scan-diagnostic", diagnostic, NULL);
}

static void finalize(Object *obj)
{
    AXSTouch *s = AXS15231E_TOUCH(obj);
    timer_free(s->scan);
    timer_free(s->irq_end);
}

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->send = touch_send;
    ic->recv = touch_recv;
    ic->event = event;
    dc->realize = realize;
    device_class_set_props(dc, properties);
    device_class_set_legacy_reset(dc, reset);
}

static const TypeInfo info = {
    .name = TYPE_AXS15231E_TOUCH,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AXSTouch),
    .instance_init = instance_init,
    .instance_finalize = finalize,
    .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&info);
}
type_init(register_types)
