/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/audio/listenai_i2s.h"

static void received(void *opaque, unsigned channel, uint32_t word)
{
    ListenaiI2S *s = opaque;
    static const unsigned widths[] = {16, 20, 24, 32};
    unsigned width = widths[(s->cfg >> 10) & 3];
    s->cb.received(s->cb.opaque, channel,
                   (int32_t)(word << (32 - width)) >> (32 - width));
}

int32_t listenai_i2s_loop_sample(uint32_t cfg, int32_t sample)
{
    static const unsigned widths[] = {16, 20, 24, 32};
    unsigned width = widths[(cfg >> 10) & 3];
    uint32_t mask = UINT32_MAX >> (32 - width);
    uint32_t bits = sample & mask;
    /*
     * The internal loopback samples with the receive delay and clock edge.
     * Transmit pad delay does not affect this internal route.
     */
    int shift = ((cfg >> 17) & 3) - ((cfg & (1u << 13)) ? 0 : 1);
    bits = shift < 0 ? bits >> -shift : (bits << shift) & mask;
    return (int32_t)(bits << (32 - width)) >> (32 - width);
}

static void serial_edge(ListenaiI2S *serial, bool level)
{
    uint32_t cfg = serial->cfg;
    if (serial->clock == level) {
        return;
    }
    serial->clock = level;
    static const unsigned widths[] = {16, 20, 24, 32};
    unsigned width = widths[(cfg >> 10) & 3];
    bool master = cfg & (1u << 21), left_high = !(cfg & (1u << 12));
    bool receive_rising = !!(cfg & (1u << 13)) ^ !!(cfg & (1u << 15));
    bool transmit_rising =
        (master ? !(cfg & (1u << 13)) : !!(cfg & (1u << 13))) ^
        !!(cfg & (1u << 16));
    unsigned delay = master ? !!(cfg & (1u << 20)) : !!(cfg & (1u << 19));
    if (level == transmit_rising) {
        bool bit =
            listenai_i2s_shift(&serial->tx, serial->ws, width, delay, left_high,
                               false, serial->cb.load, NULL, serial->cb.opaque);
        serial->cb.output(serial->cb.opaque, LISTENAI_I2S_DATA, bit);
    }
    if (level == receive_rising && serial->input_seen &&
        serial->input_enabled) {
        listenai_i2s_shift(&serial->rx, serial->ws, width,
                           delay + (master ? (cfg >> 17) & 3 : 0), left_high,
                           serial->data, NULL, received, serial);
    }
}

static void serial_master_tick(ListenaiI2S *serial)
{
    uint32_t cfg = serial->cfg;
    static const unsigned widths[] = {16, 20, 24, 32};
    unsigned width = widths[(cfg >> 10) & 3];
    unsigned slot = width + ((cfg >> 5) & 31);
    bool second_half = serial->edges & 1;
    bool level = second_half ^ !(cfg & (1u << 13));
    /* Forced BCLK primes eight clock cycles before the first left slot. */
    unsigned startup = (cfg & (1u << 27)) ? 8 : 0;
    uint64_t cycle = serial->edges / 2;
    bool ready = cycle >= startup;
    unsigned phase = ready ? (cycle - startup) % (2 * slot) : 0;
    if (cfg & 4) {
        /*
         * Internal loopback crosses the same parallel FIFO boundaries.
         * Its sampling edge precedes the external receiver's half-cycle.
         */
        unsigned delay = !!(cfg & (1u << 20));
        if (!second_half && ready) {
            if (phase == slot + delay) {
                serial->cb.load(serial->cb.opaque, 1);
                serial->loop_pending = true;
            }
            if (serial->loop_pending &&
                phase == (slot + width + delay - 1) % (2 * slot)) {
                serial->loop_pending = false;
                serial->cb.loopback(serial->cb.opaque, cfg);
            }
        }
    } else if (!second_half) {
        bool right = !ready || phase >= slot;
        serial->ws = right ^ !(cfg & (1u << 12));
        serial->cb.output(serial->cb.opaque, LISTENAI_I2S_WS, serial->ws);
        if (ready) {
            serial_edge(serial, level);
        }
        serial->cb.output(serial->cb.opaque, LISTENAI_I2S_BCLK, level);
    } else {
        serial->cb.output(serial->cb.opaque, LISTENAI_I2S_BCLK, level);
        if (ready) {
            serial_edge(serial, level);
        }
    }
    serial->edges++;
}

static void arm(ListenaiI2S *s)
{
    timer_del(s->timer);
    if (s->master_enabled && s->period) {
        timer_mod(s->timer,
                  s->epoch +
                      (s->period - s->phase + s->frequency - 1) / s->frequency);
    }
}

static void tick(void *opaque)
{
    ListenaiI2S *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->phase += (now - s->epoch) * s->frequency;
    s->phase -= s->period;
    s->epoch = now;
    serial_master_tick(s);
    if (s->cb.updated) {
        s->cb.updated(s->cb.opaque);
    }
    arm(s);
}

bool listenai_i2s_clock(ListenaiI2S *s, uint64_t hz, bool enabled,
                        bool tx_ready, bool input_enabled)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool requested = (s->cfg & 0xc0000000) == 0x80000000;
    if (s->enabled && !enabled && requested && !(s->cfg & (1u << 21)) &&
        (s->tx.known || s->rx.known)) {
        return false;
    }
    if (!requested) {
        s->started = false;
    } else if ((s->cfg & (7u << 26)) || tx_ready) {
        s->started = true;
    }
    if (s->master_enabled && s->period) {
        s->phase = (s->phase + (now - s->epoch) * s->frequency) % s->period;
    }
    uint64_t period = UINT64_C(1000000000) * s->divisor;
    if (period != s->period || hz * 2 != s->frequency) {
        s->phase = 0;
    }
    bool master = enabled && requested && s->started && (s->cfg & (1u << 21));
    bool starting = !s->master_enabled && master && !s->edges;
    s->period = period;
    s->frequency = hz * 2;
    s->epoch = now;
    if (!s->enabled && enabled && requested && !(s->cfg & (1u << 21))) {
        s->clock = s->input_clock;
        s->ws = s->input_ws;
    }
    s->enabled = enabled && requested;
    s->input_enabled = input_enabled;
    s->master_enabled = master && hz && s->divisor;
    if (!requested || !(s->cfg & (1u << 21))) {
        s->cb.release(s->cb.opaque, LISTENAI_I2S_BCLK);
        s->cb.release(s->cb.opaque, LISTENAI_I2S_WS);
    }
    if (!requested) {
        s->cb.release(s->cb.opaque, LISTENAI_I2S_DATA);
    }
    if (starting && s->master_enabled) {
        s->cb.output(s->cb.opaque, LISTENAI_I2S_BCLK, s->clock);
        s->phase = s->period;
    }
    arm(s);
    return true;
}

void listenai_i2s_input(ListenaiI2S *s, unsigned signal, bool level)
{
    if (signal == LISTENAI_I2S_DATA) {
        s->data = level;
        s->input_seen = true;
        return;
    }
    if (signal == LISTENAI_I2S_BCLK) {
        s->input_clock = level;
    } else if (signal == LISTENAI_I2S_WS) {
        s->input_ws = level;
    }
    if (!s->enabled || (s->cfg & ((1u << 21) | 4))) {
        return;
    }
    if (signal == LISTENAI_I2S_WS) {
        s->ws = level;
    } else if (signal == LISTENAI_I2S_BCLK) {
        serial_edge(s, level);
    }
}

bool listenai_i2s_config(ListenaiI2S *s, uint32_t cfg)
{
    if (s->enabled && (cfg & 0xc0000000) == 0x80000000 &&
        ((s->cfg ^ cfg) & 0x03ffffff)) {
        return false;
    }
    if (cfg & ((3u << 22) | (3u << 24))) {
        return false;
    }
    if ((cfg & 0xc0000004) == 0x80000000 && (cfg & 0x401b)) {
        return false;
    }
    if ((s->cfg ^ cfg) & 0xc0000000) {
        memset(&s->tx, 0, sizeof(s->tx));
        memset(&s->rx, 0, sizeof(s->rx));
        s->edges = 0;
        s->loop_pending = false;
        s->clock = !!(cfg & (1u << 13));
    }
    s->cfg = cfg;
    return true;
}

bool listenai_i2s_divisor(ListenaiI2S *s, uint32_t value)
{
    if (value & 0xf800ffffu) {
        return false;
    }
    if (value & (1u << 26)) {
        unsigned div = (value >> 16) & 1023;
        if (!div || (s->enabled && s->divisor != div)) {
            return false;
        }
        s->divisor = div;
    }
    return true;
}

void listenai_i2s_reset(ListenaiI2S *s)
{
    ListenaiI2SCallbacks cb = s->cb;
    QEMUTimer *timer = s->timer;
    bool data = s->data, input_seen = s->input_seen;
    bool input_clock = s->input_clock, input_ws = s->input_ws;
    timer_del(timer);
    memset(s, 0, sizeof(*s));
    s->timer = timer;
    s->cb = cb;
    /* Reset the controller, not the externally driven input wire. */
    s->data = data;
    s->input_seen = input_seen;
    s->input_clock = input_clock;
    s->input_ws = input_ws;
    s->cfg = 0x00202000;
    s->divisor = 6;
    s->clock = true;
}

void listenai_i2s_release_outputs(ListenaiI2S *s)
{
    for (unsigned signal = 0; signal <= LISTENAI_I2S_DATA; signal++) {
        s->cb.release(s->cb.opaque, signal);
    }
}

void listenai_i2s_init(ListenaiI2S *s, ListenaiI2SCallbacks cb)
{
    s->cb = cb;
    s->input_clock = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    listenai_i2s_reset(s);
}

void listenai_i2s_destroy(ListenaiI2S *s)
{
    if (s->timer) {
        timer_del(s->timer);
        timer_free(s->timer);
    }
    memset(s, 0, sizeof(*s));
}
