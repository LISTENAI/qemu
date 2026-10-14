/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_VENUSA_AUDIO_H
#define HW_VENUSA_AUDIO_H
#include "system/memory.h"
#include "qemu/timer.h"
typedef struct VenusASoC VenusASoC;
typedef struct VenusAAudio VenusAAudio;
typedef struct VenusAAudioClock {
    QEMUTimer *event;
    VenusAAudio *audio;
    unsigned index;
    uint64_t period, phase;
    int64_t epoch;
    bool enabled;
} VenusAAudioClock;
struct VenusAAudio {
    VenusASoC *soc;
    MemoryRegion apc_io, codec_io;
    uint32_t apc[0x138 / 4], codec[0x64 / 4];
    uint32_t fifo[12][16], half[12], pending[2][6], previous[12];
    uint8_t head[12], count[12];
    bool half_valid[12], right_next[6], valid_previous[3];
    uint16_t bck_div[2];
    bool serial_started[2];
    VenusAAudioClock clock[4];
    uint64_t frames[4], underruns[3], overruns[3];
    int32_t dac_last;
};
void venusa_audio_init(VenusASoC *s);
void venusa_audio_reset(VenusASoC *s);
void venusa_audio_clock(VenusASoC *s);
void venusa_audio_dma_update(VenusASoC *s);
void venusa_i2s_reset(VenusASoC *s, unsigned index);
void venusa_codec_reset(VenusASoC *s);
#endif
