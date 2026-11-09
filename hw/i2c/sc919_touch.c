/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SC919 v1 host scan protocol, with an ideal differential AFE.
 * The protocol follows the v1 host library; scan period, pulse duration,
 * electrode coupling and offset removal below are MODEL parameters, not
 * measured silicon timing or capacitance. The guest learns its own baseline
 * and performs all touch detection, filtering and coordinate calculation.
 * Active/idle, requested frequency scans and CMD memory are modeled for the
 * 8-RX profile. Frequency scans use an explicitly ideal zero-noise scene.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/i2c/sc919_touch.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"

OBJECT_DECLARE_SIMPLE_TYPE(SC919Touch, SC919_TOUCH)
struct SC919Touch {
    I2CSlave parent_obj;
    uint16_t memory[32768], config[16];
    uint8_t frame[160], read_frame[160], header[4];
    unsigned sent, received, address, word_hi;
    uint32_t period_us, pulse_us, amplitude;
    int32_t baseline, afe_offset, trim;
    uint16_t x, y;
    bool pressed, reset_low, calibrated, read_valid;
    uint64_t frames, skipped, rejected;
    QEMUTimer *scan_timer, *irq_timer;
    qemu_irq irq;
};
#define REG(s, a) ((s)->memory[(a) / 2])

static uint16_t checksum(uint16_t acc, uint16_t word)
{
    uint32_t t = acc + (uint32_t)word;
    return (t << 1) + ((t >> 15) & 1);
}

static uint32_t lfsr(uint32_t x)
{
    return (x >> 1) | (((x << 3) ^ (x << 25)) & 0x02000000);
}

static unsigned nibble(uint32_t x)
{
    return ((x >> 2) & 1) | ((x >> 5) & 2) | ((x >> 6) & 4) | ((x >> 22) & 8);
}

static uint32_t key(uint16_t *h)
{
    uint32_t seed = h[13] | (uint32_t)h[14] << 16;
    unsigned rot = (h[12] >> 1) & 3;
    uint32_t state = ((seed >> rot) | (seed << (26 - rot))) & 0x03ffffff;
    for (unsigned i = 0; i < 26; i++) { state = lfsr(state); }
    uint32_t result = nibble(state);
    for (unsigned shift = 4; shift < 32; shift += 4) {
        for (unsigned i = 0; i < 5; i++) { state = lfsr(state); }
        result |= nibble(state) << shift;
    }
    return result;
}

static void irq_release(void *opaque)
{
    qemu_set_irq(SC919_TOUCH(opaque)->irq, 1);
}

static void schedule(SC919Touch *s)
{
    if (!s->reset_low && (REG(s, 0xf000) & 1)) {
        if (!timer_pending(s->scan_timer)) {
            timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)s->period_us * 1000);
        }
    } else {
        timer_del(s->scan_timer);
        timer_del(s->irq_timer);
        qemu_set_irq(s->irq, 1);
    }
}

static void scan(void *opaque)
{
    SC919Touch *s = opaque;
    if (s->reset_low || !(REG(s, 0xf000) & 1)) { return; }
    /* RAW_DAT_RD holds the frame; release also permits SkipFrame without a
     * RAM read. Exact silicon IRQ clearing is not specified by this model. */
    if ((REG(s, 0xf004) & 0x200) || REG(s, 0xf512)) {
        s->skipped++; schedule(s); return;
    }
    if (((REG(s, 0xf044) >> 8) & 15) != 8 ||
        ((s->config[3] >> 8) & 15) != 6 || (REG(s, 0xf01c) & 0x4000)) {
        error_report("SC919 touch: unsupported scan geometry or SWK mode");
        REG(s, 0xf000) &= ~1; s->rejected++; return;
    }
    uint16_t h[16] = {0}, raw[64];
    unsigned hop = s->config[5], first = (hop >> 4) & 15, last = hop & 15;
    unsigned repeats = (hop >> 8) & 3;
    if (!repeats) { repeats = 4; }
    bool frequency_scan = hop & 0x400;
    if (frequency_scan && (first > last || (last - first + 1) * repeats > 8)) {
        error_report("SC919 touch: frequency scan exceeds raw row capacity");
        REG(s, 0xf000) &= ~1; s->rejected++; return;
    }
    /* An ideal differential AFE removes its static channel offsets exactly
     * at the requested conversion. No guest calibration RAM is modified. */
    if (s->config[3] & 0x1000) {
        s->trim = s->afe_offset;
        s->calibrated = s->afe_offset - s->trim == 0;
    }
    h[1] = ((s->config[2] & 0x10) ? 0x4000 : 0) |
           (s->calibrated ? 0x100 : 0);
    h[3] = (s->config[10] & 3) << 8;
    /* F50A selects a batch of per-frequency noise rows, not AA electrodes.
     * H4.bit9 identifies this completed scan type to the host HOP algorithm.
     * The ideal scene has zero differential noise at every selected frequency;
     * no contact field is mixed into these rows. Firmware selects frequencies. */
    h[4] = frequency_scan ? 0x200 : 0;
    h[12] = s->config[13]; h[13] = s->config[14]; h[14] = s->config[15] & 1023;
    for (unsigned i = 0; i < 64; i++) {
        int value = s->baseline + s->afe_offset - s->trim;
        if (!frequency_scan && s->pressed && i < 48 && s->calibrated) {
            /* Smooth local coupling in logical electrode space. Width is
             * 0.7 electrode pitches; input is a physical scene location. */
            double dx = (i / 8 + 0.5) - (s->x + 0.5) / 40.0;
            double dy = (i % 8 + 0.5) - (s->y + 0.5) / 40.0;
            value += lrint(s->amplitude * exp(-(dx * dx + dy * dy) / 0.98));
        }
        raw[i] = (uint16_t)(int16_t)value;
        h[0] = checksum(h[0], raw[i]);
    }
    for (unsigned i = 1; i < 16; i++) { h[0] = checksum(h[0], h[i]); }
    uint32_t mask = key(h);
    for (unsigned i = 0; i < 16; i++) { stw_be_p(s->frame + i * 2, h[i]); }
    for (unsigned i = 0; i < 64; i += 2) {
        stl_be_p(s->frame + 32 + i * 2, (((uint32_t)raw[i] << 16) | raw[i + 1]) ^ mask);
    }
    s->frames++;
    qemu_set_irq(s->irq, 0);
    timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)s->pulse_us * 1000);
    schedule(s);
}

static bool address_valid(unsigned address)
{
    return (address >= 0xe000 && address < 0xe258) ||
           (address >= 0xf000 && address < 0xf0be) ||
           (address >= 0xf500 && address < 0xf520);
}

static void write_word(SC919Touch *s, unsigned address, uint16_t value)
{
    if (address == 0xf00c) { return; } /* ID is not configuration RAM. */
    uint16_t old = REG(s, address);
    REG(s, address) = value;
    if (address == 0xf512 && old && !value) {
        memcpy(s->config, &REG(s, 0xf500), sizeof(s->config));
        if (s->config[3] & 0x1000) { s->calibrated = false; }
    }
    if (address == 0xf000) { schedule(s); }
}

static int event(I2CSlave *dev, enum i2c_event event)
{
    SC919Touch *s = SC919_TOUCH(dev);
    if (s->reset_low) { return 1; }
    if (event == I2C_START_SEND) { s->sent = 0; s->read_valid = false; }
    if (event == I2C_START_RECV) {
        if (!s->read_valid) { s->rejected++; return 1; }
        s->received = 0;
        memcpy(s->read_frame, s->frame, sizeof(s->frame));
    }
    return 0;
}

static int sc_send(I2CSlave *dev, uint8_t byte)
{
    SC919Touch *s = SC919_TOUCH(dev);
    unsigned n = s->sent++;
    if (n < 4) {
        s->header[n] = byte;
        if (n == 0 && byte != 0 && byte != 0xa2) { goto reject; }
        if (n == 1 && byte != (s->header[0] == 0xa2 ? 0x80 : 0)) { goto reject; }
        if (n == 3) {
            s->address = (s->header[2] << 8) | byte;
            if (s->address & 1) { goto reject; }
            bool frame = s->address >= 0xcfe0 && s->address < 0xd080;
            if (!address_valid(s->address) && !(frame && s->header[0] == 0xa2)) { goto reject; }
            s->read_valid = s->header[0] == 0xa2;
        }
    } else {
        if (s->header[0] || !address_valid(s->address)) { goto reject; }
        if (!(n & 1)) { s->word_hi = byte; }
        else { write_word(s, s->address, (s->word_hi << 8) | byte); s->address += 2; }
    }
    return 0;
reject:
    s->rejected++; s->read_valid = false;
    return 1;
}

static uint8_t sc_recv(I2CSlave *dev)
{
    SC919Touch *s = SC919_TOUCH(dev);
    if (!s->received++) { return 0; } /* Protocol dummy byte. */
    unsigned a = s->address++;
    if (a >= 0xcfe0 && a < 0xd080) { return s->read_frame[a - 0xcfe0]; }
    if (!address_valid(a)) { s->rejected++; return 0xff; }
    return REG(s, a & ~1) >> ((a & 1) ? 0 : 8);
}

static void reset(DeviceState *dev)
{
    SC919Touch *s = SC919_TOUCH(dev);
    timer_del(s->scan_timer); timer_del(s->irq_timer);
    memset(s->memory, 0, sizeof(s->memory)); memset(s->config, 0, sizeof(s->config));
    memset(s->frame, 0, sizeof(s->frame));
    memset(s->read_frame, 0, sizeof(s->read_frame));
    /* Interface revision 1 is specified by the host library system version;
     * this is a v1 model identity, not a measured ID from the target panel. */
    REG(s, 0xf00c) = 0x0120;
    s->sent = s->received = s->address = 0;
    s->trim = 0;
    s->read_valid = s->calibrated = s->pressed = false;
    s->frames = s->skipped = s->rejected = 0;
    qemu_set_irq(s->irq, 1);
}

static void reset_pin(void *opaque, int line, int level)
{
    SC919Touch *s = opaque;
    if (s->reset_low == !level) { return; }
    s->reset_low = !level;
    if (s->reset_low) { reset(DEVICE(s)); }
    schedule(s);
}

static void contact_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    SC919Touch *s = opaque;
    char *value = s->pressed ? g_strdup_printf("%u,%u,press", s->x, s->y) : g_strdup("release");
    visit_type_str(v, name, &value, errp); g_free(value);
}

static void contact_set(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    SC919Touch *s = opaque;
    char *value = NULL, extra, state[8]; unsigned x, y;
    if (!visit_type_str(v, name, &value, errp)) { return; }
    if (!strcmp(value, "release")) { s->pressed = false; }
    else if (sscanf(value, "%u,%u,%7s%c", &x, &y, state, &extra) == 3 &&
             !strcmp(state, "press") && x < 240 && y < 320) {
        s->x = x; s->y = y; s->pressed = true;
    } else { error_setg(errp, "Touch input must be x,y,press or release"); }
    g_free(value);
}

void sc919_touch_add_input(Object *owner, DeviceState *dev, const char *name)
{
    object_property_add(owner, name, "str", contact_get, contact_set, NULL, dev);
}

static char *diagnostic_get(Object *obj, Error **errp)
{
    SC919Touch *s = SC919_TOUCH(obj);
    GString *value = g_string_new(NULL);
    g_string_append_printf(value, "{\"frames\":%" PRIu64 ",\"skipped\":%" PRIu64
        ",\"rejected\":%" PRIu64 ",\"received\":%u,\"address\":%u,\"frame\":\"",
        s->frames, s->skipped, s->rejected, s->received, s->address);
    for (unsigned i = 0; i < sizeof(s->read_frame); i++) {
        g_string_append_printf(value, "%02x", s->read_frame[i]);
    }
    g_string_append(value, "\"}");
    return g_string_free(value, false);
}

static void realize(DeviceState *dev, Error **errp)
{
    SC919Touch *s = SC919_TOUCH(dev);
    if (s->period_us < 5000 || s->period_us > 100000 || !s->pulse_us ||
        s->pulse_us >= s->period_us || s->amplitude > 4096 ||
        s->baseline < -16000 || s->baseline > 16000 ||
        s->afe_offset < -4096 || s->afe_offset > 4096) {
        error_setg(errp, "Invalid SC919 ideal AFE profile");
    }
}

static const Property properties[] = {
    DEFINE_PROP_UINT32("scan-period-us", SC919Touch, period_us, 20000),
    DEFINE_PROP_UINT32("irq-pulse-us", SC919Touch, pulse_us, 100),
    DEFINE_PROP_UINT32("coupling-counts", SC919Touch, amplitude, 600),
    DEFINE_PROP_INT32("baseline-counts", SC919Touch, baseline, 0),
    DEFINE_PROP_INT32("afe-offset-counts", SC919Touch, afe_offset, 64),
};

static void instance_init(Object *obj)
{
    SC919Touch *s = SC919_TOUCH(obj);
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, scan, s);
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, irq_release, s);
    qdev_init_gpio_out_named(DEVICE(s), &s->irq, "int", 1);
    qdev_init_gpio_in_named(DEVICE(s), reset_pin, "reset", 1);
    object_property_add_str(obj, "scan-diagnostic", diagnostic_get, NULL);
}

static void finalize(Object *obj)
{
    SC919Touch *s = SC919_TOUCH(obj);
    timer_free(s->scan_timer); timer_free(s->irq_timer);
}

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->send = sc_send; ic->recv = sc_recv; ic->event = event;
    dc->realize = realize; device_class_set_props(dc, properties);
    device_class_set_legacy_reset(dc, reset);
}

static const TypeInfo info = {
    .name = TYPE_SC919_TOUCH, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(SC919Touch), .instance_init = instance_init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
