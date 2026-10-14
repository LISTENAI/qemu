/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit digital video fixture. A raw file supplies bytes, never guest RAM. */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-core.h"
#include "system/reset.h"
#include "qemu/cutils.h"
#include "hw/irq.h"
#include "qom/object.h"
#include "qemu/module.h"
#include "qemu/error-report.h"

#define TYPE_LISTENAI_VIDEO_SOURCE "listenai-video-source"
OBJECT_DECLARE_SIMPLE_TYPE(ListenAIVideoSource, LISTENAI_VIDEO_SOURCE)
struct ListenAIVideoSource {
    DeviceState parent_obj;
    char *file, *sink;
    uint32_t width, height, bytes_per_pixel, pixel_hz, frames;
    uint64_t period_ns, delay_ns, frame_start, half_tick, frame_index;
    char *data;
    size_t length, frame_bytes;
    qemu_irq lines[4];
    QEMUTimer *timer;
};
static void step(void *opaque)
{
    ListenAIVideoSource *s = opaque;
    uint64_t cycle = s->half_tick / 2;
    uint64_t line_bytes = (uint64_t)s->width * s->bytes_per_pixel;
    uint64_t cycles = 2 + (line_bytes + 2) * s->height;
    if (cycle >= cycles) {
        qemu_set_irq(s->lines[1], 0); qemu_set_irq(s->lines[2], 0);
        s->frame_index++;
        if (s->frames && s->frame_index >= s->frames) { return; }
        s->frame_start += s->period_ns; s->half_tick = 0;
        timer_mod(s->timer, s->frame_start); return;
    }
    if (!(s->half_tick & 1)) {
        qemu_set_irq(s->lines[0], cycle == 0);
        uint64_t position = cycle >= 2 ? cycle - 2 : 0;
        unsigned y = position / (line_bytes + 2);
        unsigned x = position % (line_bytes + 2);
        bool valid = cycle >= 2 && x > 0 && x <= line_bytes;
        qemu_set_irq(s->lines[1], valid);
        if (valid) {
            size_t frame = s->frame_index % (s->length / s->frame_bytes);
            qemu_set_irq(s->lines[3], (uint8_t)s->data[frame * s->frame_bytes + y * line_bytes + x - 1]);
        }
        qemu_set_irq(s->lines[2], 1);
    } else { qemu_set_irq(s->lines[2], 0); }
    s->half_tick++;
    uint64_t elapsed = ((__uint128_t)s->half_tick * 1000000000 + 2 * s->pixel_hz - 1) / (2 * s->pixel_hz);
    timer_mod(s->timer, s->frame_start + elapsed);
}
static void reset(DeviceState *dev)
{
    ListenAIVideoSource *s = LISTENAI_VIDEO_SOURCE(dev);
    for (unsigned i = 0; i < 4; i++) { qemu_set_irq(s->lines[i], 0); }
    s->frame_index = s->half_tick = 0;
    s->frame_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->delay_ns;
    timer_mod(s->timer, s->frame_start);
}
static void realize(DeviceState *dev, Error **errp)
{
    ListenAIVideoSource *s = LISTENAI_VIDEO_SOURCE(dev);
    if (!s->file || !s->sink || !s->width || !s->height || s->width > 8191 || s->height > 8191 ||
        !s->bytes_per_pixel || s->bytes_per_pixel > 3 || !s->pixel_hz || s->pixel_hz > 100000000 ||
        s->period_ns > INT64_MAX / 2 || s->delay_ns > INT64_MAX / 2) {
        error_setg(errp, "digital video source requires bounded geometry, timing and a raw input file"); return;
    }
    struct stat statbuf;
    if (stat(s->file, &statbuf) || !S_ISREG(statbuf.st_mode) ||
        statbuf.st_size <= 0 || statbuf.st_size > 32 * 1024 * 1024) {
        error_setg(errp, "digital video input must be a regular file up to 32 MiB"); return;
    }
    GError *error = NULL;
    if (!g_file_get_contents(s->file, &s->data, &s->length, &error)) {
        error_setg(errp, "read digital video input: %s", error->message); g_error_free(error); return;
    }
    s->frame_bytes = (size_t)s->width * s->height * s->bytes_per_pixel;
    uint64_t active_ns = DIV_ROUND_UP((2 + ((uint64_t)s->width * s->bytes_per_pixel + 2) * s->height) * 1000000000, s->pixel_hz);
    if (!s->length || s->length > 32 * 1024 * 1024 || s->length % s->frame_bytes || s->period_ns < active_ns) {
        error_setg(errp, "digital video input must contain complete frames and fit the configured frame period"); return;
    }
    Object *sink = object_resolve_path(s->sink, NULL);
    if (!sink || !object_dynamic_cast(sink, TYPE_DEVICE)) {
        error_setg(errp, "digital video sink is not a device: %s", s->sink); return;
    }
    for (unsigned i = 0; i < 4; i++) { s->lines[i] = qdev_get_gpio_in_named(DEVICE(sink), "dvp-input", i); }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, step, s);
    qemu_register_resettable(OBJECT(s));
}
static void unrealize(DeviceState *dev)
{
    ListenAIVideoSource *s = LISTENAI_VIDEO_SOURCE(dev);
    qemu_unregister_resettable(OBJECT(s));
    timer_free(s->timer); s->timer = NULL;
    g_clear_pointer(&s->data, g_free);
}
static const Property properties[] = {
    DEFINE_PROP_STRING("file", ListenAIVideoSource, file),
    DEFINE_PROP_STRING("sink", ListenAIVideoSource, sink),
    DEFINE_PROP_UINT32("width", ListenAIVideoSource, width, 640),
    DEFINE_PROP_UINT32("height", ListenAIVideoSource, height, 480),
    DEFINE_PROP_UINT32("bytes-per-pixel", ListenAIVideoSource, bytes_per_pixel, 2),
    DEFINE_PROP_UINT32("pixel-hz", ListenAIVideoSource, pixel_hz, 24000000),
    DEFINE_PROP_UINT32("frames", ListenAIVideoSource, frames, 0),
    DEFINE_PROP_UINT64("period-ns", ListenAIVideoSource, period_ns, 33333333),
    DEFINE_PROP_UINT64("delay-ns", ListenAIVideoSource, delay_ns, 100000000),
};
static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize; dc->unrealize = unrealize;
    dc->user_creatable = true;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, properties);
    dc->desc = "Explicit raw digital video signal source";
}
static const TypeInfo info = {
    .name = TYPE_LISTENAI_VIDEO_SOURCE, .parent = TYPE_DEVICE,
    .instance_size = sizeof(ListenAIVideoSource), .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
