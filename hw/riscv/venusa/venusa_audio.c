/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA APC FIFO, serial audio clocks and codec digital sample paths. */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/riscv/venusa/venusa.h"
#include "system/address-spaces.h"

#define APC_BASE 0x44700000
#define CODEC_BASE 0x44800000
#define AUDIO_HZ UINT64_C(24000000)

/* FIFO order: TX0 L/R, TX1 L/R, TX2 L/R, RX0 L/R, RX1 L/R, RX2 L/R.
 * TX0 R does not exist; its stereo word contains two successive DAC samples. */
static unsigned mode(VenusAAudio *a, unsigned ch)
{
    return (a->apc[3 + ch / 2] >> (1 + 16 * (ch & 1))) & 3;
}
static bool enabled(VenusAAudio *a, unsigned ch)
{
    return ch != 1 && (a->apc[0] & 1) &&
           (a->apc[3 + ch / 2] & (1u << (16 * (ch & 1))));
}
static bool stereo(VenusAAudio *a, unsigned ch)
{
    return ch >= 2 && (a->apc[3 + ch / 2] & (1u << 25));
}
static void event(VenusAAudio *a, unsigned ch, uint32_t bits)
{
    if ((ch & 1) && stereo(a, ch)) { ch--; }
    for (unsigned core = 0; core < 2; core++) {
        a->pending[core][ch / 2] |= bits << (5 * (ch & 1));
    }
}
static unsigned threshold(VenusAAudio *a, unsigned ch)
{
    static const unsigned levels[] = { 1, 4, 8, 16 };
    return levels[(a->apc[3 + ch / 2] >> 26) & 3];
}
static bool dma_ready(VenusAAudio *a, unsigned ch)
{
    if (!enabled(a, ch)) { return false; }
    unsigned n = ch < 6 ? 16 - a->count[ch] : a->count[ch];
    if (stereo(a, ch)) {
        if (ch & 1) { return false; }
        if (!enabled(a, ch + 1)) { return false; }
        n = ch < 6 ? MIN(16 - a->count[ch], 16 - a->count[ch + 1]) :
                     MIN(a->count[ch], a->count[ch + 1]);
        if (mode(a, ch)) {
            /* A wide stereo frame occupies two bus words. Once its left
             * word transfers, the right word remains eligible on its own. */
            n = n * 2 + a->right_next[ch / 2];
        }
    }
    return n >= threshold(a, ch);
}
void venusa_audio_dma_update(VenusASoC *s)
{
    static const unsigned channels[] = { 0, 2, 4, 6, 7, 8, 10 };
    static const unsigned requests[] = { 8, 9, 10, 12, 13, 14, 15 };
    VenusAAudio *a = &s->audio;
    for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
        bool selected = s->syscfg[0xa0 / 4] & (1u << requests[i]);
        if (selected) {
            listenai_dma_request(&s->dma, requests[i],
                (s->syscfg[0x34 / 4] & (1u << 10)) && dma_ready(a, channels[i]));
        }
    }
}
static void signals(VenusAAudio *a)
{
    for (unsigned ch = 0; ch < 12; ch++) {
        unsigned n = a->count[ch];
        uint32_t conditions = enabled(a, ch) &&
                              (!(ch & 1) || !stereo(a, ch)) ? (!n ? 1 : 0) |
                              (n == 16 ? 2 : 0) |
                              (dma_ready(a, ch) ? 16 : 0) : 0;
        event(a, ch, conditions & ~a->previous[ch]);
        a->previous[ch] = conditions;
    }
    for (unsigned group = 0; group < 3; group++) {
        unsigned ch = group * 2;
        bool valid = enabled(a, ch) && a->count[ch];
        if (stereo(a, ch)) { valid &= enabled(a, ch + 1) && a->count[ch + 1]; }
        if (valid && !a->valid_previous[group]) {
            for (unsigned core = 0; core < 2; core++) {
                a->pending[core][group] |= 1u << 10;
            }
        }
        a->valid_previous[group] = valid;
    }
    for (unsigned core = 0; core < 2; core++) {
        uint32_t pending = 0;
        for (unsigned group = 0; group < 6; group++) {
            pending |= a->pending[core][group] &
                       ~a->apc[(core ? 0xc0 : 0x60) / 4 + group];
        }
        venusa_irq(a->soc, 40 + core, pending != 0);
    }
    venusa_audio_dma_update(a->soc);
}
static void clear_fifo(VenusAAudio *a, unsigned ch)
{
    a->head[ch] = a->count[ch] = 0;
    a->half_valid[ch] = false;
    a->right_next[ch / 2] = false;
}
static void push(VenusAAudio *a, unsigned ch, uint32_t word)
{
    if (a->count[ch] == 16) { event(a, ch, 8); }
    else { a->fifo[ch][(a->head[ch] + a->count[ch]++) & 15] = word; }
}
static uint32_t pop(VenusAAudio *a, unsigned ch)
{
    if (!a->count[ch]) { event(a, ch, 4); return 0; }
    uint32_t word = a->fifo[ch][a->head[ch]];
    a->head[ch] = (a->head[ch] + 1) & 15;
    a->count[ch]--;
    return word;
}
static bool tx_sample(VenusAAudio *a, unsigned ch, int32_t *sample)
{
    *sample = 0;
    if (!enabled(a, ch)) { return false; }
    if (!a->count[ch]) {
        event(a, ch, 4);
        a->underruns[ch / 2]++;
        return (a->apc[3 + ch / 2] & (1u << 15)) != 0;
    }
    unsigned format = mode(a, ch);
    uint32_t word = a->fifo[ch][a->head[ch]];
    if (!format && !stereo(a, ch)) {
        *sample = (int16_t)(word >> (a->half_valid[ch] ? 16 : 0));
        if (a->half_valid[ch]) { pop(a, ch); }
        a->half_valid[ch] = !a->half_valid[ch];
    } else {
        pop(a, ch);
        *sample = !format ? (int16_t)word : format == 1 ?
                  (int32_t)(word << 8) >> 8 : format == 3 ?
                  (int32_t)word >> 8 : (int32_t)word;
    }
    return true;
}
static void rx_sample(VenusAAudio *a, unsigned ch, int32_t sample)
{
    if (!enabled(a, ch)) { return; }
    unsigned format = mode(a, ch);
    if (!format && !stereo(a, ch)) {
        if (!a->half_valid[ch]) {
            a->half[ch] = sample & 65535;
            a->half_valid[ch] = true;
            return;
        }
        push(a, ch, a->half[ch] | ((uint32_t)sample << 16));
        a->half_valid[ch] = false;
    } else {
        push(a, ch, !format ? sample & 65535 : format == 1 ?
             sample & 0xffffff : format == 3 ? (uint32_t)sample << 8 : sample);
    }
}
static void arm(VenusAAudioClock *c)
{
    timer_del(c->event);
    if (c->enabled && c->period) {
        timer_mod(c->event, c->epoch +
                  (c->period - c->phase + AUDIO_HZ - 1) / AUDIO_HZ);
    }
}
static void configure(VenusAAudioClock *c, uint64_t period, bool enable)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (c->enabled && c->period) {
        c->phase = (c->phase + (now - c->epoch) * AUDIO_HZ) % c->period;
    }
    if (period != c->period) { c->phase = 0; }
    c->epoch = now;
    c->period = period;
    c->enabled = enable;
    arm(c);
}
static unsigned sample_bits(VenusAAudio *a, unsigned ch)
{
    unsigned format = mode(a, ch);
    return !format ? 16 : format == 2 ? 32 : 24;
}

static uint32_t serial_load(void *opaque, unsigned channel)
{
    VenusAI2SPort *serial = opaque;
    VenusAAudio *a = serial->audio;
    unsigned ch = 2 + 2 * serial->port;
    if (!channel) {
        /*
         * The parallel FIFO presents the left word without advancing it.
         * Both channel pointers advance when the right word starts.
         */
        if (!enabled(a, ch) || !a->count[ch]) {
            return 0;
        }
        uint32_t word = a->fifo[ch][a->head[ch]];
        unsigned format = mode(a, ch);
        if (!format && !stereo(a, ch)) {
            return (int16_t)(word >> (a->half_valid[ch] ? 16 : 0));
        }
        return !format ? (int16_t)word : format == 1 ?
            (int32_t)(word << 8) >> 8 : format == 3 ? (int32_t)word >> 8 : word;
    }
    serial->frame_valid[0] = tx_sample(a, ch, &serial->frame[0]);
    serial->frame_valid[1] = tx_sample(a, ch + 1, &serial->frame[1]);
    signals(a);
    return serial->frame[1];
}
static void serial_received(void *opaque, unsigned channel, int32_t sample)
{
    VenusAI2SPort *serial = opaque;
    VenusAAudio *a = serial->audio;
    if (!channel) {
        serial->received_left = sample;
        serial->left_valid = true;
    } else if (serial->left_valid) {
        serial->left_valid = false;
        rx_sample(a, 8 + 2 * serial->port, serial->received_left);
        rx_sample(a, 9 + 2 * serial->port, sample);
        a->frames[serial->port + 1]++;
        signals(a);
    }
}
static void serial_output(void *opaque, unsigned signal, bool level)
{
    VenusAI2SPort *serial = opaque;
    static const unsigned pads[] = {4, 5, 7};
    if (!serial->port) {
        listenai_pinmux_output(&serial->audio->soc->pinmux, pads[signal], 9,
                               level);
    }
}
static void serial_release(void *opaque, unsigned signal)
{
    VenusAI2SPort *serial = opaque;
    static const unsigned pads[] = { 4, 5, 7 };
    if (!serial->port) {
        listenai_pinmux_release_output(&serial->audio->soc->pinmux,
                                      pads[signal], 9);
    }
}

static void serial_updated(void *opaque)
{
    signals(((VenusAI2SPort *)opaque)->audio);
}
static void serial_loopback(void *opaque, uint32_t cfg)
{
    VenusAI2SPort *serial = opaque;
    VenusAAudio *a = serial->audio;
    unsigned rx = 8 + 2 * serial->port;
    rx_sample(a, rx, listenai_i2s_loop_sample(cfg, serial->frame[0]));
    rx_sample(a, rx + 1, listenai_i2s_loop_sample(cfg, serial->frame[1]));
    if (serial->port == 1 && (a->apc[7] >> 28) == 2) {
        if (serial->frame_valid[0]) {
            rx_sample(a, 8, serial->frame[0]);
        }
        if (serial->frame_valid[1]) {
            rx_sample(a, 9, serial->frame[1]);
        }
    }
    a->frames[serial->port + 1]++;
}
void venusa_i2s_pins_changed(VenusASoC *soc)
{
    VenusAI2SPort *port = &soc->audio.serial[0];
    ListenaiI2S *s = &port->engine;
    if (!s->timer) {
        return;
    }
    uint8_t mask = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (listenai_pinmux_function(&soc->pinmux, 4 + i, 9)) {
            mask |= 1u << i;
        }
    }
    if ((port->mux_mask & ~mask) && s->enabled &&
        (s->tx.known || s->rx.known)) {
        unsigned pad = 4 + ctz32(port->mux_mask & ~mask);
        venusa_fail(soc, 0x44100000 + pad * 4, 4, true,
                    listenai_pinmux_config(&soc->pinmux, pad));
    }
    port->mux_mask = mask;
    s->input_enabled = mask & 4;
}
void venusa_i2s_pad_input(VenusASoC *s, unsigned pad, bool level)
{
    ListenaiI2S *serial = &s->audio.serial[0].engine;
    if (pad == 6) {
        listenai_i2s_input(serial, LISTENAI_I2S_DATA, level);
    }
    if (!listenai_pinmux_function(&s->pinmux, 4, 9) ||
        !listenai_pinmux_function(&s->pinmux, 5, 9)) {
        return;
    }
    if (pad == 5) {
        listenai_i2s_input(serial, LISTENAI_I2S_WS, level);
    } else if (pad == 4) {
        listenai_i2s_input(serial, LISTENAI_I2S_BCLK, level);
    }
}

static int32_t saturate24(double value)
{
    return value >= 8388607 ? 8388607 : value <= -8388608 ? -8388608 :
           (int32_t)llround(value);
}

static int32_t adc_convert(VenusAAudio *a, unsigned channel, int16_t sample)
{
    unsigned bit = channel ? 0 : 1;
    uint32_t analog = a->codec[0x1c / 4], control = a->codec[2];
    unsigned digital = (control >> (channel ? 0 : 7)) & 127;
    unsigned pga = (control >> (channel ? 16 : 21)) & 31;
    if ((analog & ((1u << bit) | (1u << (bit + 2)) | 16)) != (1u << bit) ||
        digital < 2) { return 0; }
    double value = sample * 256.0 * pow(10.0, ((int)pga * 2 - 12 +
                                              (int)digital - 85) / 20.0);
    /* Functional first-order high-pass response at the documented cutoffs.
     * Analog modulator noise and implementation-specific filter rounding
     * are outside the normalized PCM scene-input model. */
    static const unsigned cutoff[] = { 122, 153, 156, 245, 306, 392, 490, 612 };
    for (unsigned stage = 0; stage < 2; stage++) {
        if (control & (1u << (15 - stage))) {
            double ratio = stage ? cutoff[(control >> 26) & 7] / 48000.0 :
                                   3.7 / 48000.0;
            double pole = exp(-2.0 * M_PI * ratio);
            double next = pole * (a->highpass_y[channel][stage] + value -
                                  a->highpass_x[channel][stage]);
            a->highpass_x[channel][stage] = value;
            a->highpass_y[channel][stage] = next;
            value = next;
        }
        if (!stage && (control & (1u << 30))) { break; }
    }
    return saturate24(value);
}

static void tick(void *opaque)
{
    VenusAAudioClock *c = opaque;
    VenusAAudio *a = c->audio;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->phase += (now - c->epoch) * AUDIO_HZ;
    c->phase -= c->period;
    c->epoch = now;
    if (c->index == 2) {
        /* The ideal analog comparator settles at capacitor code 21. Each
         * search step consumes 128 codec clocks; the final step latches
         * completion. Clock gating preserves the partially elapsed step. */
        a->calibration_step++;
        if (a->calibration_step == 22) {
            a->codec[0x48 / 4] = (21u << 3) | 2;
            a->calibrating = false;
            configure(c, c->period, false);
        } else {
            a->codec[0x48 / 4] = (a->calibration_step << 3) |
                                      (a->calibration_step == 21);
            arm(c);
        }
        return;
    }
    a->frames[c->index ? 3 : 0]++;
    if (c->index == 0) {
        int32_t sample;
        bool valid = tx_sample(a, 0, &sample);
        unsigned bits = sample_bits(a, 0);
        int64_t normalized = (int64_t)sample * (INT64_C(1) << (32 - bits));
        a->dac_last = normalized >> 8;
        if ((a->apc[7] >> 28) == 1) {
            rx_sample(a, 8, normalized >> (32 - sample_bits(a, 8)));
        }
        if (a->output && enabled(a, 0)) {
            bool mute = (a->codec[0x34 / 4] & 0x40) ||
                        !(a->codec[0x44 / 4] & 1);
            double gain = pow(10.0, ((int)(a->codec[0x30 / 4] & 255) - 225) / 20.0);
            double pcm = mute ? 0 : (normalized / 65536.0) * gain;
            int16_t value = pcm >= 32767 ? 32767 : pcm <= -32768 ? -32768 : llround(pcm);
            a->output(a->pcm_opaque, AUDIO_HZ * UINT64_C(1000000000) / c->period,
                      value);
        }
        (void)valid;
    } else {
        unsigned rate = AUDIO_HZ * UINT64_C(1000000000) / c->period;
        int16_t input[2] = { 0, 0 };
        if (a->input) { a->input(a->pcm_opaque, rate, input); }
        for (unsigned ch = 0; ch < 2; ch++) {
            int32_t value = ch && (a->codec[2] & (1u << 29)) ?
                (int32_t)(a->codec[0x50 / 4] << 8) >> 8 :
                adc_convert(a, ch, input[ch]);
            a->codec[0x50 / 4 + ch] = value & 0xffffff;
            int64_t normalized = (int64_t)value * 256;
            rx_sample(a, 6 + ch, normalized >> (32 - sample_bits(a, 6 + ch)));
        }
    }
    signals(a);
    arm(c);
}
void venusa_audio_clock(VenusASoC *s)
{
    VenusAAudio *a = &s->audio;
    uint32_t clocks = s->syscfg[0x34 / 4];
    bool common = (clocks & (1u << 10)) && (a->apc[0] & 1);
    if ((clocks & (1u << 9)) && (clocks & 0x3000)) {
        venusa_fail(s, 0x43100034, 4, true, clocks);
    }
    configure(&a->clock[2], UINT64_C(128000000000),
              a->calibrating && (clocks & (1u << 9)));
    static const unsigned rates[] = { 8000, 0, 0, 16000, 0, 24000, 32000,
                                      0, 48000, 96000 };
    unsigned sr = a->codec[0x2c / 4] & 15;
    bool dac = common && (clocks & (1u << 9)) &&
               (a->codec[0x2c / 4] & 0x60) == 0x60;
    if (dac && (sr >= ARRAY_SIZE(rates) || !rates[sr])) {
        venusa_fail(s, CODEC_BASE + 0x2c, 4, true, a->codec[0x2c / 4]);
    }
    configure(&a->clock[0], sr < ARRAY_SIZE(rates) && rates[sr] ?
              UINT64_C(1000000000) * (AUDIO_HZ / rates[sr]) : 0, dac);
    unsigned ar = a->codec[1] & 15;
    bool adc = common && (clocks & (1u << 9)) && (a->codec[0] & 3) == 3 &&
               (a->codec[1] & 0x180) == 0x180 && !(a->codec[7] & 16);
    if (adc && ar != 0 && ar != 3 && ar != 8) {
        venusa_fail(s, CODEC_BASE + 4, 4, true, a->codec[1]);
    }
    configure(&a->clock[1],
              ar < ARRAY_SIZE(rates) && rates[ar]
                  ? UINT64_C(1000000000) * (AUDIO_HZ / rates[ar])
                  : 0,
              adc);
    for (unsigned p = 0; p < 2; p++) {
        uint32_t cfg = a->apc[(0x24 + 8 * p) / 4];
        unsigned tx = 2 + 2 * p;
        bool enable = common && (clocks & (1u << (15 - p)));
        if (enable && (cfg & (1u << 31)) &&
            (s->syscfg[0x30 / 4] & (1u << (29 - p)))) {
            venusa_fail(s, 0x43100030, 4, true, s->syscfg[0x30 / 4]);
        }
        if (!p) {
            a->serial[p].engine.input_clock = !!(s->gpio[0].inputs & (1u << 4));
            a->serial[p].engine.input_ws = !!(s->gpio[0].inputs & (1u << 5));
        }
        if (!listenai_i2s_clock(
                &a->serial[p].engine, AUDIO_HZ, enable,
                (enabled(a, tx) && a->count[tx]) ||
                    (enabled(a, tx + 1) && a->count[tx + 1]),
                !p && listenai_pinmux_function(&s->pinmux, 6, 9))) {
            venusa_fail(s, 0x43100034, 4, true, clocks);
        }
    }
    signals(a);
}
static uint64_t apc_read(void *opaque, hwaddr off, unsigned size)
{
    VenusAAudio *a = opaque;
    unsigned shift = (off & 2) * 8;
    if (size == 2 && off >= 0x48 && off <= 0x5e && !(off & 1)) { off &= ~3u; }
    else if (size != 4 || (off & 3) || off >= sizeof(a->apc)) { goto fail; }
    if (off >= 0x0c && off <= 0x20) {
        unsigned ch = (off - 0x0c) / 2;
        return a->apc[off / 4] | (a->count[ch] << 4) |
               (a->count[ch + 1] << 20);
    }
    if (off >= 0x48 && off <= 0x5c) {
        unsigned ch = 6 + (off - 0x48) / 4;
        uint32_t word;
        if (!(ch & 1) && stereo(a, ch)) {
            if (!mode(a, ch)) {
                word = (pop(a, ch) & 65535) | (pop(a, ch + 1) << 16);
            } else {
                word = pop(a, ch + a->right_next[ch / 2]);
                a->right_next[ch / 2] = !a->right_next[ch / 2];
            }
        } else { word = pop(a, ch); }
        signals(a);
        return word >> shift;
    }
    if (off >= 0x34 && off <= 0x44) { goto fail; }
    if (off >= 0x60 && off < 0x120) {
        unsigned core = (off - 0x60) / 0x60;
        unsigned which = ((off - 0x60) % 0x60) / 0x18;
        unsigned group = ((off - 0x60) % 0x18) / 4;
        if (which == 1) { return 0; }
        if (which == 2) { return a->pending[core][group]; }
        if (which == 3) {
            return a->pending[core][group] &
                   ~a->apc[(core ? 0xc0 : 0x60) / 4 + group];
        }
    }
    return a->apc[off / 4];
fail:
    venusa_fail(a->soc, APC_BASE + off, size, false, 0);
}
static void apc_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusAAudio *a = opaque;
    if (size == 2 && off >= 0x34 && off <= 0x46 && !(off & 1)) {
        value <<= (off & 2) * 8;
        off &= ~3u;
    } else if (size != 4 || (off & 3) || off >= sizeof(a->apc)) { goto fail; }
    if (off >= 0x34 && off <= 0x44) {
        unsigned ch = off == 0x34 ? 0 : 2 + (off - 0x38) / 4;
        if (enabled(a, ch)) {
            if (!(ch & 1) && stereo(a, ch)) {
                if (!mode(a, ch)) {
                    push(a, ch, value & 65535);
                    push(a, ch + 1, value >> 16);
                } else {
                    push(a, ch + a->right_next[ch / 2], value);
                    a->right_next[ch / 2] = !a->right_next[ch / 2];
                }
            } else { push(a, ch, value); }
        }
    } else if (off >= 0x48 && off <= 0x5c) { goto fail; }
    else if (off >= 0x60 && off < 0x120) {
        unsigned core = (off - 0x60) / 0x60;
        unsigned which = ((off - 0x60) % 0x60) / 0x18;
        unsigned group = ((off - 0x60) % 0x18) / 4;
        if (which == 0) { a->apc[off / 4] = value; }
        else if (which == 1) { a->pending[core][group] &= ~value; }
    } else if (off >= 0x0c && off <= 0x20) {
        unsigned ch = (off - 0x0c) / 2;
        if (value & 8) { clear_fifo(a, ch); }
        if (value & (1u << 19)) { clear_fifo(a, ch + 1); }
        if ((off == 0x1c && (value & 0x600)) ||
            (off == 0x1c && (value >> 28) == 3)) { goto fail; }
        if ((value & (1u << 25)) && ch != 0 &&
            ((value >> 1) & 3) != ((value >> 17) & 3)) { goto fail; }
        a->apc[off / 4] = value & ~0x01f801f8u;
        if (off == 0x1c) { a->apc[off / 4] &= ~0x1000u; }
    } else if (off == 4 || off == 8) {
        if (value & 1) {
            for (unsigned ch = off == 4 ? 0 : 6; ch < (off == 4 ? 6 : 12); ch++) {
                clear_fifo(a, ch);
            }
        }
    } else if (off == 0x24 || off == 0x2c) {
        unsigned p = (off - 0x24) / 8;
        uint32_t old = a->apc[off / 4];
        if (!listenai_i2s_config(&a->serial[p].engine, value)) {
            goto fail;
        }
        if ((old ^ value) & 0xc0000000) {
            a->serial[p].left_valid = false;
        }
        a->apc[off / 4] = value;
    } else if (off == 0x28 || off == 0x30) {
        unsigned p = (off - 0x28) / 8;
        if (!listenai_i2s_divisor(&a->serial[p].engine, value)) {
            goto fail;
        }
        a->apc[off / 4] = value & ~(1u << 26);
    } else if (!off) {
        if (value & 12) { goto fail; }
        a->apc[0] = value & 3;
    } else if (off == 0x130 || off == 0x134) { return; }
    else { a->apc[off / 4] = value; }
    venusa_audio_clock(a->soc);
    return;
fail:
    venusa_fail(a->soc, APC_BASE + off, size, true, value);
}
static uint64_t codec_read(void *opaque, hwaddr off, unsigned size)
{
    VenusAAudio *a = opaque;
    if (size != 4 || (off & 3) || off >= sizeof(a->codec)) {
        venusa_fail(a->soc, CODEC_BASE + off, size, false, 0);
    }
    return a->codec[off / 4];
}
static void codec_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusAAudio *a = opaque;
    if (size != 4 || (off & 3) || off >= sizeof(a->codec)) { goto fail; }
    /* Digital microphone serial capture, ALC and noise-shaping filters
     * require separate signal processing models. */
    if ((off == 0x14 && (value & 0x400000)) ||
        (off == 0x10 && (value & 0x30000)) ||
        (off == 0x0c && (value & 0x4000)) ||
        (off == 0x60 && (value & 0x7f))) { goto fail; }
    if (off == 0x48 || off == 0x50 || off == 0x54) { return; }
    if (off == 4 && (value & 0x1000) && !(a->codec[1] & 0x1000) &&
        (a->codec[0] & 3) != 3) {
        goto fail;
    }
    uint32_t old = a->codec[off / 4];
    a->codec[off / 4] = off == 0x5c ? value & 0x3fff :
                        off == 0x58 ? value & 0xffffff : value;
    if (off == 4 && (value & 0x1000) && !(old & 0x1000)) {
        a->calibrating = true;
        a->calibration_step = 0;
        a->codec[0x48 / 4] = 0;
        a->clock[2].phase = 0;
        a->clock[2].epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (off == 4 && !(value & 0x100)) {
        memset(a->highpass_x, 0, sizeof(a->highpass_x));
        memset(a->highpass_y, 0, sizeof(a->highpass_y));
    }
    venusa_audio_clock(a->soc);
    return;
fail:
    venusa_fail(a->soc, CODEC_BASE + off, size, true, value);
}
static const MemoryRegionOps apc_ops = {
    .read = apc_read, .write = apc_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 4 },
};
static const MemoryRegionOps codec_ops = {
    .read = codec_read, .write = codec_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
void venusa_i2s_reset(VenusASoC *s, unsigned index)
{
    VenusAAudio *a = &s->audio;
    a->apc[(0x24 + index * 8) / 4] = 0x00202000;
    a->apc[(0x28 + index * 8) / 4] = 0x00050000;
    listenai_i2s_reset(&a->serial[index].engine);
    listenai_i2s_release_outputs(&a->serial[index].engine);
    a->serial[index].left_valid = false;
    memset(a->serial[index].frame, 0, sizeof(a->serial[index].frame));
}
void venusa_codec_reset(VenusASoC *s)
{
    VenusAAudio *a = &s->audio;
    static const uint32_t reset[] = {
        0, 0x613, 0x0cc62ad5, 0, 0x63182b00, 0x00810146, 0x000188b4,
        0x0001005c, 0x01d81408, 0, 0, 0x208, 0xe1, 0x70, 0xd40, 0,
        0x00300000, 0, 0x80, 0, 0, 0, 0, 0, 0,
    };
    memcpy(a->codec, reset, sizeof(reset));
    configure(&a->clock[0], 0, false);
    configure(&a->clock[1], 0, false);
    configure(&a->clock[2], 0, false);
    a->calibrating = false;
    a->calibration_step = 0;
    a->dac_last = 0;
    memset(a->highpass_x, 0, sizeof(a->highpass_x));
    memset(a->highpass_y, 0, sizeof(a->highpass_y));
}
void venusa_audio_reset(VenusASoC *s)
{
    VenusAAudio *a = &s->audio;
    memset(a->apc, 0, sizeof(a->apc));
    memset(a->pending, 0, sizeof(a->pending));
    memset(a->previous, 0, sizeof(a->previous));
    memset(a->valid_previous, 0, sizeof(a->valid_previous));
    memset(a->frames, 0, sizeof(a->frames));
    memset(a->underruns, 0, sizeof(a->underruns));
    memset(a->overruns, 0, sizeof(a->overruns));
    a->apc[0] = 2;
    a->apc[0x120 / 4] = a->apc[0x124 / 4] = 2400;
    for (unsigned i = 0; i < 6; i++) {
        static const uint32_t masks[] = { 0x41f, 0x5ff, 0x5ff,
                                           0x3ff, 0xdff, 0xdff };
        a->apc[0x60 / 4 + i] = a->apc[0xc0 / 4 + i] = masks[i];
    }
    for (unsigned ch = 0; ch < 12; ch++) { clear_fifo(a, ch); }
    venusa_i2s_reset(s, 0);
    venusa_i2s_reset(s, 1);
    venusa_audio_clock(s);
}
void venusa_audio_init(VenusASoC *s)
{
    VenusAAudio *a = &s->audio;
    a->soc = s;
    for (unsigned p = 0; p < 2; p++) {
        a->serial[p].audio = a;
        a->serial[p].port = p;
        ListenaiI2SCallbacks callbacks = {
            .opaque = &a->serial[p],
            .load = serial_load,
            .received = serial_received,
            .output = serial_output,
            .release = serial_release,
            .loopback = serial_loopback,
            .updated = serial_updated,
        };
        listenai_i2s_init(&a->serial[p].engine, callbacks);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(a->clock); i++) {
        a->clock[i].audio = a;
        a->clock[i].index = i;
        a->clock[i].event = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, &a->clock[i]);
    }
    memory_region_init_io(&a->apc_io, OBJECT(s), &apc_ops, a, "venusa-apc", 0x1000);
    /* APC and DMA share the SoC owner. FIFO request callbacks can service
     * a DMA beat synchronously; the DMA engine guards recursive service. */
    a->apc_io.disable_reentrancy_guard = true;
    memory_region_add_subregion(get_system_memory(), APC_BASE, &a->apc_io);
    memory_region_init_io(&a->codec_io, OBJECT(s), &codec_ops, a, "venusa-codec", 0x1000);
    memory_region_add_subregion(get_system_memory(), CODEC_BASE, &a->codec_io);
    venusa_codec_reset(s);
    venusa_audio_reset(s);
}
