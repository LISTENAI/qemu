/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA APC FIFO, serial audio clocks and codec digital sample paths. */
#include "qemu/osdep.h"
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
        listenai_dma_request(&s->dma, requests[i], selected &&
            (s->syscfg[0x34 / 4] & (1u << 10)) && dma_ready(a, channels[i]));
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
static int32_t serial_sample(uint32_t cfg, int32_t sample)
{
    static const unsigned widths[] = { 16, 20, 24, 32 };
    unsigned width = widths[(cfg >> 10) & 3];
    uint32_t mask = UINT32_MAX >> (32 - width);
    uint32_t bits = sample & mask;
    /* The internal loopback samples with the receive delay and clock edge.
     * Transmit pad delay does not affect this internal route. */
    int shift = ((cfg >> 17) & 3) - ((cfg & (1u << 13)) ? 0 : 1);
    bits = shift < 0 ? bits >> -shift : (bits << shift) & mask;
    return (int32_t)(bits << (32 - width)) >> (32 - width);
}
static void tick(void *opaque)
{
    VenusAAudioClock *c = opaque;
    VenusAAudio *a = c->audio;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->phase += (now - c->epoch) * AUDIO_HZ;
    c->phase -= c->period;
    c->epoch = now;
    a->frames[c->index]++;
    if (c->index == 0) {
        int32_t sample;
        bool valid = tx_sample(a, 0, &sample);
        a->dac_last = sample;
        if (valid && (a->apc[7] >> 28) == 1) { rx_sample(a, 8, sample); }
    } else if (c->index < 3) {
        unsigned port = c->index - 1, tx = 2 + 2 * port, rx = 8 + 2 * port;
        uint32_t cfg = a->apc[(0x24 + port * 8) / 4];
        int32_t left, right;
        bool l = tx_sample(a, tx, &left), r = tx_sample(a, tx + 1, &right);
        if (cfg & 4) {
            rx_sample(a, rx, serial_sample(cfg, left));
            rx_sample(a, rx + 1, serial_sample(cfg, right));
        }
        if (port == 1 && (a->apc[7] >> 28) == 2) {
            if (l) { rx_sample(a, 8, left); }
            if (r) { rx_sample(a, 9, right); }
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
    for (unsigned p = 0; p < 2; p++) {
        uint32_t cfg = a->apc[(0x24 + 8 * p) / 4];
        unsigned tx = 2 + 2 * p;
        bool requested = (cfg & (1u << 31)) && !(cfg & (1u << 30));
        if (!requested) { a->serial_started[p] = false; }
        else if ((cfg & (7u << 26)) ||
                 (enabled(a, tx) && a->count[tx]) ||
                 (enabled(a, tx + 1) && a->count[tx + 1])) {
            a->serial_started[p] = true;
        }
        bool enable = common && (clocks & (1u << (15 - p))) && requested &&
                      a->serial_started[p] && (cfg & (1u << 21));
        static const unsigned widths[] = { 16, 20, 24, 32 };
        unsigned framebits = 2 * (widths[(cfg >> 10) & 3] + ((cfg >> 5) & 31));
        configure(&a->clock[1 + p], UINT64_C(1000000000) *
                  framebits * a->bck_div[p], enable && a->bck_div[p]);
    }
    signals(a);
}
static uint64_t apc_read(void *opaque, hwaddr off, unsigned size)
{
    VenusAAudio *a = opaque;
    if (size != 4 || (off & 3) || off >= sizeof(a->apc)) { goto fail; }
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
        return word;
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
    if (size != 4 || (off & 3) || off >= sizeof(a->apc)) { goto fail; }
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
        /* TDM and channel duplication need their own serial framing model. */
        if (value & ((3u << 22) | (3u << 24))) { goto fail; }
        a->apc[off / 4] = value;
    } else if (off == 0x28 || off == 0x30) {
        unsigned p = (off - 0x28) / 8;
        if (value & (1u << 26)) {
            unsigned divisor = (value >> 16) & 1023;
            if (!divisor) { goto fail; }
            a->bck_div[p] = divisor;
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
    if (off == 0x58 || off == 0x5c) { return (uint32_t)a->dac_last; }
    return a->codec[off / 4];
}
static void codec_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusAAudio *a = opaque;
    if (size != 4 || (off & 3) || off >= sizeof(a->codec)) { goto fail; }
    /* ADC calibration/DMIC require the analog input frontend. */
    if (off == 4 && (value & 0x1180)) { goto fail; }
    if (off == 0x48 || off == 0x50 || off == 0x54 || off == 0x5c) { return; }
    a->codec[off / 4] = value;
    venusa_audio_clock(a->soc);
    return;
fail:
    venusa_fail(a->soc, CODEC_BASE + off, size, true, value);
}
static const MemoryRegionOps apc_ops = {
    .read = apc_read, .write = apc_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
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
    a->bck_div[index] = 6;
    a->serial_started[index] = false;
    configure(&a->clock[index + 1], 0, false);
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
    configure(&a->clock[3], 0, false);
    a->dac_last = 0;
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
