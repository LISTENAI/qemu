/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_LISA_STREAM_H
#define HW_AUDIO_LISA_STREAM_H
#include "qemu/timer.h"
#include "audio/lisa_stream.h"
#include "qemu/lisa-mapping.h"
typedef struct LisaHostAudio {
    LisaMapping mapping;
    LisaAudioStream *stream;
    QEMUTimer *timer;
    unsigned output_rate, output_index, output_phase;
    uint64_t output_ns;
    int16_t output_history[97];
} LisaHostAudio;
void lisa_host_audio_init(LisaHostAudio *s, const char *path);
void lisa_host_audio_epoch(LisaHostAudio *s, uint64_t ns);
/* Host-only 48 -> 16 kHz FIR; raw Codec samples and clocks are untouched. */
bool lisa_host_audio_output(LisaHostAudio *s, uint64_t ns, unsigned rate,
                            int16_t value, bool enabled);
void lisa_host_audio_output_reset(LisaHostAudio *s);
void lisa_host_audio_finish(LisaHostAudio *s, uint64_t ns);
void lisa_host_audio_report(LisaHostAudio *s, FILE *file);
#endif
