/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared Sitronix RGB565 GRAM and scan engine; controller protocols differ. */
#include "qemu/osdep.h"
#include "hw/display/sitronix_lcd.h"
#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qobject/qnum.h"
#include "ui/console.h"

typedef struct SitronixLCDState {
    SSIPeripheral parent;
    QemuConsole *console;
    QEMUTimer *scan;
    qemu_irq te;
    uint16_t gram[240 * 320], scanned[240 * 320], presented[240 * 320];
    uint8_t scanned_flags[320], presented_flags[320];
    uint8_t command, madctl, parameters[32], analog[256][32], analog_count[256];
    uint32_t width, height, x_offset, y_offset, rotation;
    unsigned x_start, x_end, y_start, y_end, x, y, count, format;
    int pixel_high;
    bool panel_inverted, inverted, sleeping, on, tear, reset_released, data_mode, dirty;
    double backlight;
    uint64_t pixels;
    int64_t scan_start, line_ns;
    unsigned scanned_rows;
    bool scanning, blanking, scan_reverse;
    bool sc919, command2;
    uint8_t packet[4];
    unsigned packet_count, lanes;
    int64_t reset_start, ready_ns, sleep_ready_ns;
    void (*failure)(void *, const char *);
    void *failure_opaque;
} SitronixLCDState;
OBJECT_DECLARE_SIMPLE_TYPE(SitronixLCDState, SITRONIX_LCD)

static void fail(SitronixLCDState *s, const char *message, unsigned value)
{
    error_report("%s %s (command=0x%02x value=0x%x)",
                 s->sc919 ? "SC919" : "ST7789", message, s->command, value);
    if (s->failure) { s->failure(s->failure_opaque, "unsupported-panel"); }
    exit(1);
}

static void update(void *opaque);

/* Complete rows before a GRAM/control write. This gives each row its virtual
 * sampling time without adding a timer callback for every scan line. */
static void scan_rows(SitronixLCDState *s, int64_t now)
{
    if (!s->scanning || s->blanking || now <= s->scan_start) { return; }
    unsigned rows = MIN((now - s->scan_start) / s->line_ns, 320);
    while (s->scanned_rows < rows) {
        unsigned y = s->scan_reverse ? 319 - s->scanned_rows : s->scanned_rows;
        memcpy(s->scanned + y * 240, s->gram + y * 240, 240 * sizeof(uint16_t));
        s->scanned_flags[y] = (s->madctl & 8) | (s->inverted ? 1 : 0);
        s->scanned_rows++;
    }
}

static int64_t line_period(SitronixLCDState *s)
{
    /* ST7789P3 FRCTRL2 / SC919 v0.0 FRCTRL1: 10 MHz oscillator,
     * 250 + 16 * RTNA clocks/line, with programmed front/back porches. */
    return (250 + 16 * (s->analog[s->sc919 ? 0xb3 : 0xc6][0] & 31)) * 100;
}

static void schedule(SitronixLCDState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->sleeping || !s->reset_released) {
        timer_del(s->scan); s->scanning = false;
        qemu_set_irq(s->te, 0);
    } else if (!s->scanning) {
        s->scanning = s->blanking = true;
        s->line_ns = line_period(s);
        s->scan_start = now + s->analog[0xb2][0] * s->line_ns;
        timer_mod(s->scan, s->scan_start);
    }
    qemu_set_irq(s->te, s->scanning && s->blanking && s->tear);
}

static void scan(void *opaque)
{
    SitronixLCDState *s = opaque;
    int64_t now = s->scan_start + (s->blanking ? 0 : 320 * s->line_ns);
    if (s->blanking) {
        s->blanking = false; s->scanned_rows = 0;
        s->scan_reverse = s->madctl & 16;
        qemu_set_irq(s->te, 0);
        timer_mod(s->scan, s->scan_start + 320 * s->line_ns);
        return;
    }
    scan_rows(s, now);
    assert(s->scanned_rows == 320);
    if (memcmp(s->presented, s->scanned, sizeof(s->presented)) ||
        memcmp(s->presented_flags, s->scanned_flags, sizeof(s->presented_flags))) {
        memcpy(s->presented, s->scanned, sizeof(s->presented));
        memcpy(s->presented_flags, s->scanned_flags, sizeof(s->presented_flags));
        s->dirty = true;
    }
    /* Publish at the completed scan's timestamp. GUI refresh never samples
     * the live GRAM, and cannot advance or change the panel scan phase. */
    update(s);
    s->blanking = true;
    s->line_ns = line_period(s);
    s->scan_start = now + (s->analog[0xb2][0] + s->analog[0xb2][1]) * s->line_ns;
    qemu_set_irq(s->te, s->tear);
    timer_mod(s->scan, s->scan_start);
}

static void reset_controller(SitronixLCDState *s)
{
    s->count = 0; memset(s->analog_count, 0, sizeof(s->analog_count));
    s->analog[0xc6][0] = 15;
    s->analog[0xb3][0] = 15;
    s->command2 = false;
    s->analog[0xb2][0] = s->analog[0xb2][1] = 12;
    s->sleeping = true; s->on = s->tear = s->inverted = false;
    s->madctl = s->command = 0; s->format = 6; s->pixel_high = -1;
    s->x_start = s->y_start = s->x = s->y = 0; s->x_end = 239; s->y_end = 319;
    s->dirty = true; qemu_set_irq(s->te, 0); schedule(s);
}

static uint32_t transfer(SSIPeripheral *dev, uint32_t data)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    if (!s->reset_released) { return 0; }
    scan_rows(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (!s->data_mode) {
        if (s->pixel_high >= 0) { fail(s, "incomplete RGB565 pixel", data); }
        s->command = data; s->count = 0;
        switch (data) {
        case 0: case 0x13: break;
        case 1: reset_controller(s); break;
        case 0x10: s->dirty = true; s->sleeping = true; break;
        case 0x11: s->dirty = true; s->sleeping = false; break;
        case 0x20: s->inverted = false; break;
        case 0x21: s->inverted = true; break;
        case 0x28: s->dirty = true; s->on = false; break;
        case 0x29: s->dirty = true; s->on = true; break;
        case 0x2c: s->x = s->x_start; s->y = s->y_start; break;
        case 0x3c:
            if (!s->sc919) { fail(s, "unimplemented command", data); }
            break;
        case 0x34: s->tear = false; qemu_set_irq(s->te, 0); break;
        case 0x2a: case 0x2b: case 0x35: case 0x36: case 0x3a:
        case 0x26: case 0xb0: case 0xb1: case 0xb2: case 0xb7: case 0xba:
        case 0xbb: case 0xc0: case 0xc2: case 0xc3: case 0xc4:
        case 0xc6: case 0xd0: case 0xd6: case 0xdf: case 0xe0: case 0xe1: break;
        default:
            if (!s->sc919 || !((data >= 0x70 && data <= 0x8e) ||
                               data == 0xb3 || data == 0xe4 || data == 0xe5)) {
                fail(s, "unimplemented command", data);
            }
        }
        schedule(s); return 0;
    }
    if (s->command == 0x2c || (s->sc919 && s->command == 0x3c)) {
        if (s->format != 5) { fail(s, "requires RGB565", s->format); }
        if (s->pixel_high < 0) { s->pixel_high = data; }
        else {
            int x = s->madctl & 0x20 ? s->y : s->x;
            int y = s->madctl & 0x20 ? s->x : s->y;
            if (s->madctl & 0x40) { x = 239 - x; }
            if (s->madctl & 0x80) { y = 319 - y; }
            if (x < 0 || x >= 240 || y < 0 || y >= 320) { fail(s, "pixel outside GRAM", data); }
            s->gram[y * 240 + x] = (s->pixel_high << 8) | data;
            s->pixel_high = -1; s->pixels++;
            if (++s->x > s->x_end) { s->x = s->x_start; if (++s->y > s->y_end) { s->y = s->y_start; } }
        }
        return 0;
    }
    if (s->count == sizeof(s->parameters)) { fail(s, "too many parameters", data); }
    s->parameters[s->count++] = data;
    if (s->sc919 && s->command >= 0x70) {
        if (s->command != 0xdf && !s->command2) {
            /* Command-table 2 writes are gated by CMD2EN, with EXTC low. */
            return 0;
        }
        switch (s->command) {
        case 0xdf:
            if (s->count > 4 || (s->count <= 3 && data != (uint8_t[]){0x5a,0x69,0x02}[s->count-1]) ||
                (s->count == 4 && (data & 0x30))) { fail(s, "unsupported command-table control", data); }
            if (s->count == 4) { s->command2 = data & 1; }
            break;
        case 0xb0:
            if (s->count > 2 || (s->count == 1 && data != 0x80) ||
                (s->count == 2 && data != 0xf0)) { fail(s, "unsupported RAM interface", data); }
            break;
        case 0xb2:
            if (s->count <= 2 && (!data || (data & 0x80))) { fail(s, "unsupported porch", data); }
            break;
        case 0xe4:
            if (s->count > 4 || data != (uint8_t[]){0,0x78,0xa0,0}[s->count-1]) {
                fail(s, "unsupported scan resolution", data);
            }
            break;
        }
        /* Analog drive/gamma controls are retained, with ideal panel voltages.
         * Digital porch, frame rate, RAM mode and resolution are handled above.
         * SC919 preliminary v0.0 and the module driver include reserved tails. */
        memcpy(s->analog[s->command], s->parameters, s->count);
        s->analog_count[s->command] = s->count;
        return 0;
    }
    switch (s->command) {
    case 0x2a: case 0x2b:
        if (s->count == 4) {
            unsigned first = (s->parameters[0] << 8) | s->parameters[1];
            unsigned last = (s->parameters[2] << 8) | s->parameters[3];
            unsigned limit = ((s->command == 0x2a) == !(s->madctl & 0x20)) ? 240 : 320;
            if (first > last || last >= limit) { fail(s, "window outside GRAM", last); }
            if (s->command == 0x2a) { s->x_start = first; s->x_end = last; }
            else { s->y_start = first; s->y_end = last; }
        }
        break;
    case 0x3a:
        if ((data & 7) != 5) { fail(s, "unsupported pixel format", data); }
        s->format = 5; break;
    case 0x36: s->madctl = data; break;
    case 0x35:
        if (data) { fail(s, "horizontal TE is not modeled", data); }
        s->tear = true; schedule(s); break;
    case 0xb2:
        if (s->count > 5 || (s->count <= 2 && (data & 0x80)) ||
            (s->count == 3 && data != 0)) {
            fail(s, "unsupported porch setting", data);
        }
        s->analog[0xb2][s->count - 1] = data;
        s->analog_count[0xb2] = s->count;
        break;
    case 0xb0:
        /* Serial DBI RGB565, MSB first. Other RAM interface/endian modes
         * require distinct pixel transport and must not silently render. */
        if (s->count > 2 || s->parameters[0] != 0 ||
            (s->count == 2 && data != 0xf0)) {
            fail(s, "unsupported RAM interface", data);
        }
        memcpy(s->analog[s->command], s->parameters, s->count);
        s->analog_count[s->command] = s->count; break;
    case 0xba:
        if (s->count != 1 || data != 0) { fail(s, "digital gamma is not modeled", data); }
        s->analog[s->command][0] = data; s->analog_count[s->command] = 1; break;
    default:
        memcpy(s->analog[s->command], s->parameters, s->count);
        s->analog_count[s->command] = s->count; break;
    }
    return 0;
}

void sitronix_lcd_qspi_lanes(DeviceState *dev, unsigned lanes)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    assert(s->sc919 && (lanes == 1 || lanes == 4));
    s->lanes = lanes;
}

static uint32_t sc919_transfer(SSIPeripheral *dev, uint32_t data)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    if (!s->reset_released) { return 0; }
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->ready_ns) { fail(s, "reset recovery", data); }
    if (s->packet_count < 4) {
        unsigned i = s->packet_count++;
        unsigned lanes = i && s->packet[0] == 0x38 ? 4 : 1;
        if (s->lanes != lanes) { fail(s, "QSPI header lane count", s->lanes); }
        s->packet[i] = data;
        if (i == 0 && data != 0x02 && data != 0x32 && data != 0x38) {
            fail(s, "unsupported QSPI instruction", data);
        }
        if ((i == 1 || i == 3) && data) { fail(s, "QSPI command address", data); }
        if (i == 3) {
            if (s->packet[2] == 0x11 && qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->sleep_ready_ns) {
                fail(s, "sleep-out during reset recovery", s->packet[2]);
            }
            s->data_mode = false; transfer(dev, s->packet[2]); s->data_mode = true;
        }
    } else {
        if (s->lanes != (s->packet[0] == 0x02 ? 1 : 4)) { fail(s, "QSPI payload lane count", s->lanes); }
        transfer(dev, data);
    }
    return 0;
}

static int sc919_cs(SSIPeripheral *dev, bool level)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    if (level && s->reset_released) {
        if (s->packet_count && s->packet_count < 4) { fail(s, "incomplete QSPI header", s->packet_count); }
        if (s->pixel_high >= 0) { fail(s, "incomplete RGB565 pixel", s->pixel_high); }
        if (s->packet_count == 4 &&
            (((s->command == 0x2a || s->command == 0x2b || s->command == 0xdf) && s->count != 4) ||
             ((s->command == 0x35 || s->command == 0x36 || s->command == 0x3a) && s->count != 1))) {
            fail(s, "incomplete command parameters", s->count);
        }
    }
    s->packet_count = 0;
    return 0;
}

static bool enabled(SitronixLCDState *s)
{
    return s->on && !s->sleeping && s->reset_released;
}

static uint32_t rgb(SitronixLCDState *s, unsigned x, unsigned y)
{
    unsigned px = x, py = y;
    switch (s->rotation) {
    case 90: px = s->width - 1 - y; py = x; break;
    case 180: px = s->width - 1 - x; py = s->height - 1 - y; break;
    case 270: px = y; py = s->height - 1 - x; break;
    }
    unsigned row = py + s->y_offset;
    uint16_t pixel = s->presented[row * 240 + px + s->x_offset];
    uint8_t flags = s->presented_flags[row];
    unsigned r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
    if (flags & 8) { unsigned tmp = r; r = b; b = tmp; }
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    if (!!(flags & 1) != s->panel_inverted) { r = 255 - r; g = 255 - g; b = 255 - b; }
    double duty = enabled(s) ? s->backlight : 0;
    return ((uint32_t)(r * duty) << 16) | ((uint32_t)(g * duty) << 8) | (uint32_t)(b * duty);
}

static void update(void *opaque)
{
    SitronixLCDState *s = opaque;
    if (!s->dirty) { return; }
    DisplaySurface *surface = qemu_console_surface(s->console);
    assert(surface_bits_per_pixel(surface) == 32);
    for (unsigned y = 0; y < surface_height(surface); y++) {
        uint32_t *row = (uint32_t *)(surface_data(surface) + y * surface_stride(surface));
        for (unsigned x = 0; x < surface_width(surface); x++) { row[x] = rgb(s, x, y); }
    }
    s->dirty = false;
    dpy_gfx_update(s->console, 0, 0, surface_width(surface), surface_height(surface));
}

static void invalidate(void *opaque) { ((SitronixLCDState *)opaque)->dirty = true; }
static const GraphicHwOps graphics = { .invalidate = invalidate, .gfx_update = update };

static void input(void *opaque, int pin, int level)
{
    SitronixLCDState *s = opaque;
    if (pin == 0) { s->data_mode = level; }
    else {
        if (s->reset_released == !!level) { return; }
        bool asserted = !level;
        s->reset_released = level;
        if (s->sc919) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if (asserted) { s->reset_start = now; }
            else if (now - s->reset_start >= 10000) {
                reset_controller(s); s->packet_count = 0;
                s->ready_ns = now + 5000000;
                s->sleep_ready_ns = now + 120000000;
            }
        } else if (asserted) { reset_controller(s); }
        s->dirty = true; schedule(s);
    }
}

void sitronix_lcd_backlight(DeviceState *dev, double duty)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    assert(duty >= 0 && duty <= 1);
    if (s->backlight != duty) { s->backlight = duty; s->dirty = true; }
}

void sitronix_lcd_save(DeviceState *dev, const char *path)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    unsigned width = s->rotation % 180 ? s->height : s->width;
    unsigned height = s->rotation % 180 ? s->width : s->height;
    FILE *f = fopen(path, "wb");
    if (!f) { perror("Sitronix LCD frame"); exit(1); }
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (unsigned y = 0; y < height; y++) {
        uint8_t row[320 * 3];
        for (unsigned x = 0; x < width; x++) {
            uint32_t color = rgb(s, x, y);
            row[x * 3] = color >> 16; row[x * 3 + 1] = color >> 8; row[x * 3 + 2] = color;
        }
        if (fwrite(row, 3, width, f) != width) { perror("Sitronix LCD frame"); exit(1); }
    }
    if (fclose(f)) { perror("Sitronix LCD frame"); exit(1); }
}

void sitronix_lcd_report(DeviceState *dev, FILE *f)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    fprintf(f, "{\"pixels_written\":%" PRIu64 ",\"enabled\":%s,\"backlight\":%.9f}",
            s->pixels, enabled(s) ? "true" : "false", s->backlight);
}

QDict *sitronix_lcd_display_state(DeviceState *dev)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    QDict *state = qdict_new();
    qdict_put_bool(state, "enabled", enabled(s));
    qdict_put_int(state, "pixels_written", s->pixels);
    qdict_put_obj(state, "drive_duty", QOBJECT(qnum_from_double(s->backlight)));
    qdict_put_obj(state, "effective_duty", QOBJECT(qnum_from_double(enabled(s) ? s->backlight : 0)));
    return state;
}

void sitronix_lcd_failure_callback(DeviceState *dev, void (*fn)(void *, const char *), void *opaque)
{
    SitronixLCDState *s = SITRONIX_LCD(dev); s->failure = fn; s->failure_opaque = opaque;
}

static void reset(DeviceState *dev)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    s->packet_count = 0;
    s->ready_ns = s->sleep_ready_ns = 0;
    reset_controller(s); memset(s->gram, 0, sizeof(s->gram)); s->pixels = 0;
    memset(s->scanned, 0, sizeof(s->scanned));
    memset(s->presented, 0, sizeof(s->presented));
    memset(s->scanned_flags, 0, sizeof(s->scanned_flags));
    memset(s->presented_flags, 0, sizeof(s->presented_flags));
    /* Pin levels and backlight belong to the board, not controller reset. */
}

void sitronix_lcd_redrive(DeviceState *dev)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    qemu_set_irq(s->te, s->scanning && s->blanking && s->tear);
}

static void realize(SSIPeripheral *dev, Error **errp)
{
    SitronixLCDState *s = SITRONIX_LCD(dev);
    if (!s->width || !s->height || s->width > 240 || s->height > 320 ||
        s->x_offset > 240 - s->width || s->y_offset > 320 - s->height ||
        s->rotation > 270 || s->rotation % 90) {
        error_setg(errp, "Invalid Sitronix LCD viewport or mounting rotation"); return;
    }
    s->scan = timer_new_ns(QEMU_CLOCK_VIRTUAL, scan, s);
    s->reset_released = true;
    qdev_init_gpio_in(DEVICE(s), input, 2);
    qdev_init_gpio_out_named(DEVICE(s), &s->te, "te", 1);
    s->console = graphic_console_init(DEVICE(s), 0, &graphics, s);
    qemu_console_resize(s->console, s->rotation % 180 ? s->height : s->width,
                         s->rotation % 180 ? s->width : s->height);
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("width", SitronixLCDState, width, 240),
    DEFINE_PROP_UINT32("height", SitronixLCDState, height, 320),
    DEFINE_PROP_UINT32("x-offset", SitronixLCDState, x_offset, 0),
    DEFINE_PROP_UINT32("y-offset", SitronixLCDState, y_offset, 0),
    DEFINE_PROP_UINT32("rotation", SitronixLCDState, rotation, 0),
    DEFINE_PROP_BOOL("panel-inverted", SitronixLCDState, panel_inverted, false),
};

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);
    sc->realize = realize; sc->transfer = transfer; sc->cs_polarity = SSI_CS_LOW;
    device_class_set_legacy_reset(dc, reset); device_class_set_props(dc, properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}
static const TypeInfo info = {
    .name = TYPE_SITRONIX_LCD, .parent = TYPE_SSI_PERIPHERAL, .abstract = true,
    .instance_size = sizeof(SitronixLCDState), .class_init = class_init,
};
static void sc919_init(Object *obj) { SITRONIX_LCD(obj)->sc919 = true; }
static void sc919_class(ObjectClass *klass, const void *data)
{
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);
    sc->transfer = sc919_transfer; sc->set_cs = sc919_cs;
}
static const TypeInfo variants[] = {
    { .name = TYPE_ST7789, .parent = TYPE_SITRONIX_LCD },
    { .name = TYPE_SC919, .parent = TYPE_SITRONIX_LCD,
      .instance_init = sc919_init, .class_init = sc919_class },
};
static void register_types(void) {
    type_register_static(&info);
    type_register_static_array(variants, ARRAY_SIZE(variants));
}
type_init(register_types)
