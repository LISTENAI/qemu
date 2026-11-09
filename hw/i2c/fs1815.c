/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Analog amplifier v1 profile. FS1815CN V1.3 supplies the documented control
 * registers. Opaque analog calibration registers accept only the values in
 * this profile; they do not implement undocumented operating modes.
 * Ideal linear voltage gain with full-scale clipping; no speaker, boost,
 * thermal/protection dynamics or acoustic response are modeled. Telemetry
 * registers without a modeled physical source NACK rather than report success.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/i2c/fs1815.h"
#include "qemu/module.h"
#include "qapi/error.h"

OBJECT_DECLARE_SIMPLE_TYPE(FS1815State, FS1815)
struct FS1815State {
    I2CSlave parent_obj;
    uint16_t regs[256], read_word;
    unsigned address, sent, received, high;
    double gain;
};

bool fs1815_enabled(DeviceState *dev)
{
    FS1815State *s = FS1815(dev);
    return !(s->regs[0x10] & 1) && (s->regs[0x11] & 0x83) == 0x83;
}

int16_t fs1815_amplify(DeviceState *dev, int16_t input)
{
    FS1815State *s = FS1815(dev);
    if (!fs1815_enabled(dev)) { return 0; }
    return MAX(INT16_MIN, MIN(INT16_MAX, lrint(input * s->gain)));
}

static void reset(DeviceState *dev)
{
    FS1815State *s = FS1815(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[1] = 0x12; s->regs[0x0f] = 0x34;
    s->regs[0x10] = 1; s->regs[0x18] = 0x10;
    s->regs[0x19] = 7; s->regs[0x20] = 0x1830;
    s->regs[0x58] = 0x027a; s->regs[0x59] = 0x0821;
    s->regs[0x5a] = 0x0841; s->regs[0xbc] = 0x9403;
    s->gain = pow(10., 24. / 20.);
    s->address = s->sent = s->received = s->high = s->read_word = 0;
}

static bool readable(unsigned reg)
{
    switch (reg) {
    case 0x00: case 0x01: case 0x0f: case 0x10: case 0x11: case 0x18:
    case 0x19: case 0x1f: case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x2a: case 0x3e: case 0x3f: case 0x4c: case 0x58: case 0x59:
    case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x88: case 0x89:
    case 0xbc: case 0xcc: return true;
    default: return false;
    }
}

static bool write_word(FS1815State *s, unsigned reg, uint16_t value)
{
    switch (reg) {
    case 0x00: case 0x01: case 0x0f: return true; /* Read-only. */
    case 0x10:
        if (value & ~3) { return false; }
        if (value & 2) { reset(DEVICE(s)); return true; }
        break;
    case 0x11: if (value & ~0x8f) { return false; } break;
    case 0x18:
        if (value & ~31) { return false; }
        s->gain = pow(10., (value * 1.5) / 20.);
        break;
    /* Documented analog settings: supported profile with ideal output. */
    case 0x19: if (value != 7 && value != 19) { return false; } break;
    case 0x20: if (value != 0x1830 && value != 0x1282) { return false; } break;
    case 0x58: if (value != 0x827a && value != 0x821a) { return false; } break;
    case 0x59: if (value != 0xd821 && value != 0x0c2c) { return false; } break;
    case 0x5a: if (value != 0xd841 && value != 0xfcb9) { return false; } break;
    case 0xbc: if (value != 0x9423 && value != 0x1423) { return false; } break;
    case 0x1f: if (value != 0 && value != 0xca91) { return false; } break;
    case 0x21: if (value != 0xb560) { return false; } break;
    case 0x22: if (value != 0x0108) { return false; } break;
    case 0x23: if (value != 0x9407) { return false; } break;
    case 0x2a: if (value != 0x0030) { return false; } break;
    case 0x3e: if (value != 0x8500) { return false; } break;
    case 0x3f: if (value != 0x8110 && value != 0x0110) { return false; } break;
    case 0x4c: if (value != 0x7008 && value != 0x7000) { return false; } break;
    case 0x5b: if (value != 0xd07b && value != 0xdc7b) { return false; } break;
    case 0x5c: if (value != 0xd800) { return false; } break;
    case 0x5d: if (value != 0x2001) { return false; } break;
    case 0x88: if (value != 0xa000) { return false; } break;
    case 0x89: if (value != 0x0032) { return false; } break;
    case 0xcc: if (value != 0 || s->regs[0x1f] != 0xca91) { return false; } break;
    default: return false;
    }
    s->regs[reg] = value;
    return true;
}

static int event(I2CSlave *dev, enum i2c_event event)
{
    FS1815State *s = FS1815(dev);
    if (event == I2C_START_SEND) { s->sent = 0; }
    if (event == I2C_START_RECV) {
        if (!readable(s->address)) { return -1; }
        s->received = 0; s->read_word = s->regs[s->address];
    }
    return 0;
}

static int fs_send(I2CSlave *dev, uint8_t value)
{
    FS1815State *s = FS1815(dev);
    if (!s->sent) {
        s->address = value; s->sent++;
        return readable(value) ? 0 : -1;
    }
    if (s->sent++ == 1) { s->high = value; return 0; }
    if (s->sent != 3) { return -1; }
    return write_word(s, s->address, s->high << 8 | value) ? 0 : -1;
}

static uint8_t fs_recv(I2CSlave *dev)
{
    FS1815State *s = FS1815(dev);
    return s->received++ == 0 ? s->read_word >> 8 : s->received == 2 ? s->read_word : 0xff;
}

static bool enabled_get(Object *obj, Error **errp) { return fs1815_enabled(DEVICE(obj)); }
static void init(Object *obj) { object_property_add_bool(obj, "enabled", enabled_get, NULL); }
static void class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *i2c = I2C_SLAVE_CLASS(klass);
    i2c->event = event; i2c->send = fs_send; i2c->recv = fs_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), reset);
}
static const TypeInfo info = {
    .name = TYPE_FS1815, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(FS1815State), .instance_init = init,
    .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
