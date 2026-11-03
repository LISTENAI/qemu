/* SPDX-License-Identifier: GPL-2.0-or-later */
/* JD79651C digital B/W interface. Register references: Jadard V1.0.1,
 * 2021-04-09, sections 6-9, published by Good Display:
 * https://v4.cecdn.yun300.cn/100001_1909185148/JD79651CA_DS_V1.0.1_20210409.pdf
 * Analog waveforms are not optically simulated. Register/board-supplied LUT
 * frame counts and the documented PLL table inform the completion event.
 * Without factory LUTs, three scan frames estimate refresh completion.
 * These digital deadlines do not model pigment response or physical timing.
 * No factory OTP waveform, revision, calibration or identity is invented.
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/display/jd79651c.h"
#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "trace.h"

#define WIDTH 176
#define HEIGHT 296
#define STRIDE (WIDTH / 8)
#define BYTES (STRIDE * HEIGHT)
#define LUT_BYTES 224
enum { IDLE, POWER_ON, REFRESH, POWER_OFF };

typedef struct JD79651CState {
    SSIPeripheral parent;
    QemuConsole *console;
    QEMUTimer *event;
    qemu_irq busy_n;
    uint8_t regs[256][56], counts[256], otp[5][56];
    uint8_t ram[2][BYTES], valid[2][BYTES];
    uint8_t presented[BYTES], pending[BYTES];
    uint8_t presented_valid[BYTES], pending_valid[BYTES];
    uint32_t width, height, x_offset, y_offset, rotation;
    char *otp_luts;
    bool refresh_timing_estimated;
    bool have_otp, reset_released, data_mode, sleeping, powered, partial;
    bool command_valid, dirty, auto_off, auto_sleep, vendor_unlocked;
    uint8_t command;
    unsigned count, data_count, data_expected, data_plane;
    unsigned x0, y0, x1, y1, state;
    uint64_t pixels_written, refreshes, cancelled, ignored;
    int64_t deadline, presented_ns;
    void (*failure)(void *, const char *);
    void *failure_opaque;
} JD79651CState;
OBJECT_DECLARE_SIMPLE_TYPE(JD79651CState, JD79651C)

static void update(void *opaque);

static G_NORETURN void fail(JD79651CState *s, const char *reason, unsigned value)
{
    error_report("JD79651C %s (command=0x%02x value=0x%x)", reason, s->command, value);
    if (s->failure) { s->failure(s->failure_opaque, "unsupported-panel"); }
    exit(1);
}

void jd79651c_redrive(DeviceState *dev)
{
    JD79651CState *s = JD79651C(dev);
    qemu_set_irq(s->busy_n, s->state == IDLE && s->reset_released);
}

/* Vendor entry permits factory-configured monochrome operation without PSR
 * or TRES writes. The declared panel geometry supplies the functional default;
 * no factory OTP bytes are synthesized or exposed to the guest. */
static bool factory_settings(JD79651CState *s)
{
    return s->vendor_unlocked && !s->counts[0] && !s->counts[0x61];
}

static unsigned resolution(JD79651CState *s, bool vertical)
{
    static const unsigned sizes[4][2] = {{96, 230}, {96, 252}, {128, 296}, {160, 296}};
    if (s->regs[0x61][0]) {
        return vertical ? (s->regs[0x61][1] << 8) | s->regs[0x61][2] : s->regs[0x61][0];
    }
    if (factory_settings(s)) {
        return vertical ? s->height : s->width;
    }
    return sizes[s->regs[0][0] >> 6][vertical];
}

static uint64_t frame_ns(JD79651CState *s)
{
    /* R30H, table on p45; indices are M-1 and N-1. */
    static const uint8_t hz[7][7] = {
        {29,14,10,7,6,5,4}, {57,29,19,14,11,10,8},
        {86,43,29,21,17,14,12}, {114,57,38,29,23,19,16},
        {150,72,48,36,29,24,20}, {171,86,57,43,34,29,24},
        {200,100,67,50,40,33,29},
    };
    unsigned m = (s->regs[0x30][0] >> 3) & 7, n = s->regs[0x30][0] & 7;
    if (!m || !n) { fail(s, "invalid PLL ratio", s->regs[0x30][0]); }
    return DIV_ROUND_UP(1000000000ULL, hz[m - 1][n - 1]);
}

static void schedule(JD79651CState *s, unsigned state, uint64_t ns)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!ns || ns > INT64_MAX - now) { fail(s, "operation deadline overflow", state); }
    s->state = state; s->deadline = now + ns;
    timer_mod(s->event, s->deadline);
    jd79651c_redrive(DEVICE(s));
}

static void power_on(JD79651CState *s)
{
    if ((s->regs[1][0] & 3) != 3) {
        fail(s, "external source/gate power is unavailable", s->regs[1][0]);
    }
    /* Ideal power ramp using documented soft-start periods (R06H) and
     * the >=80 ms power-sequence interval in section 9.1. */
    unsigned ms = 80 + 10 * (1 + (s->regs[6][0] >> 6)) +
                       10 * (1 + (s->regs[6][1] >> 6));
    schedule(s, POWER_ON, (uint64_t)ms * 1000000);
}

static void window(JD79651CState *s, unsigned *x0, unsigned *y0, unsigned *x1, unsigned *y1)
{
    *x0 = s->partial ? s->x0 : 0; *y0 = s->partial ? s->y0 : 0;
    *x1 = s->partial ? s->x1 : resolution(s, false) - 1;
    *y1 = s->partial ? s->y1 : resolution(s, true) - 1;
    if (*x0 > *x1 || *y0 > *y1 || *x1 >= resolution(s, false) ||
        *y1 >= resolution(s, true)) { fail(s, "window outside configured resolution", *x1); }
}

static uint64_t refresh_frames(JD79651CState *s, bool registers, bool drives[5])
{
    s->refresh_timing_estimated = !registers && !s->have_otp;
    if (s->refresh_timing_estimated) {
        for (unsigned i = 0; i < 5; i++) { drives[i] = true; }
        /* Functional completion estimate, not a calibrated panel waveform. */
        return 3;
    }
    unsigned groups = 6 - (s->regs[0x26][0] & 3);
    uint64_t frames = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (registers && s->counts[0x20 + i] < groups * 7) {
            fail(s, "incomplete register waveform", i);
        }
        const uint8_t *lut = registers ? s->regs[0x20 + i] : s->otp[i];
        uint64_t total = 0;
        for (unsigned g = 0; g < groups; g++) {
            const uint8_t *p = lut + g * 7;
            uint64_t a = (p[1] & 63) + (p[2] & 63), b = (p[3] & 63) + (p[4] & 63);
            total += (p[0] + 1ULL) * (a * (p[5] + 1) + b * (p[6] + 1));
            for (unsigned phase = 1; phase <= 4; phase++) {
                drives[i] |= (p[phase] & 63) && ((p[phase] >> 6) == 1 || (p[phase] >> 6) == 2);
            }
        }
        frames = MAX(frames, total);
    }
    if (!frames) { fail(s, "empty refresh waveform", 0); }
    /* Section 9.3/9.5 includes two terminating zero-voltage frames. */
    return frames + 2;
}

static void start_refresh(JD79651CState *s)
{
    bool factory = factory_settings(s);
    bool temperature_override = s->regs[0xe0][0] == 2 && s->regs[0xe5][0] == 0x55;
    if (!s->powered || (!factory && (!(s->regs[0][0] & 0x10) ||
                                     (s->regs[0][0] & 3) != 3))) {
        fail(s, "refresh requires powered monochrome mode and enabled outputs", s->regs[0][0]);
    }
    if (factory && (s->partial || (s->width & 7) || s->x_offset || s->y_offset ||
        (s->counts[0xe0] && !temperature_override) ||
        (!temperature_override && s->regs[0x50][0] != 0x17))) {
        fail(s, "unsupported factory panel configuration", s->regs[0xe0][0]);
    }
    bool drives[5] = {false};
    uint64_t frames = refresh_frames(s, !factory && (s->regs[0][0] & 0x20), drives);
    unsigned x0, y0, x1, y1;
    window(s, &x0, &y0, &x1, &y1);
    memcpy(s->pending, s->presented, BYTES);
    memcpy(s->pending_valid, s->presented_valid, BYTES);
    bool invert = factory || (s->regs[0x50][0] & 0x10);
    /* Temperature override does not select a differential RAM transfer.
     * Factory monochrome defaults present the complete new image. */
    bool new_only = factory || (s->regs[0x50][0] & 0x20);
    for (unsigned y = y0; y <= y1; y++) {
        for (unsigned x = x0; x <= x1; x++) {
            unsigned src = y * STRIDE + x / 8, mask = 0x80 >> (x % 8);
            if (!s->valid[1][src] || (!new_only && !s->valid[0][src])) {
                fail(s, "refresh contains unwritten pixel data", src);
            }
            bool new_bit = !!(s->ram[1][src] & mask), old_bit = !!(s->ram[0][src] & mask);
            bool new_black = new_bit != invert, old_black = old_bit != invert;
            unsigned lut = new_only ? (new_black ? 3 : 2) :
                (new_black ? (old_black ? 4 : 3) : (old_black ? 2 : 1));
            if (!drives[lut]) { continue; }
            unsigned px = !factory && (s->regs[0][0] & 4) ? x : resolution(s, false) - 1 - x;
            unsigned py = factory || (s->regs[0][0] & 8) ? y : resolution(s, true) - 1 - y;
            unsigned dst = py * STRIDE + px / 8, bit = 0x80 >> (px % 8);
            /* DDX[0]=1 maps one to white; DDX[0]=0 maps zero to white. */
            if (new_bit == invert) { s->pending[dst] |= bit; }
            else { s->pending[dst] &= ~bit; }
            s->pending_valid[dst] |= bit;
        }
    }
    schedule(s, REFRESH, frames * frame_ns(s));
}

static void event(void *opaque)
{
    JD79651CState *s = opaque;
    unsigned state = s->state;
    s->state = IDLE; s->deadline = 0;
    if (state == POWER_ON) {
        s->powered = true;
        if (s->auto_off) { start_refresh(s); return; }
    } else if (state == REFRESH) {
        memcpy(s->presented, s->pending, BYTES);
        memcpy(s->presented_valid, s->pending_valid, BYTES);
        s->presented_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->refreshes++; s->dirty = true;
        update(s);
        if (s->auto_off) {
            schedule(s, POWER_OFF, (1 + ((s->regs[3][0] >> 4) & 3)) * frame_ns(s));
            return;
        }
    } else if (state == POWER_OFF) {
        s->powered = false;
        if (s->auto_sleep) { s->sleeping = true; }
        s->auto_off = s->auto_sleep = false;
    }
    jd79651c_redrive(DEVICE(s));
}

static void reset_controller(JD79651CState *s)
{
    if (s->state != IDLE) { s->cancelled++; }
    timer_del(s->event); s->state = IDLE; s->deadline = 0;
    memset(s->regs, 0, sizeof(s->regs)); memset(s->counts, 0, sizeof(s->counts));
    memset(s->ram, 0, sizeof(s->ram)); memset(s->valid, 0, sizeof(s->valid));
    s->regs[0][0] = 0x0f; s->regs[0][1] = 9;
    const uint8_t power[] = {3, 0, 0x26, 0x26, 6};
    memcpy(s->regs[1], power, sizeof(power));
    memset(s->regs[6], 0x17, 3);
    s->regs[0x30][0] = 0x3c; s->regs[0x50][0] = 0xd7; s->regs[0x60][0] = 0x22;
    s->regs[0x80][0] = 0x10; s->regs[0xe1][0] = s->regs[0xe4][0] = 3;
    s->sleeping = s->powered = s->partial = s->auto_off = s->auto_sleep = false;
    s->vendor_unlocked = false; s->refresh_timing_estimated = true;
    s->command_valid = false; s->count = s->data_count = s->data_expected = 0;
    s->x0 = s->y0 = 0; s->x1 = WIDTH - 1; s->y1 = HEIGHT - 1;
    /* Reset cancels uncompleted work and volatile state, not the pigment. */
    jd79651c_redrive(DEVICE(s));
}

static unsigned parameter_limit(unsigned op)
{
    switch (op) {
    case 0x00: case 0x2a: return 2;
    case 0x01: return 5;
    case 0x06: case 0x61: case 0x65: return 3;
    case 0x90: return 7;
    case 0x20: return 56;
    case 0x21: case 0x22: case 0x23: case 0x24: return 42;
    case 0x03: case 0x07: case 0x17: case 0x26: case 0x30: case 0x41:
    case 0x4d: case 0x50: case 0x60: case 0x82: case 0xe0: case 0xe1: case 0xe3:
    case 0xe4: case 0xe5: case 0xf3: return 1;
    default: return 0;
    }
}

static void finish_parameters(JD79651CState *s)
{
    if (!s->command_valid || s->command == 0x10 || s->command == 0x13 || s->command == 0x71) { return; }
    unsigned limit = parameter_limit(s->command);
    if (s->count != limit && !(s->command == 0 && s->count == 1) &&
        !(s->command == 0x20 && s->count == 42)) {
        fail(s, "incomplete command parameters", s->count);
    }
}

static uint32_t transfer(SSIPeripheral *dev, uint32_t data)
{
    JD79651CState *s = JD79651C(dev);
    if (!s->data_mode || !s->command_valid ||
        (s->command != 0x10 && s->command != 0x13) || !s->data_count) {
        trace_jd79651c_spi(data, s->data_mode, s->reset_released,
                           s->command_valid, s->command, s->count);
    }
    if (data > 255) { fail(s, "requires eight-bit SPI words", data); }
    if (!s->reset_released || s->sleeping) { return 0; }
    if (s->state != IDLE && (!s->data_mode ? data != 0x71 :
        (s->command != 0x71 && s->command != 0x11))) {
        s->ignored++; return 0;
    }
    if (!s->data_mode) {
        finish_parameters(s);
        s->command = data; s->count = 0; s->command_valid = true;
        switch (data) {
        case 0x10: case 0x13: {
            unsigned x0, y0, x1, y1;
            window(s, &x0, &y0, &x1, &y1);
            s->data_count = 0; s->data_plane = data == 0x13;
            s->data_expected = ((x1 - x0 + 1) / 8) * (y1 - y0 + 1);
            for (unsigned y = y0; y <= y1; y++) {
                memset(s->valid[s->data_plane] + y * STRIDE + x0 / 8,
                       0, (x1 - x0 + 1) / 8);
            }
            break;
        }
        case 0x02: schedule(s, POWER_OFF, (1 + ((s->regs[3][0] >> 4) & 3)) * frame_ns(s)); break;
        case 0x04: power_on(s); break;
        case 0x11:
            /* R11H and section 9.3: completed data stop starts driving.
             * The readable Data_flag remains valid while BUSY_N is low. */
            if (s->data_expected && s->data_count == s->data_expected) {
                start_refresh(s);
            }
            break;
        case 0x12: start_refresh(s); break;
        case 0x71: break;
        case 0x91: s->partial = true; break;
        case 0x92: s->partial = false; break;
        default:
            if (!parameter_limit(data)) { fail(s, "unimplemented command", data); }
            s->counts[data] = 0;
            break;
        }
        return 0;
    }
    if (!s->command_valid) { fail(s, "data without a command", data); }
    if (s->command == 0x71) {
        return (s->partial ? 0x40 : 0) | 0x10 |
               (s->data_count == s->data_expected && s->data_expected ? 8 : 0) |
               (s->powered ? 4 : 0) | (s->state == POWER_OFF ? 2 : 0) | (s->state == IDLE);
    }
    if (s->command == 0x11) {
        return s->data_count == s->data_expected && s->data_expected ? 0x80 : 0;
    }
    if (s->command == 0x10 || s->command == 0x13) {
        if (s->data_count >= s->data_expected) { fail(s, "excess pixel data", s->data_count); }
        unsigned x0, y0, x1, y1;
        window(s, &x0, &y0, &x1, &y1);
        unsigned row_bytes = (x1 - x0 + 1) / 8;
        unsigned offset = (y0 + s->data_count / row_bytes) * STRIDE + x0 / 8 + s->data_count % row_bytes;
        s->ram[s->data_plane][offset] = data; s->valid[s->data_plane][offset] = true;
        s->data_count++; s->pixels_written += 8;
        return 0;
    }
    unsigned limit = parameter_limit(s->command);
    if (!limit || s->count >= limit) { fail(s, "excess command parameters", data); }
    s->regs[s->command][s->count++] = data;
    s->counts[s->command] = s->count;
    if (s->command == 0 && s->count == 1 && !(data & 1)) {
        reset_controller(s); return 0;
    }
    if (s->count != limit) { return 0; }
    switch (s->command) {
    case 0x07:
        if (data != 0xa5 || s->powered) { fail(s, "invalid deep-sleep entry", data); }
        s->sleeping = true; break;
    case 0x17:
        if (data != 0xa5 && data != 0xa7) { fail(s, "invalid auto sequence", data); }
        s->auto_off = true; s->auto_sleep = data == 0xa7;
        power_on(s); break;
    case 0x30: frame_ns(s); break;
    case 0x41: if (data & 0x80) { fail(s, "external temperature sensor unavailable", data); } break;
    case 0x4d:
        /* FITI vendor-command entry; no OTP contents or analog state change. */
        if (data != 0x55) { fail(s, "invalid vendor command key", data); }
        s->vendor_unlocked = true;
        break;
    case 0xf3:
        /* Retain the fixed FITI analog setting after vendor command entry.
         * The digital model does not calibrate voltage. */
        if (!s->vendor_unlocked || data != 0x0a) {
            fail(s, "unsupported vendor analog setting", data);
        }
        break;
    case 0x61:
        if (!resolution(s, false) || resolution(s, false) > WIDTH ||
            (resolution(s, false) & 7) || !resolution(s, true) || resolution(s, true) > HEIGHT) {
            fail(s, "invalid resolution", data);
        }
        break;
    case 0x65:
        if (s->regs[0x65][0] || s->regs[0x65][1] || data) { fail(s, "nonzero gate/source start unsupported", data); }
        break;
    case 0x90:
        s->x0 = s->regs[0x90][0] & 0xf8; s->x1 = s->regs[0x90][1] | 7;
        s->y0 = (s->regs[0x90][2] << 8) | s->regs[0x90][3];
        s->y1 = (s->regs[0x90][4] << 8) | s->regs[0x90][5];
        if (data & ~1) { fail(s, "invalid partial scan setting", data); }
        { unsigned x0, y0, x1, y1; window(s, &x0, &y0, &x1, &y1); }
        break;
    case 0xe0:
        if (data && !(s->vendor_unlocked && data == 2)) {
            fail(s, "cascade unavailable", data);
        }
        break;
    default: break;
    }
    return 0;
}

static uint32_t rgb(JD79651CState *s, unsigned x, unsigned y)
{
    unsigned px = x, py = y;
    switch (s->rotation) {
    case 90: px = s->width - 1 - y; py = x; break;
    case 180: px = s->width - 1 - x; py = s->height - 1 - y; break;
    case 270: px = y; py = s->height - 1 - x; break;
    }
    px += s->x_offset; py += s->y_offset;
    unsigned offset = py * STRIDE + px / 8, mask = 0x80 >> (px % 8);
    if (!(s->presented_valid[offset] & mask)) { return 0x808080; }
    return s->presented[offset] & mask ? 0xffffff : 0;
}

static void update(void *opaque)
{
    JD79651CState *s = opaque;
    if (!s->dirty) { return; }
    DisplaySurface *surface = qemu_console_surface(s->console);
    for (unsigned y = 0; y < surface_height(surface); y++) {
        uint32_t *row = (uint32_t *)(surface_data(surface) + y * surface_stride(surface));
        for (unsigned x = 0; x < surface_width(surface); x++) { row[x] = rgb(s, x, y); }
    }
    s->dirty = false;
    dpy_gfx_update(s->console, 0, 0, surface_width(surface), surface_height(surface));
}

static void invalidate(void *opaque) { ((JD79651CState *)opaque)->dirty = true; }
static const GraphicHwOps graphics = {.invalidate = invalidate, .gfx_update = update};

static void input(void *opaque, int pin, int level)
{
    JD79651CState *s = opaque;
    trace_jd79651c_gpio(pin, level);
    if (!pin) { s->data_mode = level; return; }
    if (s->reset_released == !!level) { return; }
    s->reset_released = level;
    if (!level) { reset_controller(s); }
    jd79651c_redrive(DEVICE(s));
}

QDict *jd79651c_display_state(DeviceState *dev)
{
    JD79651CState *s = JD79651C(dev);
    QDict *state = qdict_new();
    qdict_put_str(state, "model", TYPE_JD79651C);
    qdict_put_bool(state, "enabled", true); /* Retained pigment needs no backlight. */
    qdict_put_bool(state, "powered", s->powered);
    qdict_put_bool(state, "sleeping", s->sleeping);
    qdict_put_bool(state, "busy", s->state != IDLE);
    qdict_put_bool(state, "otp_waveform_available", s->have_otp);
    qdict_put_str(state, "refresh_timing", s->refresh_timing_estimated ? "estimated" : "lut");
    qdict_put_int(state, "pixels_written", s->pixels_written);
    qdict_put_int(state, "refreshes", s->refreshes);
    qdict_put_int(state, "cancelled_operations", s->cancelled);
    qdict_put_int(state, "ignored_busy_bytes", s->ignored);
    qdict_put_int(state, "presented_virtual_ns", s->presented_ns);
    qdict_put_int(state, "operation_deadline_ns", s->deadline);
    return state;
}

void jd79651c_report(DeviceState *dev, FILE *stream)
{
    JD79651CState *s = JD79651C(dev);
    fprintf(stream, "{\"model\":\"jd79651c\",\"pixels_written\":%" PRIu64
            ",\"refreshes\":%" PRIu64 ",\"busy\":%s}",
            s->pixels_written, s->refreshes, s->state == IDLE ? "false" : "true");
}

void jd79651c_save(DeviceState *dev, const char *path)
{
    JD79651CState *s = JD79651C(dev);
    DisplaySurface *surface = qemu_console_surface(s->console);
    FILE *f = fopen(path, "wb");
    if (!f) { fail(s, "cannot open frame capture", errno); }
    fprintf(f, "P6\n%d %d\n255\n", surface_width(surface), surface_height(surface));
    for (unsigned y = 0; y < surface_height(surface); y++) {
        for (unsigned x = 0; x < surface_width(surface); x++) {
            uint32_t color = rgb(s, x, y);
            uint8_t pixel[3] = {color >> 16, color >> 8, color};
            if (fwrite(pixel, 1, 3, f) != 3) { fail(s, "frame capture failed", errno); }
        }
    }
    if (fclose(f)) { fail(s, "frame capture close failed", errno); }
}

void jd79651c_failure_callback(DeviceState *dev, void (*fn)(void *, const char *), void *opaque)
{
    JD79651CState *s = JD79651C(dev);
    s->failure = fn; s->failure_opaque = opaque;
}

static void reset(DeviceState *dev) { reset_controller(JD79651C(dev)); }

static void realize(SSIPeripheral *dev, Error **errp)
{
    JD79651CState *s = JD79651C(dev);
    if (!s->width || !s->height || s->width > WIDTH || s->height > HEIGHT ||
        s->x_offset > WIDTH - s->width || s->y_offset > HEIGHT - s->height ||
        s->rotation > 270 || s->rotation % 90) {
        error_setg(errp, "Invalid JD79651C viewport or mounting rotation"); return;
    }
    if (s->otp_luts) {
        if (strlen(s->otp_luts) != LUT_BYTES * 2) { error_setg(errp, "Invalid JD79651C OTP LUT size"); return; }
        unsigned at = 0;
        for (unsigned i = 0; i < 5; i++) {
            for (unsigned j = 0; j < (i ? 42 : 56); j++) {
                int hi = g_ascii_xdigit_value(s->otp_luts[at++]);
                int lo = g_ascii_xdigit_value(s->otp_luts[at++]);
                if (hi < 0 || lo < 0) { error_setg(errp, "Invalid JD79651C OTP LUT encoding"); return; }
                s->otp[i][j] = (hi << 4) | lo;
            }
        }
        s->have_otp = true;
    }
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, event, s);
    s->reset_released = true; s->dirty = true;
    qdev_init_gpio_in(DEVICE(s), input, 2);
    qdev_init_gpio_out_named(DEVICE(s), &s->busy_n, "busy-n", 1);
    s->console = graphic_console_init(DEVICE(s), 0, &graphics, s);
    qemu_console_resize(s->console, s->rotation % 180 ? s->height : s->width,
                         s->rotation % 180 ? s->width : s->height);
}

static void unrealize(DeviceState *dev)
{
    JD79651CState *s = JD79651C(dev);
    timer_free(s->event);
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("width", JD79651CState, width, WIDTH),
    DEFINE_PROP_UINT32("height", JD79651CState, height, HEIGHT),
    DEFINE_PROP_UINT32("x-offset", JD79651CState, x_offset, 0),
    DEFINE_PROP_UINT32("y-offset", JD79651CState, y_offset, 0),
    DEFINE_PROP_UINT32("rotation", JD79651CState, rotation, 0),
    DEFINE_PROP_STRING("otp-luts", JD79651CState, otp_luts),
};

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);
    sc->realize = realize; sc->transfer = transfer; sc->cs_polarity = SSI_CS_LOW;
    dc->unrealize = unrealize;
    device_class_set_legacy_reset(dc, reset); device_class_set_props(dc, properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}
static const TypeInfo info = {
    .name = TYPE_JD79651C, .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(JD79651CState), .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
