/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host optical scenes; sensor timing and receiver clocks stay in devices. */
#include "qemu/osdep.h"
#include "hw/core/listenai-camera-scene.h"
#include "hw/i2c/gc0328.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"

/* Media publications never run through the monitor. A bounded reader locks
 * the latest complete slot; the sensor copies its pending scene at SOF. */
static int camera_input_receive(ListenaiCameraScene *s, uint64_t generation, Error **errp)
{
    LisaCameraInput *p = s->camera_mapping.address;
    if (!p) { return 0; }
    int best = -1;
    uint64_t sequence = generation == s->camera_generation ? s->camera_sequence : 0;
    /* Even selection metadata is accessed only with reader ownership. A
     * pending new generation cannot hide the active source's other slots. */
    for (unsigned slot = 0; slot < 3; slot++) {
        uint64_t expected = 0;
        if (!__atomic_compare_exchange_n(&p->owners[slot], &expected, 2, false,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { continue; }
        LisaCameraFrame *frame = &p->frames[slot];
        if (frame->generation == generation && frame->sequence > sequence) {
            if (best >= 0) { __atomic_store_n(&p->owners[best], 0, __ATOMIC_RELEASE); }
            best = slot; sequence = frame->sequence;
        } else { __atomic_store_n(&p->owners[slot], 0, __ATOMIC_RELEASE); }
    }
    if (best < 0) { return 0; }
    LisaCameraFrame *frame = &p->frames[best];
    int result = 1;
    if (frame->state > 3 || ((frame->state == 1 || frame->state == 2) && !frame->frame_index) ||
        (generation == s->camera_generation && frame->frame_index < s->camera_frame_index)) {
        error_setg(errp, "Invalid camera source state"); result = -1; goto out;
    }
    if (frame->state == 1 || frame->state == 2) {
        if (!gc0328_set_frame(s->camera, frame->rgb, LISA_CAMERA_BYTES, errp)) {
            result = -1; goto out;
        }
    } else { gc0328_clear_frame(s->camera); }
    s->camera_sequence = frame->sequence;
    s->camera_frame_index = frame->frame_index;
    s->camera_host_ns = frame->host_ns;
    s->camera_received_ns = g_get_monotonic_time() * 1000;
    s->camera_source_state = frame->state;
out:
    __atomic_store_n(&p->owners[best], 0, __ATOMIC_RELEASE);
    return result;
}

static void camera_input_poll(void *opaque)
{
    ListenaiCameraScene *s = opaque;
    if (s->camera_generation) {
        Error *error = NULL;
        int changed = camera_input_receive(s, s->camera_generation, &error);
        if (error) { error_report_err(error); exit(1); }
        if (changed > 0) { s->changed(s->opaque); }
    }
    /* Wake an idle receiver after a live source reconnects. Host polling does
     * not advance guest time; each new frame is still latched at sensor SOF. */
    timer_mod(s->camera_poll, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 16);
}

static void camera_source_set(ListenaiCameraScene *s, const char *value, Error **errp)
{
    uint64_t generation;
    if (!s->camera_mapping.address) { error_setg(errp, "Camera shared input is not configured"); return; }
    if (!*value || strspn(value, "0123456789") != strlen(value) ||
        qemu_strtou64(value, NULL, 10, &generation) || !generation ||
        generation <= s->camera_generation) {
        error_setg(errp, "Camera source generation is invalid or stale"); return;
    }
    int result = camera_input_receive(s, generation, errp);
    if (result < 0) { return; }
    if (!result) { error_setg(errp, "Camera source has no complete published frame"); return; }
    s->camera_generation = generation;
    s->camera_latched_frame = 0;
    s->camera_skipped = s->camera_repeated = 0;
    LisaCameraInput *p = s->camera_mapping.address;
    __atomic_store_n(&p->consumed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&p->skipped, 0, __ATOMIC_RELEASE);
    s->changed(s->opaque);
}

char *listenai_camera_scene_status(ListenaiCameraScene *s)
{
    LisaCameraInput *p = s->camera_mapping.address;
    const char *states[] = {"none", "still", "live", "disconnected"};
    return g_strdup_printf("{\"receiver_enabled\":%s,\"generation\":%" PRIu64 ",\"sequence\":%" PRIu64
        ",\"received_frame\":%" PRIu64 ",\"sampled_frame\":%" PRIu64
        ",\"source_state\":\"%s\",\"captured_host_ns\":%" PRIu64
        ",\"received_host_ns\":%" PRIu64 ",\"skipped_frames\":%" PRIu64
        ",\"repeated_frames\":%" PRIu64 ",\"producer_dropped_frames\":%" PRIu64 "}",
        (s->enabled && s->enabled(s->opaque)) ? "true" : "false",
        s->camera_generation, s->camera_sequence, s->camera_frame_index, s->camera_latched_frame,
        states[s->camera_source_state],
        s->camera_host_ns, s->camera_received_ns, s->camera_skipped, s->camera_repeated,
        p ? __atomic_load_n(&p->dropped, __ATOMIC_ACQUIRE) : 0);
}

static void camera_frame_set(ListenaiCameraScene *s, const char *value, Error **errp)
{
    if (s->camera_mapping.address) { error_setg(errp, "Shared camera input owns this run"); return; }
    if (!*value) { gc0328_clear_frame(s->camera); return; }
    const size_t bytes = 640 * 480 * 3, encoded = bytes / 3 * 4;
    if (strlen(value) != encoded || strspn(value,
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") != encoded) {
        error_setg(errp, "Camera input must be base64 640x480 RGB888"); return;
    }
    gsize length;
    g_autofree uint8_t *rgb = g_base64_decode(value, &length);
    if (gc0328_set_frame(s->camera, rgb, length, errp)) {
        s->changed(s->opaque);
    }
}


int listenai_camera_scene_receive(ListenaiCameraScene *s, Error **errp)
{
    return s->camera_generation ? camera_input_receive(s, s->camera_generation, errp) : 0;
}

void listenai_camera_scene_latch(ListenaiCameraScene *s)
{
    if (s->camera_generation) {
        if (s->camera_latched_frame == s->camera_frame_index) { s->camera_repeated++; }
        else { s->camera_skipped += s->camera_frame_index - s->camera_latched_frame - 1; }
        s->camera_latched_frame = s->camera_frame_index;
        LisaCameraInput *p = s->camera_mapping.address;
        __atomic_store_n(&p->consumed, s->camera_latched_frame, __ATOMIC_RELEASE);
        __atomic_store_n(&p->skipped, s->camera_skipped, __ATOMIC_RELEASE);
    }
}

static void source_property(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    char *value = NULL;
    if (visit_type_str(v, name, &value, errp)) { camera_source_set(opaque, value, errp); }
    g_free(value);
}

static void frame_property(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    char *value = NULL;
    if (visit_type_str(v, name, &value, errp)) { camera_frame_set(opaque, value, errp); }
    g_free(value);
}

void listenai_camera_scene_init(ListenaiCameraScene *s, Object *owner,
                               DeviceState *sensor, const char *shared,
                               void (*changed)(void *), bool (*enabled)(void *), void *opaque)
{
    s->camera = sensor; s->changed = changed; s->enabled = enabled; s->opaque = opaque;
    object_property_add(owner, "x-lisa-camera-frame", "str", NULL, frame_property, NULL, s);
    object_property_add(owner, "x-lisa-camera-source", "str", NULL, source_property, NULL, s);
    if (shared) {
        LisaCameraInput *p = lisa_named_mapping(shared, sizeof(*p), true, &s->camera_mapping);
        if (!p) { perror("Create camera input map"); exit(1); }
        p->width = 640; p->height = 480; p->stride = 640 * 3; p->bytes = LISA_CAMERA_BYTES;
        __atomic_store_n(&p->magic, LISA_CAMERA_MAGIC, __ATOMIC_RELEASE);
        s->camera_poll = timer_new_ms(QEMU_CLOCK_REALTIME, camera_input_poll, s);
        timer_mod(s->camera_poll, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 16);
    }
}

void listenai_camera_scene_destroy(ListenaiCameraScene *s)
{
    if (s->camera_poll) { timer_free(s->camera_poll); }
    lisa_mapping_close(&s->camera_mapping);
}
