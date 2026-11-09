/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LS-8500 datasheet v1.6, register descriptions pp. 11-17.
 * Ideal 5600 K white-LED scene, using the typical 550 lux / 5000 count
 * sensitivity at 4x analog, 2x digital gain and 100 ms integration.
 * No incident IR, dark noise or package spectral/angular variation is modeled.
 * Synchronous external triggering is unsupported and NACKed.
 */
#include "qemu/osdep.h"
#include "hw/i2c/ls8500.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"

OBJECT_DECLARE_SIMPLE_TYPE(LS8500State, LS8500)
struct LS8500State {
    I2CSlave parent_obj;
    uint8_t regs[33], address, sent;
    uint8_t read_data[4], below, above;
    uint32_t lux;
    uint16_t conversion;
    uint64_t conversions, data_reads;
    uint16_t last_read_als;
    bool waiting, reading, pending;
    uint16_t pending_value;
    QEMUTimer *timer;
    qemu_irq irq;
};
static const uint32_t integration_us[16] = {
    200,400,800,1600,3100,6300,12500,25000,50000,100000,
    200000,400000,800000,1600000,1600000,1600000,
};
static const uint32_t wait_us[16] = {
    0,800,1600,3200,6300,12500,25000,50000,100000,200000,
    400000,800000,1600000,3200000,6400000,12800000,
};
static void update_irq(LS8500State *s)
{
    qemu_set_irq(s->irq, !(s->regs[2] & s->regs[3] & 3));
}
static void begin(LS8500State *s)
{
    unsigned a = s->regs[15] & 7, d = (s->regs[15] >> 3) & 7;
    const unsigned analog[8] = {1,4,16,128,256,256,256,256};
    uint32_t us = integration_us[s->regs[16] & 15];
    uint64_t counts = (uint64_t)s->lux * 5000 * analog[a] * (1u << MIN(d, 4)) * us;
    counts /= UINT64_C(550) * 8 * 100000;
    s->conversion = MIN(counts, 65535);
    s->waiting = false;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)us * 1000);
}
static void publish(LS8500State *s)
{
    s->regs[19] = s->regs[20] = 0; /* No incident infrared. */
    s->regs[21] = s->pending_value;
    s->regs[22] = s->pending_value >> 8;
    s->regs[1] |= 1;
    s->pending = false;
}

static void sample(void *opaque)
{
    LS8500State *s = opaque;
    if (!(s->regs[0] & 1)) { return; }
    if (s->waiting) { begin(s); return; }
    s->pending_value = s->conversion;
    s->pending = true;
    if (!s->reading) { publish(s); }
    s->conversions++;
    unsigned value = (s->regs[3] & 4) ? 0 : s->conversion;
    unsigned low = s->regs[7] | s->regs[8] << 8;
    unsigned high = s->regs[9] | s->regs[10] << 8;
    unsigned persistence = s->regs[5] & 15;
    s->below = value < low ? MIN(s->below + 1, 15) : 0;
    s->above = value > high ? MIN(s->above + 1, 15) : 0;
    if (!persistence || s->below >= persistence) { s->regs[2] |= s->regs[3] & 1; }
    if (!persistence || s->above >= persistence) { s->regs[2] |= s->regs[3] & 2; }
    update_irq(s);
    unsigned wait = wait_us[s->regs[16] >> 4];
    if (wait) {
        s->waiting = true;
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)wait * 1000);
    } else { begin(s); }
}
static bool known(unsigned a)
{
    return a <= 3 || a == 5 || (a >= 7 && a <= 10) || a == 15 ||
           a == 16 || (a >= 19 && a <= 22) || a == 32;
}
static void reset(DeviceState *dev)
{
    LS8500State *s = LS8500(dev);
    timer_del(s->timer); memset(s->regs, 0, sizeof(s->regs));
    /* Register-set reset table: LCONF1=C0. Its detailed table lists 0B;
     * the model follows the register-set table, with reserved high bits kept. */
    s->regs[9] = s->regs[10] = 255; s->regs[15] = 0xc0;
    s->regs[16] = 9; s->regs[32] = 9;
    s->address = s->sent = s->below = s->above = 0;
    s->data_reads = 0; s->last_read_als = 0;
    s->conversions = 0; s->waiting = s->reading = s->pending = false;
    memset(s->read_data, 0, sizeof(s->read_data)); update_irq(s);
}
static int event(I2CSlave *dev, enum i2c_event ev)
{
    LS8500State *s = LS8500(dev);
    if (ev == I2C_FINISH || ev == I2C_START_SEND) {
        s->reading = false;
        if (s->pending) { publish(s); }
    }
    if (ev == I2C_START_SEND) { s->sent = 0; }
    if (ev == I2C_START_RECV) {
        s->reading = true;
        memcpy(s->read_data, s->regs + 19, 4);
    }
    return 0;
}
static int send_byte(I2CSlave *dev, uint8_t value)
{
    LS8500State *s = LS8500(dev);
    if (!s->sent) {
        if (!known(value)) { return 1; }
        s->address = value; s->sent = 1; return 0;
    }
    unsigned a = s->address;
    if (!known(a)) { return 1; }
    if (a == 0 && (value & 0x40)) { return 1; }
    if (a == 16 && (value & 15) == 14) { return 1; } /* Undefined LIT encoding. */
    if (a == 0 && (value & 0x80)) { reset(DEVICE(s)); return 0; }
    unsigned mask = a == 0 ? 1 : a == 3 ? 7 : a == 5 ? 15 :
                    a == 15 ? 63 : a == 16 || (a >= 7 && a <= 10) ? 255 : 0;
    bool enabled = s->regs[0] & 1;
    s->regs[a] = (s->regs[a] & ~mask) | (value & mask); s->address++;
    if (a == 0 && enabled != !!(value & 1)) {
        timer_del(s->timer);
        s->below = s->above = 0;
        if (value & 1) { begin(s); }
    }
    update_irq(s); return 0;
}
static uint8_t recv_byte(I2CSlave *dev)
{
    LS8500State *s = LS8500(dev);
    unsigned a = s->address++;
    if (!known(a)) { return 0xff; }
    uint8_t value = (a >= 19 && a <= 22) ? s->read_data[a - 19] : s->regs[a];
    if (a == 2) { s->regs[2] = 0; update_irq(s); }
    if (a == 22) {
        s->regs[1] &= ~1;
        s->data_reads++;
        s->last_read_als = s->read_data[2] | s->read_data[3] << 8;
    }
    return value;
}
void ls8500_set_illuminance(DeviceState *dev, uint32_t lux)
{
    LS8500(dev)->lux = lux;
}
QDict *ls8500_snapshot(DeviceState *dev)
{
    LS8500State *s = LS8500(dev);
    QDict *result = qdict_new();
    qdict_put_int(result, "illuminance_lux", s->lux);
    qdict_put_int(result, "conversions", s->conversions);
    qdict_put_int(result, "data_reads", s->data_reads);
    qdict_put_int(result, "last_read_als", s->last_read_als);
    qdict_put_int(result, "next_event_ns", timer_pending(s->timer) ? timer_expire_time_ns(s->timer) : 0);
    qdict_put_int(result, "als", s->regs[21] | s->regs[22] << 8);
    qdict_put_int(result, "ir", s->regs[19] | s->regs[20] << 8);
    qdict_put_bool(result, "enabled", s->regs[0] & 1);
    qdict_put_bool(result, "ready", s->regs[1] & 1);
    qdict_put_bool(result, "interrupt", s->regs[2] & s->regs[3] & 3);
    return result;
}

static void set_scene(Object *obj, const char *value, Error **errp)
{
    uint64_t lux;
    if (qemu_strtou64(value, NULL, 10, &lux) || lux > 60000) {
        error_setg(errp, "Illuminance must be 0..60000 lux"); return;
    }
    ls8500_set_illuminance(DEVICE(obj), lux);
}
static char *get_scene(Object *obj, Error **errp)
{
    return g_strdup_printf("%u", LS8500(obj)->lux);
}
static void init(Object *obj)
{
    LS8500State *s = LS8500(obj);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sample, s);
    qdev_init_gpio_out_named(DEVICE(s), &s->irq, "int", 1);
    object_property_add_str(obj, "illuminance-lux", get_scene, set_scene);
}
static void finalize(Object *obj) { timer_free(LS8500(obj)->timer); }
static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->event = event; ic->send = send_byte; ic->recv = recv_byte;
    device_class_set_legacy_reset(dc, reset);
}
static const TypeInfo info = {
    .name = TYPE_LS8500, .parent = TYPE_I2C_SLAVE, .instance_size = sizeof(LS8500State),
    .instance_init = init, .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
