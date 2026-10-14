/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RD EVB microphone scene input and speaker transport. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "hw/audio/lisa_stream.h"
#include "hw/loader.h"
#include "qemu/error-report.h"
#include "system/system.h"

typedef struct VenusAPCM {
    VenusASoC *soc;
    LisaHostAudio host;
    Notifier exit;
    uint8_t *input;
    size_t length, position;
    unsigned rate, channels, output_rate;
    GByteArray *output;
    const char *output_path;
} VenusAPCM;

static G_NORETURN void pcm_fail(VenusAPCM *p, const char *message)
{
    error_report("VenusA PCM: %s", message);
    venusa_report(p->soc, "audio-error");
    exit(1);
}

static void input(void *opaque, unsigned rate, int16_t samples[2])
{
    VenusAPCM *p = opaque;
    samples[0] = samples[1] = 0;
    if (p->position == p->length) {
        p->soc->audio.input_missing++;
        return;
    }
    if (rate != p->rate) { pcm_fail(p, "input rate differs from ADC"); }
    for (unsigned i = 0; i < p->channels; i++) {
        samples[i] = lduw_le_p(p->input + p->position);
        p->position += 2;
    }
    p->soc->audio.input_frames++;
}

static bool pa_enabled(VenusAPCM *p)
{
    uint32_t driven, levels;
    listenai_gpio_outputs_snapshot(&p->soc->gpio[0], &driven, &levels);
    return (driven & levels & 1) != 0;
}

static void output(void *opaque, unsigned rate, int16_t sample)
{
    VenusAPCM *p = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (p->output) {
        if (p->output_rate && p->output_rate != rate) {
            pcm_fail(p, "sample rate changed during WAV capture");
        }
        if (p->output->len >= 32000000) {
            pcm_fail(p, "WAV capture capacity reached");
        }
        uint8_t bytes[2];
        stw_le_p(bytes, sample);
        g_byte_array_append(p->output, bytes, sizeof(bytes));
        p->output_rate = rate;
    }
    if (p->host.stream) {
        LisaAudioStream *s = p->host.stream;
        if (rate != LISA_AUDIO_RATE) {
            lisa_audio_store(&s->guest_error, LISA_AUDIO_FORMAT);
            pcm_fail(p, "host speaker requires 16 kHz DAC");
        }
        lisa_host_audio_epoch(&p->host, now);
        if (lisa_audio_load(&s->host_error) || lisa_audio_load(&s->guest_error) ||
            !lisa_audio_dac(s, now, sample, pa_enabled(p))) {
            pcm_fail(p, "host audio transport failed");
        }
    }
    p->soc->audio.output_samples++;
    if (sample) { p->soc->audio.output_nonzero++; }
}

static void finish(Notifier *notifier, void *data)
{
    VenusAPCM *p = container_of(notifier, VenusAPCM, exit);
    lisa_host_audio_finish(&p->host, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (p->output) {
        uint32_t size = p->output->len;
        unsigned rate = p->output_rate ? p->output_rate : 16000;
        uint8_t header[44] = { 0 };
        memcpy(header, "RIFF", 4); stl_le_p(header + 4, size + 36);
        memcpy(header + 8, "WAVEfmt ", 8); stl_le_p(header + 16, 16);
        stw_le_p(header + 20, 1); stw_le_p(header + 22, 1);
        stl_le_p(header + 24, rate); stl_le_p(header + 28, rate * 2);
        stw_le_p(header + 32, 2); stw_le_p(header + 34, 16);
        memcpy(header + 36, "data", 4); stl_le_p(header + 40, size);
        FILE *f = fopen(p->output_path, "wb");
        if (!f || fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
            fwrite(p->output->data, 1, size, f) != size || fclose(f)) {
            pcm_fail(p, "could not save WAV capture");
        }
        g_byte_array_unref(p->output);
    }
    g_free(p->input);
    g_free(p);
}

void venusa_board_audio_connect(VenusASoC *s)
{
    VenusAPCM *p = g_new0(VenusAPCM, 1);
    p->soc = s;
    const char *path = getenv("LISEM_QEMU_AUDIO_INPUT");
    if (path) {
        const char *format = getenv("LISEM_QEMU_AUDIO_FORMAT");
        char extra;
        int size = get_image_size(path);
        if (!format || sscanf(format, "%u,%u%c", &p->rate, &p->channels,
                              &extra) != 2 ||
            !p->rate || p->rate > 192000 || p->channels < 1 || p->channels > 2 ||
            size < 0 || size > 64000000 || size % (2 * p->channels)) {
            pcm_fail(p, "invalid PCM16 input format or size");
        }
        p->length = size;
        p->input = g_malloc(MAX(1, size));
        if (load_image_size(path, p->input, size) != size) {
            pcm_fail(p, "could not load PCM16 input");
        }
    }
    p->output_path = getenv("LISEM_QEMU_AUDIO_OUTPUT");
    if (p->output_path) { p->output = g_byte_array_new(); }
    lisa_host_audio_init(&p->host, getenv("LISEM_QEMU_HOST_AUDIO"));
    if (p->host.stream && lisa_audio_load(&p->host.stream->capture)) {
        pcm_fail(p, "host microphone transport does not supply two microphones");
    }
    s->audio.pcm_opaque = p;
    s->audio.input = input;
    s->audio.output = output;
    p->exit.notify = finish;
    qemu_add_exit_notifier(&p->exit);
}
