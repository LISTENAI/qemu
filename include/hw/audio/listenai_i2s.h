/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_LISTENAI_I2S_H
#define HW_AUDIO_LISTENAI_I2S_H
#include "hw/audio/listenai_i2s_shift.h"
#include "qemu/timer.h"

/*
 * Shared serial controller. APC/FIFO layout and pad muxing belong to callers.
 */
enum { LISTENAI_I2S_BCLK, LISTENAI_I2S_WS, LISTENAI_I2S_DATA };
typedef struct ListenaiI2SCallbacks {
    void *opaque;
    uint32_t (*load)(void *opaque, unsigned channel);
    void (*received)(void *opaque, unsigned channel, int32_t sample);
    void (*output)(void *opaque, unsigned signal, bool level);
    void (*release)(void *opaque, unsigned signal);
    void (*loopback)(void *opaque, uint32_t cfg);
    void (*updated)(void *opaque);
} ListenaiI2SCallbacks;
typedef struct ListenaiI2S {
    ListenaiI2SCallbacks cb;
    QEMUTimer *timer;
    uint32_t cfg;
    uint16_t divisor;
    bool clock, ws, data, input_seen, input_enabled, enabled, master_enabled;
    bool input_clock, input_ws;
    bool started, loop_pending;
    uint64_t edges, period, phase, frequency;
    int64_t epoch;
    ListenaiI2SShift tx, rx;
} ListenaiI2S;
/*
 * cfg uses the common APC I2S_CFG0 layout. Unsupported framing is rejected;
 * active format or divider changes require disabling/resetting the interface.
 */
void listenai_i2s_init(ListenaiI2S *s, ListenaiI2SCallbacks callbacks);
/*
 * Reset internal framing/timers, retaining input wires and output ownership.
 * For a shared reset, cancel every connected engine/DMA first, then release
 * outputs so the release edges cannot complete another old transfer.
 */
void listenai_i2s_reset(ListenaiI2S *s);
void listenai_i2s_release_outputs(ListenaiI2S *s);
/*
 * Cancel/free the timer and clear state; safe on a zero-initialized helper.
 * Finalization does not invoke board wiring callbacks.
 */
void listenai_i2s_destroy(ListenaiI2S *s);
bool listenai_i2s_config(ListenaiI2S *s, uint32_t cfg);
bool listenai_i2s_divisor(ListenaiI2S *s, uint32_t cfg1);
/*
 * hz is MCLK. tx_ready permits startup unless a force-clock bit is set.
 * Master gating retains phase. Gating a shifting slave independently of its
 * external clock is unsupported and returns false without changing state.
 */
bool listenai_i2s_clock(ListenaiI2S *s, uint64_t hz, bool enabled,
                        bool tx_ready, bool input_enabled);
/* signal is BCLK/WS/DATA; input wire levels are sampled even while disabled. */
void listenai_i2s_input(ListenaiI2S *s, unsigned signal, bool level);
int32_t listenai_i2s_loop_sample(uint32_t cfg, int32_t sample);
#endif
