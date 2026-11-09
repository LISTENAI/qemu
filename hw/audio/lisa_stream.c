/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Board-side host PCM transport; independent of the SoC Codec and its DMA. */
#include "qemu/osdep.h"
#include "qemu/lisa-mapping.h"
#include "hw/audio/lisa_stream.h"
#include "qemu/error-report.h"
#include "system/runstate.h"
#include "exec/icount.h"

static void advance(void *opaque)
{
    LisaHostAudio *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    lisa_audio_advance(s->stream, now);
    /* Attach only once actual playback has begun. The pacing layer reads
     * the live consumer cursor on every host wait, including idle warps. */
    if (lisa_audio_load(&s->stream->output_read)) {
        icount_clock_audio_cursor(&s->stream->output_cursor_ns);
    }
    timer_mod(s->timer, now + 10000000);
}

static void running(void *opaque, bool enabled, RunState state)
{
    LisaHostAudio *s = opaque;
    if (lisa_audio_load(&s->stream->state) != LISA_AUDIO_DONE) {
        lisa_audio_store(&s->stream->state, enabled ? LISA_AUDIO_RUNNING : LISA_AUDIO_PAUSED);
    }
}

void lisa_host_audio_init(LisaHostAudio *s, const char *path)
{
    if (!path) { return; }
    LisaAudioStream *p;
    if (!strncmp(path, "shm:", 4)) {
        p = lisa_named_mapping(path, sizeof(*p), false, &s->mapping);
    } else {
        int fd = lisa_open_shared_file(path, false);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != sizeof(LisaAudioStream)) {
            error_report("Invalid host audio transport file"); exit(1);
        }
        p = lisa_shared_mapping(fd, sizeof(*p));
        close(fd);
    }
    if (!p || p->magic != LISA_AUDIO_MAGIC || p->bytes != sizeof(*p) ||
        p->rate != LISA_AUDIO_RATE || p->capacity != LISA_AUDIO_CAPACITY ||
        lisa_audio_load(&p->ready) != 1 || lisa_audio_load(&p->state) != LISA_AUDIO_INIT ||
        lisa_audio_load(&p->host_error) || lisa_audio_load(&p->guest_error)) {
        error_report("Host audio transport is not ready or has an incompatible format"); exit(1);
    }
    s->stream = p;
    lisa_audio_store(&p->state, LISA_AUDIO_PAUSED);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, advance, s);
    timer_mod(s->timer, 0);
    qemu_add_vm_change_state_handler(running, s);
}

void lisa_host_audio_epoch(LisaHostAudio *s, uint64_t ns)
{
    LisaAudioStream *p = s->stream;
    if (!p || lisa_audio_load(&p->epoch_ready)) { return; }
    uint64_t written = lisa_audio_load(&p->input_write);
    p->epoch_ns = ns;
    p->input_origin = written > LISA_AUDIO_PREROLL ? written - LISA_AUDIO_PREROLL : 0;
    lisa_audio_store(&p->input_cursor, p->input_origin);
    p->pacing_origin_ns = icount_clock_audio_epoch();
    lisa_audio_store(&p->epoch_ready, 1);
}

/* 97-tap Blackman-windowed sinc, 7 kHz cutoff at 48 kHz, Q2.30.
 * Unity DC gain; causal group delay is 1 ms. This is a host presentation
 * filter, independent of the chip's APC SRC and raw diagnostic captures. */
static const int32_t output_fir[97] = {
    0, -2228, -11136, -10224, 24542, 78829, 84176, -22049,
    -199698, -282403, -102493, 306594, 630833, 474885, -246940, -1067851,
    -1202454, -216082, 1386319, 2287991, 1350686, -1203994, -3523057, -3340393,
    0, 4401911, 6122291, 2763060, -4102428, -9224888, -7448561, 1554677,
    11654553, 14044468, 4446157, -11831185, -22031976, -15252074, 7407375, 30395575,
    33040202, 5844917, -37802131, -64852191, -41536521, 42913271, 163910172, 270675051,
    313170668, 270675051, 163910172, 42913271, -41536521, -64852191, -37802131, 5844917,
    33040202, 30395575, 7407375, -15252074, -22031976, -11831185, 4446157, 14044468,
    11654553, 1554677, -7448561, -9224888, -4102428, 2763060, 6122291, 4401911,
    0, -3340393, -3523057, -1203994, 1350686, 2287991, 1386319, -216082,
    -1202454, -1067851, -246940, 474885, 630833, 306594, -102493, -282403,
    -199698, -22049, 84176, 78829, 24542, -10224, -11136, -2228,
    0
};

void lisa_host_audio_output_reset(LisaHostAudio *s)
{
    s->output_rate = s->output_index = s->output_phase = 0;
    s->output_ns = 0;
    memset(s->output_history, 0, sizeof(s->output_history));
}

bool lisa_host_audio_output(LisaHostAudio *s, uint64_t ns, unsigned rate,
                            int16_t value, bool enabled)
{
    LisaAudioStream *p = s->stream;
    if (!p) { return true; }
    if (rate != LISA_AUDIO_RATE && rate != 48000) {
        lisa_audio_store(&p->guest_error, LISA_AUDIO_FORMAT);
        return false;
    }
    /* A stopped/restarted DAC must not replay the previous filter tail. */
    if (rate != s->output_rate || ns <= s->output_ns ||
        ns - s->output_ns > (1000000000ULL + rate - 1) / rate) {
        lisa_host_audio_output_reset(s);
    }
    s->output_rate = rate;
    s->output_ns = ns;
    if (rate == LISA_AUDIO_RATE) {
        return lisa_audio_dac(p, ns, value, enabled);
    }
    s->output_history[s->output_index] = enabled ? value : 0;
    s->output_index = (s->output_index + 1) % 97;
    if (++s->output_phase < 3) { return true; }
    s->output_phase = 0;
    int64_t sum = 0;
    for (unsigned i = 0; i < 97; i++) {
        sum += (int64_t)output_fir[i] *
            s->output_history[(s->output_index + 96 - i) % 97];
    }
    int64_t rounded = (sum + (1LL << 29)) >> 30;
    return lisa_audio_dac(p, ns, MAX(INT16_MIN, MIN(INT16_MAX, rounded)), enabled);
}

void lisa_host_audio_finish(LisaHostAudio *s, uint64_t ns)
{
    if (!s->stream) { return; }
    timer_del(s->timer);
    lisa_audio_advance(s->stream, ns);
    lisa_audio_store(&s->stream->state, LISA_AUDIO_DONE);
}

void lisa_host_audio_report(LisaHostAudio *s, FILE *file)
{
    LisaAudioStream *p = s->stream;
    if (!p) { return; }
    fprintf(file, ",\"host_audio\":{\"rate\":%u,\"adc_missing\":%" PRIu64
            ",\"input_skipped_frames\":%" PRIu64 ",\"input_dropped_frames\":%" PRIu64
            ",\"input_resyncs\":%" PRIu64
            ",\"host_error\":%" PRIu64 ",\"guest_error\":%" PRIu64 "}",
            LISA_AUDIO_RATE, lisa_audio_load(&p->adc_missing),
            lisa_audio_load(&p->input_skipped), lisa_audio_load(&p->input_dropped),
            lisa_audio_load(&p->input_resyncs),
            lisa_audio_load(&p->host_error), lisa_audio_load(&p->guest_error));
}
