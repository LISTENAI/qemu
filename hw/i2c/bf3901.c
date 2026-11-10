/* SPDX-License-Identifier: GPL-2.0-or-later */
/* BF3901 SCCB registers and digital sensor state. */
#include "qemu/osdep.h"
#include "hw/i2c/bf3901.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

struct BF3901State {
    I2CSlave parent_obj;
    uint8_t regs[256], address;
    bool pointer, power_down, reset_released;
    uint64_t clock;
    QEMUTimer *event;
    qemu_irq pins[3]; /* VSYNC, VCLK, DATA, in physical wire order. */
    uint8_t rgb[640 * 480 * 3], frame[248 * 328];
    bool has_scene, streaming, in_frame, level, lsb, falling, vsync_high;
    bool data_vsync, sync_start_pending, sync_end_pending;
    unsigned width, height, x, y, bit;
    uint64_t rate, half_cycles, line_cycles, lead_cycles, frame_cycles,
        sync_end_cycles;
    int64_t frame_start;
    bool (*prepare)(void *, Error **);
    void *prepare_opaque;
};

static void update(BF3901State *s, bool interrupt);

static void sensor_reset(DeviceState *dev) {
    BF3901State *s = BF3901(dev);
    memset(s->regs, 0, sizeof(s->regs));
    /* BF3901 TS-SEN-PD-0020 Rev.A/0, register summary. */
    static const uint8_t defaults[][2] = {
        {0x01, 0x19}, {0x02, 0x15}, {0x03, 0x00}, {0x08, 0xa0}, {0x62, 0x01},
        {0x09, 0x10}, {0x0a, 0x11}, {0x0b, 0x00}, {0x0c, 0x00}, {0x0e, 0x10},
        {0x0f, 0x10}, {0x10, 0x11}, {0x11, 0x30}, {0x12, 0x20}, {0x13, 0xc7},
        {0x15, 0x02}, {0x17, 0x00}, {0x18, 0x78}, {0x19, 0x00}, {0x1a, 0xa0},
        {0x1b, 0x80}, {0x1c, 0x7f}, {0x1d, 0xa2}, {0x1e, 0x09}, {0x1f, 0x10},
        {0x20, 0x09}, {0x22, 0x10}, {0x23, 0x33}, {0x24, 0x41}, {0x25, 0x88},
        {0x26, 0x10}, {0x2a, 0x00}, {0x2b, 0x06}, {0x2c, 0x10}, {0x2d, 0x10},
        {0x2e, 0x10}, {0x30, 0x28}, {0x31, 0x14}, {0x33, 0x10}, {0x34, 0x1d},
        {0x35, 0x46}, {0x37, 0xa4}, {0x38, 0x7c}, {0x3a, 0x00}, {0x3d, 0x11},
        {0x3e, 0x03}, {0x40, 0x28}, {0x41, 0x28}, {0x42, 0x30}, {0x44, 0x23},
        {0x45, 0x1b}, {0x46, 0x17}, {0x47, 0x0f}, {0x48, 0x0c}, {0x49, 0x0b},
        {0x4a, 0x0c}, {0x4b, 0x09}, {0x4c, 0x08}, {0x4e, 0x07}, {0x4f, 0x05},
        {0x50, 0x04}, {0x51, 0x0d}, {0x52, 0x0e}, {0x53, 0x42}, {0x54, 0x4c},
        {0x55, 0x00}, {0x56, 0x40}, {0x57, 0x76}, {0x58, 0x21}, {0x5a, 0x96},
        {0x5c, 0x20}, {0x60, 0xe5}, {0x61, 0x48}, {0x65, 0x46}, {0x66, 0x46},
        {0x67, 0x80}, {0x68, 0x80}, {0x69, 0x00}, {0x6a, 0x81}, {0x6b, 0x42},
        {0x6d, 0x20}, {0x6f, 0x7f}, {0x70, 0x0f}, {0x71, 0x46}, {0x72, 0x2f},
        {0x73, 0x2f}, {0x74, 0x27}, {0x75, 0x12}, {0x76, 0x10}, {0x7a, 0x00},
        {0x7b, 0x55}, {0x7c, 0x88}, {0x7d, 0xba}, {0x7e, 0x1a}, {0x80, 0x96},
        {0x81, 0xe0}, {0x82, 0x1b}, {0x83, 0x37}, {0x84, 0x39}, {0x85, 0x5d},
        {0x86, 0x77}, {0x8c, 0x01}, {0x8d, 0xcb}, {0x8f, 0x82}, {0x92, 0x02},
        {0x93, 0x00}, {0x98, 0x0a}, {0x9a, 0x50}, {0x9b, 0xa4}, {0x9c, 0x7c},
        {0x9d, 0x1b}, {0xa0, 0xd0}, {0xa1, 0x31}, {0xa2, 0x0b}, {0xa3, 0x20},
        {0xa4, 0x09}, {0xa5, 0x26}, {0xa6, 0x04}, {0xa7, 0x99}, {0xa8, 0x15},
        {0xa9, 0x12}, {0xaa, 0x12}, {0xab, 0x16}, {0xac, 0x3c}, {0xad, 0xf0},
        {0xae, 0x57}, {0xb0, 0xa0}, {0xb1, 0xc6}, {0xb2, 0xcc}, {0xb3, 0x4f},
        {0xb4, 0x60}, {0xb6, 0x80}, {0xb7, 0x80}, {0xb8, 0x80}, {0xb9, 0x00},
        {0xbb, 0x20}, {0xbc, 0x0c}, {0xbd, 0xa4}, {0xbe, 0x7c}, {0xc6, 0xaa},
        {0xc8, 0x0d}, {0xc9, 0x10}, {0xd0, 0x00}, {0xd1, 0x00}, {0xd3, 0x09},
        {0xd4, 0x24}, {0xda, 0x00}, {0xdb, 0xf8}, {0xdc, 0x00}, {0xdd, 0x48},
        {0xde, 0x10}, {0xe0, 0x00}, {0xe3, 0x06}, {0xe4, 0x00}, {0xe5, 0x1e},
        {0xe6, 0x19}, {0xe7, 0x06}, {0xef, 0x0b}, {0xf1, 0x00}, {0xf8, 0x7f},
        {0xf9, 0x00}, {0xfc, 0x39}, {0xfd, 0x01},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(defaults); i++) {
        s->regs[defaults[i][0]] = defaults[i][1];
    }
    s->address = 0;
    s->pointer = true;
    update(s, true);
}

static int event(I2CSlave *dev, enum i2c_event event) {
    BF3901State *s = BF3901(dev);
    if ((event == I2C_START_SEND || event == I2C_START_RECV) &&
        (!s->clock || s->power_down || !s->reset_released)) {
        return 1;
    }
    if (event == I2C_START_SEND) {
        s->pointer = true;
    }
    return 0;
}

static int sensor_send(I2CSlave *dev, uint8_t value) {
    BF3901State *s = BF3901(dev);
    if (s->pointer) {
        s->address = value;
        s->pointer = false;
        return 0;
    }
    uint8_t reg = s->address++;
    uint8_t previous = s->regs[reg];
    trace_bf3901_register(true, reg, value);
    if (reg == 0x12 && (value & 0x80)) {
        sensor_reset(DEVICE(s));
        s->pointer = false;
    } else if (reg != 0xfc && reg != 0xfd && reg != 0x1c && reg != 0x1d &&
               reg != 0x0f) {
        s->regs[reg] = value;
    }
    update(s, (reg == 0x11 || reg == 0x1b) && previous != value);
    return 0;
}

static uint8_t sensor_recv(I2CSlave *dev) {
    BF3901State *s = BF3901(dev);
    uint8_t reg = s->address++, value = s->regs[reg];
    trace_bf3901_register(false, reg, value);
    return value;
}

void bf3901_set_clock(DeviceState *dev, uint64_t hz) {
    BF3901State *s = BF3901(dev);
    if (s->clock != hz) {
        s->clock = hz;
        update(s, true);
    }
}

static void fail(const char *reason) {
    error_report("BF3901 unsupported %s", reason);
    exit(1);
}

static bool active(BF3901State *s) {
    return s->clock && s->reset_released && !s->power_down &&
           !(s->regs[0x09] & 16) && ((s->regs[0x11] >> 4) & 3) != 2 &&
           (s->regs[0x62] & 3);
}

static int64_t deadline(BF3901State *s, uint64_t clocks) {
    return s->frame_start + (clocks * 1000000000 + s->rate - 1) / s->rate;
}

static void update(BF3901State *s, bool interrupt) {
    if (!s->event) {
        return;
    }
    if (interrupt || !active(s) || !s->has_scene) {
        timer_del(s->event);
        s->streaming = s->in_frame = false;
        s->sync_start_pending = s->sync_end_pending = false;
        qemu_set_irq(s->pins[0], !(s->regs[0x15] & 2));
        qemu_set_irq(s->pins[1], !!(s->regs[0x15] & 16));
        qemu_set_irq(s->pins[2], 0);
    }
    if (active(s) && s->has_scene && !timer_pending(s->event)) {
        timer_mod(s->event, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

bool bf3901_set_frame(DeviceState *dev, const uint8_t *rgb, size_t length,
                      Error **errp) {
    BF3901State *s = BF3901(dev);
    if (length != sizeof(s->rgb)) {
        error_setg(errp, "BF3901 scene must be one 640x480 RGB888 frame");
        return false;
    }
    memcpy(s->rgb, rgb, length);
    s->has_scene = true;
    if (!s->streaming) {
        update(s, false);
    }
    return true;
}

void bf3901_clear_frame(DeviceState *dev) {
    BF3901State *s = BF3901(dev);
    s->has_scene = false;
    update(s, true);
}

void bf3901_set_prepare(DeviceState *dev, bool (*fn)(void *, Error **),
                        void *opaque) {
    BF3901State *s = BF3901(dev);
    s->prepare = fn;
    s->prepare_opaque = opaque;
}

static void prepare_frame(BF3901State *s) {
    const uint8_t *r = s->regs;
    /* Application Note A/0 sections 3.2 and 8.2: raw, single serial lane,
     * no CCIR markers, PLL bypass. Other serializers remain unsupported. */
    unsigned format = r[0x12] & 5;
    if ((format != 1 && format != 5) || (r[0x62] & 3) != 1 ||
        !(r[0x08] & 0x80) || (r[0x6b] & 0x40) || !(r[0x1b] & 0x80) ||
        (r[0x0b] & 0x20) || r[0x0a] != 0x11 || r[0x10] != 0x11) {
        fail("serial format, subsampling or clock mode");
    }
    /* Raw output uses the front-end array window (Application Note 6.1).
     * The 0x17..0x1a backend crop applies to YUV/RGB output instead. */
    unsigned xs = 0, xe = 248, ys = 0, ye = 328;
    if (r[0x4a] & 2) {
        xs = r[0xda];
        xe = r[0xdb];
        ys = r[0xdc] | ((r[0xde] & 1) << 8);
        ye = r[0xdd] | ((r[0xde] & 0x20) << 3);
    }
    if (xe <= xs || ye <= ys || xe > 248 || ye > 328) {
        fail("array window");
    }
    s->width = xe - xs;
    s->height = ye - ys;
    unsigned div = 1;
    if (!(r[0x11] & 0x40)) {
        div = ((r[0x11] & 0x30) == 0x30 ? 4 : (r[0x11] & 0x30) == 0x10 ? 2 : 1);
        if (!(r[0x11] & 0x80)) {
            div *= 2;
        }
    }
    /* Raw pixels occupy two MCLK cycles; eight serialized bits per pixel.
     * Use half-SCLK units so both edges have absolute virtual timestamps. */
    s->rate = s->clock * 8;
    s->half_cycles = div;
    unsigned line = 294 + ((r[0x2a] & 0xf0) << 4) + (r[0x2b] & 0xfe);
    s->line_cycles = (uint64_t)line * 16 * div;
    unsigned before = r[0xe3] | (r[0xe4] << 8),
             after = r[0x92] | (r[0x93] << 8);
    s->lead_cycles = (uint64_t)before * s->line_cycles;
    /* Four blank rows and programmed dummy intervals: default 328 + 6 +
     * 2 + 4 = 340 rows (Application Note 3.2, COM12 register). */
    unsigned blank = 4, blank_mode = (r[0x4a] >> 4) & 3;
    if (blank_mode) {
        blank += blank_mode == 1 ? after / 2 : after;
    }
    s->frame_cycles = (before + s->height + after + blank) * s->line_cycles;
    s->data_vsync = r[0x15] & 0x20;
    s->sync_end_cycles =
        (before + s->height + (s->data_vsync ? 0 : after)) * s->line_cycles;
    s->lsb = !!(r[0x0c] & 0x40);
    s->falling = !!(r[0x15] & 16);
    s->vsync_high = !!(r[0x15] & 2);
    for (unsigned y = 0; y < s->height; y++) {
        for (unsigned x = 0; x < s->width; x++) {
            unsigned sx = xs + x, sy = ys + y;
            if (r[0x1e] & 0x20) {
                sx = 247 - sx;
            }
            if (r[0x1e] & 0x10) {
                sy = 327 - sy;
            }
            /* Center-crop the optical scene to the native array aspect ratio.
             */
            unsigned px = (640 - 480 * 248 / 328) / 2 + sx * 480 / 328;
            unsigned py = sy * 480 / 328;
            unsigned order =
                format == 5 && !(r[0x0c] & 0x80) ? (r[0x0c] >> 4) & 3 : 0;
            unsigned parity = ((sy & 1) << 1) | (sx & 1);
            static const unsigned bayer[4] = {2, 1, 1, 0};
            uint8_t v = s->rgb[(py * 640 + px) * 3 + bayer[parity ^ order]];
            if (r[0x12] & 2) {
                v = v == 0 ? 1 : v == 255 ? 254 : v;
            }
            s->frame[y * s->width + x] = v;
        }
    }
}

static void emit_bit(BF3901State *s) {
    unsigned shift = s->lsb ? s->bit : 7 - s->bit;
    qemu_set_irq(s->pins[2], (s->frame[s->y * s->width + s->x] >> shift) & 1);
}

static void tick(void *opaque) {
    BF3901State *s = opaque;
    if (!active(s) || !s->has_scene) {
        update(s, true);
        return;
    }
    if (!s->streaming) {
        Error *err = NULL;
        if (s->prepare && !s->prepare(s->prepare_opaque, &err)) {
            if (err) {
                error_report_err(err);
                exit(1);
            }
            return;
        }
        if (!active(s) || !s->has_scene) {
            return;
        }
        prepare_frame(s);
        s->frame_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->streaming = s->in_frame = true;
        s->x = s->y = s->bit = 0;
        s->level = false;
        s->sync_start_pending = s->data_vsync && s->lead_cycles;
        qemu_set_irq(s->pins[0], s->vsync_high ^ s->sync_start_pending);
        qemu_set_irq(s->pins[1], s->falling);
        emit_bit(s);
        timer_mod(
            s->event,
            deadline(s, s->lead_cycles +
                            (s->sync_start_pending ? 0 : s->half_cycles)));
        return;
    }
    if (s->sync_start_pending) {
        s->sync_start_pending = false;
        qemu_set_irq(s->pins[0], s->vsync_high);
        timer_mod(s->event, deadline(s, s->lead_cycles + s->half_cycles));
        return;
    }
    if (s->sync_end_pending) {
        s->sync_end_pending = false;
        qemu_set_irq(s->pins[0], !s->vsync_high);
        timer_mod(s->event, deadline(s, s->frame_cycles));
        return;
    }
    if (!s->in_frame) {
        s->streaming = false;
        tick(s);
        return;
    }
    s->level = !s->level;
    qemu_set_irq(s->pins[1], s->level ^ s->falling);
    if (!s->level) {
        if (++s->bit == 8) {
            s->bit = 0;
            if (++s->x == s->width) {
                s->x = 0;
                if (++s->y == s->height) {
                    s->in_frame = false;
                    s->sync_end_pending = true;
                    timer_mod(s->event, deadline(s, s->sync_end_cycles));
                    return;
                }
            }
        }
        emit_bit(s);
    }
    uint64_t at =
        s->lead_cycles + s->y * s->line_cycles +
        (s->x * 16 + s->bit * 2 + (s->level ? 2 : 1)) * s->half_cycles;
    timer_mod(s->event, deadline(s, at));
}

static void input(void *opaque, int pin, int level) {
    BF3901State *s = opaque;
    if (pin == 0) {
        if (s->reset_released == !!level) {
            return;
        }
        if (!level && s->reset_released) {
            sensor_reset(DEVICE(s));
        }
        s->reset_released = level;
    } else {
        if (s->power_down == !!level) {
            return;
        }
        s->power_down = level;
    }
    update(s, true);
}

static void instance_init(Object *obj) {
    BF3901State *s = BF3901(obj);
    s->reset_released = true;
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    qdev_init_gpio_out_named(DEVICE(s), s->pins, "serial", 3);
    qdev_init_gpio_in_named(DEVICE(s), input, "control", 2);
}

static void finalize(Object *obj) {
    BF3901State *s = BF3901(obj);
    timer_del(s->event);
    timer_free(s->event);
}

static void class_init(ObjectClass *oc, const void *data) {
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(oc);
    ic->event = event;
    ic->send = sensor_send;
    ic->recv = sensor_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), sensor_reset);
}

static const TypeInfo info = {
    .name = TYPE_BF3901,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(BF3901State),
    .instance_init = instance_init,
    .instance_finalize = finalize,
    .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
