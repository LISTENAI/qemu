/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OV2659 product specification v2.0, sections 2.6/2.9, 4.1-4.3 and 6.1.
 * The optical input is a calibrated post-ISP scene. Analog tuning registers
 * retain their values; the optical model has no exposure or noise feedback. */
#include "qemu/osdep.h"
#include "hw/i2c/ov2659.h"
#include "qemu/log.h"

#define SCENE_WIDTH 640
#define SCENE_HEIGHT 480
#define SCENE_BYTES (SCENE_WIDTH * SCENE_HEIGHT * 3)

struct OV2659State {
    I2CSlave parent;
    uint8_t regs[0x5601];
    uint16_t address;
    unsigned address_bytes;
    bool clock, released, power_down, has_frame;
    uint8_t rgb[SCENE_BYTES], frame_rgb[SCENE_BYTES];
    unsigned width, height, crop_x, crop_y, crop_w, crop_h;
    bool mirror, flip, colorbar;
    unsigned cached_line, cached_pair;
    uint32_t cached_yuyv;
    void (*notify)(void *, bool);
    void *opaque;
};

static bool ready(OV2659State *s)
{
    return s->clock && s->released && !s->power_down;
}

static void changed(OV2659State *s, bool interrupted)
{
    if (s->notify) {
        s->notify(s->opaque, interrupted);
    }
}

static void reset(DeviceState *dev)
{
    OV2659State *s = OV2659(dev);
    /* Digital path reset values from tables 7-1, 7-2, 7-4 and 7-5. */
    static const uint8_t defaults[0x5601] = {
        [0x3004] = 0x10, [0x3005] = 0x24, [0x3006] = 0x0d,
        [0x300a] = 0x26, [0x300b] = 0x56, [0x302b] = 0x60,
        [0x3400] = 4, [0x3402] = 4, [0x3404] = 4, [0x3501] = 2,
        [0x350b] = 0x10,
        [0x3804] = 6, [0x3805] = 0x5f, [0x3806] = 4, [0x3807] = 0xbb,
        [0x3808] = 6, [0x3809] = 0x40, [0x380a] = 4, [0x380b] = 0xb0,
        [0x380c] = 7, [0x380d] = 0x9c, [0x380e] = 4, [0x380f] = 0xd0,
        [0x3811] = 0x10, [0x3813] = 6, [0x3814] = 0x11, [0x3815] = 0x11,
        [0x3820] = 0x80, [0x4300] = 0xf8, [0x4708] = 1,
    };
    memcpy(s->regs, defaults, sizeof(defaults));
    s->address = s->address_bytes = 0;
    changed(s, true);
}

static bool known(uint16_t r)
{
    switch (r) {
    case 0x0100: case 0x0103: case 0x3000 ... 0x3006:
    case 0x300a: case 0x300b: case 0x302a: case 0x302b:
    case 0x3400 ... 0x3406: case 0x3500 ... 0x350d:
    case 0x3612: case 0x3620: case 0x3623: case 0x3630 ... 0x3634:
    case 0x3701 ... 0x3705: case 0x370a: case 0x370c:
    case 0x3800 ... 0x3815: case 0x3820: case 0x3821:
    case 0x3a00 ... 0x3a1f: case 0x4000 ... 0x4009:
    case 0x4300: case 0x4608: case 0x4609:
    case 0x4702: case 0x4708: case 0x4709:
    case 0x5000 ... 0x5006: case 0x500c ... 0x5020:
    case 0x5025 ... 0x504b: case 0x5051 ... 0x5054:
    case 0x5060 ... 0x5086: case 0x50a0: case 0x5600:
        return true;
    default:
        return false;
    }
}

static int event(I2CSlave *dev, enum i2c_event e)
{
    OV2659State *s = OV2659(dev);
    if (e == I2C_START_SEND) {
        s->address_bytes = 0;
    }
    return (e == I2C_START_SEND || e == I2C_START_RECV) && !ready(s);
}

static int send_byte(I2CSlave *dev, uint8_t data)
{
    OV2659State *s = OV2659(dev);
    if (!ready(s)) {
        return 1;
    }
    if (s->address_bytes < 2) {
        s->address = (s->address << 8) | data;
        return ++s->address_bytes == 2 && !known(s->address);
    }
    uint16_t r = s->address++;
    if (!known(r) || r == 0x300a || r == 0x300b ||
        (r == 0x302b && data != 0x60)) {
        return 1;
    }
    if (r == 0x0103 && (data & 1)) {
        reset(DEVICE(dev));
    } else {
        uint8_t old = s->regs[r];
        s->regs[r] = data;
        if (old != data && (r == 0x0100 || (r >= 0x3000 && r <= 0x3002))) {
            changed(s, true);
        } else if (old != data && r == 0x50a0) {
            changed(s, false);
        }
    }
    return 0;
}

static uint8_t recv_byte(I2CSlave *dev)
{
    OV2659State *s = OV2659(dev);
    uint16_t r = s->address++;
    if (!ready(s) || !known(r)) {
        qemu_log_mask(LOG_GUEST_ERROR, "OV2659 unavailable register 0x%04x\n", r);
        return 0xff;
    }
    return s->regs[r];
}

bool ov2659_set_frame(DeviceState *dev, const uint8_t *rgb, size_t n, Error **errp)
{
    OV2659State *s = OV2659(dev);
    if (n != SCENE_BYTES) {
        error_setg(errp, "OV2659 scene must be one 640x480 RGB888 image");
        return false;
    }
    memcpy(s->rgb, rgb, SCENE_BYTES);
    s->has_frame = true;
    return true;
}

void ov2659_clear_frame(DeviceState *dev)
{
    OV2659(dev)->has_frame = false;
}

static unsigned pair(const uint8_t *r)
{
    return (r[0] << 8) | r[1];
}

bool ov2659_begin_frame(DeviceState *dev, OV2659Output *out, Error **errp)
{
    OV2659State *s = OV2659(dev);
    const uint8_t *r = s->regs;
    if (!ready(s) || !(r[0x0100] & 1) || (r[0x3000] & 3) != 3 ||
        (r[0x3001] & 0xfc) != 0xfc || (r[0x3002] & 0xe0) != 0xe0) {
        return false;
    }
    bool bars = r[0x50a0] & 0x80;
    unsigned w = pair(r + 0x3808), h = pair(r + 0x380a);
    unsigned xoff = pair(r + 0x3810), yoff = pair(r + 0x3812);
    bool binned = r[0x3821] & 1;
    bool full = binned && xoff == 8 && yoff == 2 && w == 400 && h == 300;
    bool roi = w == 320 && h == 240;
    bool still = w == 1280 && h == 960;
    unsigned inc = r[0x3814];
    /* The active 1600x1200 optical field excludes the dummy border.
     * Both preview readout schemes place the centered 1280x960 ROI at
     * (160,120); the 400x300 factory preset covers the full field. */
    bool crop = ((binned && roi && xoff == 88 && yoff == 62) ||
                 (!binned && (roi || still) && xoff == 176 && yoff == 126));
    if ((!full && !crop) || pair(r + 0x3800) || pair(r + 0x3802) ||
        pair(r + 0x3804) != 0x065f || pair(r + 0x3806) != (binned ? 1207 : 1211) ||
        inc != (still ? 0x11 : 0x31) || r[0x3815] != inc ||
        !!(r[0x3820] & 1) != binned || !!(r[0x5002] & 0x10) == still ||
        ((r[0x3820] & 6) != 0 && (r[0x3820] & 6) != 6) ||
        ((r[0x3821] & 6) != 0 && (r[0x3821] & 6) != 6)) {
        error_setg(errp, "Unsupported OV2659 readout window %ux%u+%u+%u", w, h, xoff, yoff);
        return false;
    }
    if (r[0x4300] != 0x30 || r[0x4709] || (bars && r[0x50a0] != 0x80) ||
        (r[0x4708] & ~7) || r[0x4702] != 2) {
        error_setg(errp, "Unsupported OV2659 DVP output format or test pattern");
        return false;
    }
    if (!s->has_frame && !bars) {
        return false;
    }
    unsigned hts = pair(r + 0x380c), vts = pair(r + 0x380e);
    /* Datasheet section 2.9: the supported PLL path divides XVCLK by 3
     * and 2, then by twice the high nibble of 0x3004. One PCLK per byte. */
    unsigned mult = r[0x3005], sysdiv = r[0x3004] >> 4;
    if (!mult || mult > 63 || !sysdiv || (r[0x3004] & 15) ||
        r[0x3003] || r[0x3006] != 0x0d || hts < w || vts < h) {
        error_setg(errp, "Unsupported OV2659 PLL or line/frame timing");
        return false;
    }
    *out = (OV2659Output) {
        .width = w, .height = h, .hts = hts, .vts = vts,
        .pll_multiplier = mult, .pll_divisor = 12 * sysdiv,
        .invert_href = !!(r[0x4708] & 4), .invert_vsync = !!(r[0x4708] & 2),
        .colorbar = bars,
    };
    s->width = w; s->height = h;
    s->crop_x = full ? 0 : 64; s->crop_y = full ? 0 : 48;
    s->crop_w = SCENE_WIDTH - 2 * s->crop_x;
    s->crop_h = SCENE_HEIGHT - 2 * s->crop_y;
    s->mirror = r[0x3821] & 2; s->flip = r[0x3820] & 2;
    s->colorbar = bars; s->cached_line = UINT_MAX;
    if (!bars) {
        memcpy(s->frame_rgb, s->rgb, SCENE_BYTES);
    }
    return true;
}

static const uint8_t *pixel(OV2659State *s, unsigned x, unsigned y)
{
    static const uint8_t bars[8][3] = {
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0},
    };
    if (s->mirror) { x = s->width - 1 - x; }
    if (s->flip) { y = s->height - 1 - y; }
    if (s->colorbar) {
        return bars[x * 8 / s->width];
    }
    x = s->crop_x + x * s->crop_w / s->width;
    y = s->crop_y + y * s->crop_h / s->height;
    return s->frame_rgb + (y * SCENE_WIDTH + x) * 3;
}

uint8_t ov2659_sample(void *opaque, unsigned byte, unsigned line)
{
    OV2659State *s = OV2659(opaque);
    unsigned group = byte / 4;
    if (group != s->cached_pair || line != s->cached_line) {
        const uint8_t *a = pixel(s, group * 2, line);
        const uint8_t *b = pixel(s, group * 2 + 1, line);
        int r = a[0] + b[0], g = a[1] + b[1], blue = a[2] + b[2];
        uint32_t y0 = (77*a[0] + 150*a[1] + 29*a[2] + 128) >> 8;
        uint32_t y1 = (77*b[0] + 150*b[1] + 29*b[2] + 128) >> 8;
        uint32_t cb = CLAMP(128 + ((-43*r - 85*g + 128*blue + 256) >> 9), 0, 255);
        uint32_t cr = CLAMP(128 + ((128*r - 107*g - 21*blue + 256) >> 9), 0, 255);
        s->cached_yuyv = y0 | (cb << 8) | (y1 << 16) | (cr << 24);
        s->cached_pair = group;
        s->cached_line = line;
    }
    return s->cached_yuyv >> ((byte % 4) * 8);
}

void ov2659_set_clock(DeviceState *dev, bool enabled)
{
    OV2659(dev)->clock = enabled;
}

void ov2659_set_notify(DeviceState *dev, void (*notify)(void *, bool), void *opaque)
{
    OV2659(dev)->notify = notify;
    OV2659(dev)->opaque = opaque;
}

static void control_input(void *opaque, int pin, int level)
{
    OV2659State *s = opaque;
    if (pin == 0 && s->released != !!level) {
        s->released = level;
        if (!level) {
            reset(DEVICE(s));
        } else {
            changed(s, true);
        }
    } else if (pin == 1 && s->power_down != !!level) {
        s->power_down = level;
        changed(s, true);
    }
}

static void instance_init(Object *obj)
{
    OV2659(obj)->released = true;
    qdev_init_gpio_in_named(DEVICE(obj), control_input, "control", 2);
}

static void class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->event = event;
    ic->send = send_byte;
    ic->recv = recv_byte;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), reset);
}

static const TypeInfo ov2659_info = {
    .name = TYPE_OV2659, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(OV2659State), .instance_init = instance_init,
    .class_init = class_init,
};

static void register_types(void) { type_register_static(&ov2659_info); }
type_init(register_types)
