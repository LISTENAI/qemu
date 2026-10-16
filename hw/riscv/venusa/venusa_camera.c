/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional board fixture: GC0328 SCCB, sensor timing and actual DVP pins. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "hw/i2c/gc0328.h"
#include "hw/core/listenai-board.h"
#include "hw/core/listenai-camera-scene.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "system/reset.h"

#define TYPE_VENUSA_CAMERA "venusa-gc0328-camera"
OBJECT_DECLARE_SIMPLE_TYPE(VenusACamera, VENUSA_CAMERA)
struct VenusACamera {
    DeviceState parent_obj;
    char *soc_path, *file;
    VenusASoC *soc;
    DeviceState *sensor;
    ListenaiCameraScene scene;
    QEMUTimer *timer;
    qemu_irq pins[4];
    uint64_t hz_n, hz_d, start, pixel, line, total, phase, frames, samples;
    unsigned width, height, bpp, sync, row, byte, stage;
    bool active;
};
static uint64_t deadline(VenusACamera *s, uint64_t clocks)
{
    __uint128_t n = (__uint128_t)clocks * s->hz_d * 1000000000;
    return s->start + (n + s->hz_n - 1) / s->hz_n;
}
static bool routed(VenusACamera *s)
{
    for (unsigned i = 21; i <= 31; i++) {
        if (!listenai_pinmux_function(&s->soc->pinmux, i, 13)) { return false; }
    }
    return true;
}
static void begin(VenusACamera *s)
{
    Error *error = NULL;
    uint64_t n, d;
    venusa_dvp_xclk_ratio(s->soc, &n, &d);
    if (s->scene.camera && listenai_camera_scene_receive(&s->scene, &error) < 0) {
        error_report_err(error); exit(1);
    }
    gc0328_set_clock(s->sensor, !!n);
    if (!n || !routed(s) || !gc0328_frame_info(s->sensor, &s->width,
            &s->height, &s->bpp, &s->pixel, &s->line, &s->total, &error)) {
        if (error) { error_report_err(error); exit(EXIT_FAILURE); }
        s->active = false; return;
    }
    if (s->scene.camera) { listenai_camera_scene_latch(&s->scene); }
    /* Half-MCLK units preserve the two PCLK edges without rounding drift. */
    s->hz_n = n * 2; s->hz_d = d;
    s->line *= 2; s->total *= 2;
    if (s->line < s->width * s->bpp * s->pixel ||
        s->total < (12 + s->height) * s->line) {
        error_report("unsupported GC0328 digital line/frame timing"); exit(EXIT_FAILURE);
    }
    s->sync = gc0328_sync_mode(s->sensor);
    qemu_set_irq(s->pins[2], !!(s->sync & 4));
    s->stage = s->row = s->byte = s->phase = 0;
    s->frames++;
    s->active = true;
    timer_mod(s->timer, s->start);
}
static void step(void *opaque)
{
    VenusACamera *s = opaque;
    if (!s->active) { return; }
    bool vinactive = !!(s->sync & 2);
    bool hinactive = !!(s->sync & 1);
    bool invert = !!(s->sync & 4);
    if (s->stage == 0) {
        qemu_set_irq(s->pins[0], !vinactive);
        qemu_set_irq(s->pins[1], hinactive);
        qemu_set_irq(s->pins[2], !invert);
        s->stage = 1;
        timer_mod(s->timer, deadline(s, s->pixel / 2)); return;
    }
    if (s->stage == 1) {
        qemu_set_irq(s->pins[2], invert);
        s->stage = 2;
        timer_mod(s->timer, deadline(s, 12 * s->line)); return;
    }
    if (s->stage == 4) {
        s->start = deadline(s, s->total);
        begin(s); return;
    }
    uint64_t line_start = (12 + s->row) * s->line;
    if (s->stage == 2) {
        qemu_set_irq(s->pins[0], vinactive);
        qemu_set_irq(s->pins[1], !hinactive);
        qemu_set_irq(s->pins[3], gc0328_sample(s->sensor, s->byte, s->row));
        s->samples++;
        qemu_set_irq(s->pins[2], !invert);
        s->stage = 3;
        timer_mod(s->timer, deadline(s, line_start + s->byte * s->pixel + s->pixel / 2));
        return;
    }
    if (s->stage == 5) {
        qemu_set_irq(s->pins[1], hinactive);
        qemu_set_irq(s->pins[2], !invert);
        s->stage = 6;
        timer_mod(s->timer, deadline(s, line_start + s->byte * s->pixel + s->pixel / 2));
        return;
    }
    qemu_set_irq(s->pins[2], invert);
    if (s->stage == 6) {
        s->byte = 0;
        if (++s->row == s->height) {
            s->stage = 4;
            timer_mod(s->timer, deadline(s, s->total)); return;
        }
    } else if (++s->byte == s->width * s->bpp) {
        s->stage = 5;
        timer_mod(s->timer, deadline(s, line_start + s->byte * s->pixel));
        return;
    }
    s->stage = 2;
    timer_mod(s->timer, deadline(s, (12 + s->row) * s->line + s->byte * s->pixel));
}
static void clock_changed(void *opaque)
{
    VenusACamera *s = opaque;
    uint64_t n, d;
    venusa_dvp_xclk_ratio(s->soc, &n, &d);
    gc0328_set_clock(s->sensor, !!n);
    if (s->active && routed(s) && n * 2 == s->hz_n && d == s->hz_d) { return; }
    timer_del(s->timer); s->active = false;
    s->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    begin(s);
}
static void notify(void *opaque, bool timing)
{
    VenusACamera *s = opaque;
    if (timing || !s->active) {
        timer_del(s->timer); s->active = false;
        s->start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); begin(s);
    }
}
static void reset(DeviceState *dev)
{
    VenusACamera *s = VENUSA_CAMERA(dev);
    timer_del(s->timer); s->active = false;
    clock_changed(s);
}
static char *timing(Object *object, Error **errp)
{
    VenusACamera *s = VENUSA_CAMERA(object);
    return g_strdup_printf("{\"active\":%s,\"now_ns\":%" PRId64
        ",\"start_ns\":%" PRIu64 ",\"clock_numerator\":%" PRIu64
        ",\"clock_denominator\":%" PRIu64 ",\"byte_clocks\":%" PRIu64
        ",\"line_clocks\":%" PRIu64 ",\"frame_clocks\":%" PRIu64
        ",\"width\":%u,\"height\":%u,\"stage\":%u,\"row\":%u,\"byte\":%u"
        ",\"source_frame_starts\":%" PRIu64 ",\"source_bytes\":%" PRIu64
        ",\"receiver_frames\":%" PRIu64 ",\"receiver_bytes\":%" PRIu64 "}",
        s->active ? "true" : "false", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        s->start, s->hz_n, s->hz_d, s->pixel, s->line, s->total,
        s->width, s->height, s->stage, s->row, s->byte, s->frames, s->samples,
        s->soc ? s->soc->dvp.frames : 0, s->soc ? s->soc->dvp.samples : 0);
}
static void instance_init(Object *object)
{
    object_property_add_str(object, "x-lisem-camera-timing", timing, NULL);
}
static void realize(DeviceState *dev, Error **errp)
{
    VenusACamera *s = VENUSA_CAMERA(dev);
    Object *soc = s->soc_path ? object_resolve_path(s->soc_path, NULL) : NULL;
    if (!soc || !object_dynamic_cast(soc, TYPE_VENUSA_SOC)) {
        error_setg(errp, "GC0328 fixture requires a VenusA soc path and RGB888 input file"); return;
    }
    s->soc = VENUSA_SOC(soc);
    if (s->soc->dvp.camera_clock_changed) {
        error_setg(errp, "VenusA camera connector is already occupied"); return;
    }
    s->sensor = DEVICE(i2c_slave_create_simple(s->soc->i2c[1].bus, TYPE_GC0328, 0x21));
    if (s->file) {
    struct stat st;
    if (stat(s->file, &st) || !S_ISREG(st.st_mode) || st.st_size != 640 * 480 * 3) {
        error_setg(errp, "GC0328 scene must be one 640x480 RGB888 regular file"); return;
    }
    g_autofree char *data = NULL; gsize length = 0; GError *error = NULL;
    if (!g_file_get_contents(s->file, &data, &length, &error)) {
        error_setg(errp, "read GC0328 scene: %s", error->message); g_error_free(error); return;
    }
    if (!gc0328_set_frame(s->sensor, (uint8_t *)data, length, errp)) { return; }
    }
    for (unsigned i = 0; i < 4; i++) {
        s->pins[i] = qdev_get_gpio_in_named(DEVICE(s->soc), "dvp-input", i);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, step, s);
    s->soc->dvp.camera_clock_changed = clock_changed;
    s->soc->dvp.camera_opaque = s;
    gc0328_set_notify(s->sensor, notify, s);
    qemu_register_resettable(OBJECT(s));
}
static void unrealize(DeviceState *dev)
{
    VenusACamera *s = VENUSA_CAMERA(dev);
    qemu_unregister_resettable(OBJECT(s));
    listenai_camera_scene_destroy(&s->scene);
    timer_free(s->timer);
    gc0328_set_notify(s->sensor, NULL, NULL);
    s->soc->dvp.camera_clock_changed = NULL;
    s->soc->dvp.camera_opaque = NULL;
    qdev_unrealize(s->sensor);
    object_unparent(OBJECT(s->sensor));
}
static bool receiver_enabled(void *opaque)
{
    VenusACamera *s = opaque;
    return s->soc->dvp.clock && !!(s->soc->dvp.regs[0x1c / 4] & 1);
}

static void source_changed(void *opaque)
{
    VenusACamera *s = opaque;
    if (!s->active) { clock_changed(s); }
}

static bool sccb_route(void *opaque)
{
    VenusACamera *s = opaque;
    return listenai_pinmux_function(&s->soc->pinmux, 34, 8) &&
           listenai_pinmux_function(&s->soc->pinmux, 33, 8);
}

DeviceState *venusa_board_camera_connect(VenusASoC *soc, Object *owner, ListenaiBoard *b)
{
    if (!b->camera) { return NULL; }
    static const unsigned pads[] = {34,33,35,23,21,22,24,25,26,27,28,29,30,31};
    for (unsigned i = 0; i < ARRAY_SIZE(pads); i++) {
        if (b->camera_pads[i] != pads[i]) {
            error_report("Invalid VenusA SCCB/DVP camera route"); exit(1);
        }
    }
    DeviceState *dev = qdev_new(TYPE_VENUSA_CAMERA);
    object_property_add_child(owner, "camera", OBJECT(dev));
    g_autofree char *path = object_get_canonical_path(OBJECT(soc));
    qdev_prop_set_string(dev, "soc", path);
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    VenusACamera *s = VENUSA_CAMERA(dev);
    listenai_board_camera_wire(b, DEVICE(soc), s->sensor);
    soc->i2c[1].route_valid = sccb_route;
    soc->i2c[1].route_opaque = s;
    listenai_camera_scene_init(&s->scene, owner, s->sensor, getenv("LISEM_QEMU_CAMERA_SHM"),
                               source_changed, receiver_enabled, s);
    return dev;
}

char *venusa_board_camera_status(DeviceState *dev)
{
    return dev ? listenai_camera_scene_status(&VENUSA_CAMERA(dev)->scene) : g_strdup("null");
}

static const Property props[] = {
    DEFINE_PROP_STRING("soc", VenusACamera, soc_path),
    DEFINE_PROP_STRING("file", VenusACamera, file),
};
static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize; dc->unrealize = unrealize;
    dc->user_creatable = true;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, props);
    dc->desc = "Explicit GC0328 camera connected to VenusA I2C1 and DVP";
}
static const TypeInfo info = {
    .name = TYPE_VENUSA_CAMERA, .parent = TYPE_DEVICE,
    .instance_size = sizeof(VenusACamera), .instance_init = instance_init,
    .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
