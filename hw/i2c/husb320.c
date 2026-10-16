/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HUSB320 I2C mode. A connected USB host supplies default Rp on CC1 and 5 V.
 * CC/VBUS events use virtual deadlines; reads never advance the state machine.
 * GPIO mode, accessories, manual force and analog supply collapse are absent. */
#include "qemu/osdep.h"
#include "hw/i2c/husb320.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "qemu/module.h"

struct HUSB320 {
    I2CSlave parent_obj;
    uint8_t reg[0x17], address;
    bool pointer, host, attached;
    int64_t ready_at, vbus_at, attach_at;
    QEMUTimer *timer;
    qemu_irq irq, id;
};

static void publish(HUSB320 *s)
{
    bool pending = (s->reg[0x14] & ~s->reg[0x0e]) |
                   (s->reg[0x15] & ~s->reg[0x0f]);
    qemu_set_irq(s->irq, !pending || (s->reg[4] & 1));
    /* The implemented peer is a host/source: ID is always released. */
    qemu_set_irq(s->id, 1);
}

static bool candidate(HUSB320 *s)
{
    return s->host && (s->reg[5] & 8) && !(s->reg[9] & 2) &&
           (s->reg[3] & 6);
}

static void schedule(HUSB320 *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), next = INT64_MAX;
    if (s->ready_at > now) { next = s->ready_at; }
    if (s->vbus_at >= 0) { next = MIN(next, s->vbus_at); }
    if (s->attach_at >= 0) { next = MIN(next, s->attach_at); }
    if (next != INT64_MAX) { timer_mod(s->timer, next); }
    else { timer_del(s->timer); }
    publish(s);
}

static void detach(HUSB320 *s)
{
    s->attach_at = -1;
    if (s->attached) {
        s->attached = false;
        s->reg[0x11] &= ~0x37;
        s->reg[0x13] = 0;
        s->reg[0x14] |= 2 | 4;
    }
}

static void reevaluate(HUSB320 *s)
{
    if (!candidate(s)) { detach(s); }
    else if (!s->attached && s->attach_at < 0) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->attach_at = MAX(now, s->ready_at) + (120 + 10 * (s->reg[5] & 7)) * 1000000LL;
    }
    schedule(s);
}

static void tick(void *opaque)
{
    HUSB320 *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < s->ready_at) { schedule(s); return; }
    if (s->vbus_at >= 0 && now >= s->vbus_at) {
        s->vbus_at = -1;
        bool previous = s->reg[0x11] & 8;
        s->reg[0x11] = (s->reg[0x11] & ~0x48) | (s->host ? 8 : 0x40);
        if (previous != s->host) { s->reg[0x14] |= 16; }
    }
    if (s->attach_at >= 0 && now >= s->attach_at) {
        s->attach_at = -1;
        if (candidate(s)) {
            s->attached = true;
            s->reg[0x11] |= 0x13; /* CC1, default Rp, Attached.SNK. */
            s->reg[0x13] = 0x10;
            s->reg[0x14] |= 0x45;
        }
    }
    schedule(s);
}

static void reset(DeviceState *dev)
{
    HUSB320 *s = HUSB320_BA(dev);
    memset(s->reg, 0, sizeof(s->reg));
    s->reg[1] = 0x10; s->reg[2] = 3;
    s->reg[3] = 0x4c; s->reg[4] = 0x41; s->reg[5] = 0x23;
    s->reg[0x16] = 0x90;
    s->attached = false; s->address = 0; s->pointer = false;
    s->ready_at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000000;
    s->vbus_at = s->ready_at + 375000; s->attach_at = -1;
    schedule(s);
}

static void host_input(void *opaque, int n, int level)
{
    HUSB320 *s = opaque;
    if (s->host == !!level) { return; }
    s->host = !!level;
    s->vbus_at = MAX(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->ready_at) + 375000;
    reevaluate(s);
}

static int event(I2CSlave *dev, enum i2c_event event)
{
    HUSB320 *s = HUSB320_BA(dev);
    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->ready_at) { return 1; }
        if (event == I2C_START_SEND) { s->pointer = true; }
    }
    return 0;
}

static bool readable(unsigned reg)
{
    return (reg >= 1 && reg <= 5) || reg == 9 || reg == 10 ||
           (reg >= 14 && reg <= 22 && reg != 16 && reg != 18);
}

static uint8_t receive(I2CSlave *dev)
{
    HUSB320 *s = HUSB320_BA(dev);
    return readable(s->address) ? s->reg[s->address++] : 0xff;
}

static int transmit(I2CSlave *dev, uint8_t value)
{
    HUSB320 *s = HUSB320_BA(dev);
    if (s->pointer) {
        s->pointer = false; s->address = value;
        return !readable(value);
    }
    unsigned reg = s->address;
    if (reg == 3) {
        unsigned role = value & 7;
        if ((value & 0x30) == 0x20) { return 1; } /* Try.SRC timing is absent. */
        value = (value & 0x78) | (role & 4 ? 4 : role & 2 ? 2 : role & 1);
        if (s->reg[reg] != value) { detach(s); }
        s->reg[reg] = value;
    } else if (reg == 4) { s->reg[reg] = value & 0xf7; }
    else if (reg == 5) {
        if ((value & 7) == 7 || (value & 0x90)) { return 1; }
        s->reg[reg] = value & 0x7f;
    } else if (reg == 9) {
        if (value & ~2) { return 1; }
        s->reg[reg] = value;
    } else if (reg == 10) {
        if (value & ~1) { return 1; }
        if (value & 1) { reset(DEVICE(dev)); return 0; }
    } else if (reg == 14 || reg == 15) {
        s->reg[reg] = value & (reg == 14 ? 0x7f : 6);
    } else if (reg == 20 || reg == 21) {
        s->reg[reg] &= ~value;
    } else { return 1; }
    s->address++;
    reevaluate(s);
    return 0;
}

static void init(Object *obj)
{
    HUSB320 *s = HUSB320_BA(obj);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    qdev_init_gpio_in_named(DEVICE(obj), host_input, "host-connected", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "int-n", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->id, "id", 1);
}

static void finalize(Object *obj)
{
    HUSB320 *s = HUSB320_BA(obj);
    timer_free(s->timer);
}

static void class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *i2c = I2C_SLAVE_CLASS(oc);
    i2c->event = event; i2c->recv = receive; i2c->send = transmit;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), reset);
}

static const TypeInfo type = {
    .name = TYPE_HUSB320_BA, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(HUSB320), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&type); }
type_init(register_types)
