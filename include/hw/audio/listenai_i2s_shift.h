/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_LISTENAI_I2S_SHIFT_H
#define HW_AUDIO_LISTENAI_I2S_SHIFT_H
#include <stdint.h>
#include <stdbool.h>

typedef struct ListenaiI2SShift {
    uint32_t word;
    unsigned remaining, wait, channel, next_channel;
    bool ws, known, pending;
} ListenaiI2SShift;

/*
 * One serial sampling edge. The WS transition precedes a word by delay bits;
 * a delayed transition may coincide with the previous word's final bit.
 */
static inline bool listenai_i2s_shift(
    ListenaiI2SShift *s, bool ws, unsigned width, unsigned delay,
    bool left_high, bool input,
    uint32_t (*load)(void *opaque, unsigned channel),
    void (*received)(void *opaque, unsigned channel, uint32_t word),
    void *opaque)
{
    if (!s->known || ws != s->ws) {
        s->known = true;
        s->ws = ws;
        s->pending = true;
        s->wait = delay;
        s->next_channel = ws == left_high ? 0 : 1;
    }
    if (s->pending && !s->wait) {
        s->pending = false;
        s->channel = s->next_channel;
        s->remaining = width;
        s->word = load ? load(opaque, s->channel) : 0;
    }
    bool output = false;
    if (s->remaining) {
        output = (s->word >> (s->remaining - 1)) & 1;
        if (!load) {
            s->word = (s->word << 1) | input;
        }
        if (!--s->remaining && received) {
            received(opaque, s->channel, s->word);
        }
    }
    if (s->pending && s->wait) {
        s->wait--;
    }
    return output;
}
#endif
