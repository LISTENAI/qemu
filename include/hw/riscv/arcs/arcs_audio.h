/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_AUDIO_H
#define HW_RISCV_ARCS_AUDIO_H
#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "hw/audio/listenai_i2s.h"
#define ARCS_GPDMA_SERVICE_NS 1000
typedef struct ArcsSoC ArcsSoC;
typedef struct ArcsAudioChannel {
    uint32_t control, source, destination, total, remaining;
    unsigned mode, width, slot, completed_slot;
    bool busy, half_sent, stop_after_block;
} ArcsAudioChannel;
typedef struct ArcsGPDMA {
    ArcsSoC *soc;
    MemoryRegion io;
    QEMUTimer *event;
    int64_t deadline;
    uint32_t regs[0xc0], pending;
    ArcsAudioChannel channel[6];
    bool requests[16], servicing;
    uint64_t bytes, blocks;
} ArcsGPDMA;
typedef struct ArcsAPC ArcsAPC;
typedef struct ArcsI2SPort {
    ArcsAPC *apc;
    unsigned index;
    ListenaiI2S engine;
    uint8_t mux_mask;
    int32_t frame[2], received_left;
    bool left_valid;
} ArcsI2SPort;
struct ArcsAPC {
    ArcsSoC *soc;
    MemoryRegion io;
    uint32_t regs[0x134 / 4], pending[2], previous[8], half[8], fifo[8][16];
    unsigned head[8], count[8];
    bool half_valid[8], right_next[4], clock;
    ArcsI2SPort serial[2];
    uint64_t reads, nonzero;
};
typedef struct ArcsCodec ArcsCodec;
typedef struct ArcsSampleClock {
    ArcsCodec *codec;
    QEMUTimer *event;
    int64_t epoch, deadline;
    uint64_t phase;
    unsigned rate;
    bool adc;
} ArcsSampleClock;
struct ArcsCodec {
    ArcsSoC *soc;
    MemoryRegion io;
    QEMUTimer *calibration;
    ArcsSampleClock adc, dac;
    uint32_t regs[24], calibration_status, clocks;
    bool powered;
    uint64_t adc_frames, dac_samples, underruns;
    void *pcm_opaque;
    void (*input)(void *opaque, unsigned rate, int16_t samples[2]);
    void (*output)(void *opaque, unsigned rate, int sample);
};
void arcs_i2s_pins_changed(ArcsSoC *soc);
void arcs_i2s_pad_input(ArcsSoC *soc, unsigned pad, bool level);
void arcs_i2s_reset(ArcsSoC *soc);
void arcs_i2s_clock(ArcsSoC *soc);
void arcs_audio_init(ArcsSoC *soc);
void arcs_gpdma_reset(ArcsSoC *soc);
void arcs_apc_reset(ArcsSoC *soc);
void arcs_codec_reset(ArcsSoC *soc);
void arcs_codec_clocks(ArcsSoC *soc, uint32_t clocks);
void arcs_codec_power(ArcsSoC *soc, bool powered);
#endif
