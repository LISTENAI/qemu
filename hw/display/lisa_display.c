/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Nonblocking, latest-frame display transport for a native host frontend. */
#include "qemu/osdep.h"
#include "qemu/lisa-mapping.h"
#include "ui/lisa_display.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/boards.h"
#include "system/reset.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qobject/qjson.h"

#define DISPLAY_MAGIC UINT64_C(0x4c49534144495331)
/* Shared with crates/core/src/display.rs. Geometry is immutable. Slot ownership:
 * 0 free, 1 producer, 2 consumer. A reader locks, then rechecks publication.
 * No bytes are read and written concurrently, even when a window stalls. */
typedef struct DisplayMap {
    uint64_t magic, width, height, stride, bytes, published, slots[3], reserved;
    uint8_t frames[]; /* Three {virtual_ns:u64, host_ns:u64, BGRA pixels} slots. */
} DisplayMap;
_Static_assert(sizeof(DisplayMap) == 80, "Display ABI mismatch");
_Static_assert(__atomic_always_lock_free(8, 0), "Display needs lock-free shared ownership");

/* Append-only diagnostic archive. count release-publishes immutable records;
 * state release-publishes the terminal boundary. Never waits for consumers. */
#define CAPTURE_MAGIC UINT64_C(0x4c53444341503031)
#define CAPTURE_HEADER 128
#define CAPTURE_LIMIT (64 * 1024 * 1024)
enum { CAP_RUNNING = 1, CAP_COMPLETE, CAP_OVERFLOW, CAP_EXPIRED, CAP_RESET };
typedef struct DisplayCapture {
    uint64_t magic, width, height, bytes, capacity, count, state, start_sequence;
    uint64_t started_ns, end_sequence, ended_ns, deadline_host_ns;
    uint64_t reserved[4];
    uint8_t frames[]; /* sequence, virtual_ns, host_ns, then BGRA pixels */
} DisplayCapture;
_Static_assert(sizeof(DisplayCapture) == CAPTURE_HEADER, "Capture ABI mismatch");

typedef struct LisaDisplay {
    DisplayChangeListener listener;
    DisplaySurface *surface;
    DisplayMap *map;
    LisaMapping mapping;
    uint64_t sequence;
    DisplayCapture *capture;
    LisaMapping capture_mapping;
    char *capture_id;
    QEMUTimer *capture_timer;
} LisaDisplay;

static void capture_finish(LisaDisplay *s, unsigned state)
{
    DisplayCapture *p = s->capture;
    if (!p || __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != CAP_RUNNING) { return; }
    if (state == CAP_COMPLETE &&
        (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= p->deadline_host_ns) {
        state = CAP_EXPIRED;
    }
    p->end_sequence = s->sequence;
    p->ended_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    __atomic_store_n(&p->state, state, __ATOMIC_RELEASE);
    timer_del(s->capture_timer);
}

static void capture_expire(void *opaque) { capture_finish(opaque, CAP_EXPIRED); }
static void capture_reset(void *opaque) { capture_finish(opaque, CAP_RESET); }

static void capture_append(LisaDisplay *s, const uint8_t *frame)
{
    DisplayCapture *p = s->capture;
    if (!p || __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != CAP_RUNNING) { return; }
    if ((uint64_t)qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= p->deadline_host_ns) {
        capture_finish(s, CAP_EXPIRED); return;
    }
    uint64_t n = __atomic_load_n(&p->count, __ATOMIC_RELAXED);
    if (n == p->capacity) { capture_finish(s, CAP_OVERFLOW); return; }
    uint8_t *dst = p->frames + n * (24 + p->bytes);
    memcpy(dst, &s->sequence, 8);
    memcpy(dst + 8, frame, 16 + p->bytes);
    __atomic_store_n(&p->count, n + 1, __ATOMIC_RELEASE);
}

static void capture_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    LisaDisplay *s = opaque;
    g_autofree char *value = g_strdup_printf(
        "{\"width\":%" PRIu64 ",\"height\":%" PRIu64 ",\"active\":%s}",
        s->map->width, s->map->height, s->capture ? "true" : "false");
    visit_type_str(v, name, &value, errp);
}

static void capture_set(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    LisaDisplay *s = opaque;
    g_autofree char *value = NULL;
    if (!visit_type_str(v, name, &value, errp)) { return; }
    if (strlen(value) > 4096) { error_setg(errp, "Display capture request too large"); return; }
    QObject *parsed = qobject_from_json(value, errp);
    g_autoptr(QDict) q = qobject_to(QDict, parsed);
    if (!q) {
        if (parsed) { error_setg(errp, "Expected display capture request object"); }
        qobject_unref(parsed); return;
    }
    const char *action = q ? qdict_get_try_str(q, "action") : NULL;
    const char *id = q ? qdict_get_try_str(q, "capture") : NULL;
    if (!action || !id) { goto invalid; }
    if (!strcmp(action, "stop") || !strcmp(action, "discard")) {
        if (qdict_size(q) != 2 || !s->capture_id || strcmp(id, s->capture_id)) { goto invalid; }
        if (!s->capture) {
            if (!strcmp(action, "discard")) { return; }
            goto invalid;
        }
        capture_finish(s, CAP_COMPLETE);
        if (!strcmp(action, "discard")) {
            lisa_mapping_close(&s->capture_mapping); s->capture = NULL;
        }
        return;
    }
    int64_t capacity, milliseconds;
    QNum *n = qobject_to(QNum, qdict_get(q, "capacity"));
    QNum *ms = qobject_to(QNum, qdict_get(q, "max_ms"));
    if (strcmp(action, "start") || qdict_size(q) != 4 || !n || !ms ||
        !qnum_get_try_int(n, &capacity) || !qnum_get_try_int(ms, &milliseconds) ||
        capacity < 1 || capacity > 4096 || milliseconds < 1 || milliseconds > 60000 ||
        capacity > (CAPTURE_LIMIT - CAPTURE_HEADER) / (24 + s->map->bytes)) { goto invalid; }
    if (s->capture) { error_setg(errp, "Release the previous display capture first"); return; }
    size_t size = CAPTURE_HEADER + capacity * (24 + s->map->bytes);
    DisplayCapture *p = lisa_named_mapping(id, size, true, &s->capture_mapping);
    if (!p) { error_setg_errno(errp, errno, "Cannot create display capture"); return; }
    p->width = s->map->width; p->height = s->map->height; p->bytes = s->map->bytes;
    p->capacity = capacity; p->start_sequence = s->sequence;
    p->started_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    p->deadline_host_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + milliseconds * 1000000;
    g_free(s->capture_id);
    s->capture = p; s->capture_id = g_strdup(id);
    __atomic_store_n(&p->state, CAP_RUNNING, __ATOMIC_RELEASE);
    __atomic_store_n(&p->magic, CAPTURE_MAGIC, __ATOMIC_RELEASE);
    timer_mod(s->capture_timer, p->deadline_host_ns);
    return;
invalid:
    error_setg(errp, "Invalid display capture action, identity, capacity or deadline");
}

static void capture_release(Object *obj, const char *name, void *opaque)
{
    LisaDisplay *s = opaque;
    qemu_unregister_reset(capture_reset, s);
    timer_free(s->capture_timer);
    lisa_mapping_close(&s->capture_mapping);
    g_clear_pointer(&s->capture_id, g_free);
    s->capture = NULL;
}

static void publish(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    LisaDisplay *s = container_of(dcl, LisaDisplay, listener);
    DisplayMap *p = s->map;
    if (!s->surface || !p || w <= 0 || h <= 0) { return; }
    for (unsigned slot = 0; slot < 3; slot++) {
        uint64_t expected = 0;
        /* Keep the current published slot available while preparing the next. */
        uint64_t previous = __atomic_load_n(&p->published, __ATOMIC_ACQUIRE);
        if (previous && (previous & 3) == slot) { continue; }
        if (!__atomic_compare_exchange_n(&p->slots[slot], &expected, 1, false,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { continue; }
        uint8_t *frame = p->frames + slot * (16 + p->bytes);
        uint64_t clocks[] = {qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), g_get_monotonic_time() * 1000};
        memcpy(frame, clocks, sizeof(clocks));
        for (unsigned row = 0; row < p->height; row++) {
            uint32_t *src = (uint32_t *)(surface_data(s->surface) + row * surface_stride(s->surface));
            uint32_t *dst = (uint32_t *)(frame + 16 + row * p->stride);
            for (unsigned col = 0; col < p->width; col++) { dst[col] = src[col] | 0xff000000; }
        }
        __atomic_store_n(&p->slots[slot], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&p->published, (++s->sequence << 2) | slot, __ATOMIC_RELEASE);
        capture_append(s, frame);
        return;
    }
}

static void switch_surface(DisplayChangeListener *dcl, DisplaySurface *surface)
{
    LisaDisplay *s = container_of(dcl, LisaDisplay, listener);
    if (surface_width(surface) != s->map->width || surface_height(surface) != s->map->height ||
        surface_format(surface) != PIXMAN_x8r8g8b8) {
        error_report("Unsupported host display surface change"); exit(1);
    }
    s->surface = surface;
    publish(dcl, 0, 0, s->map->width, s->map->height);
}

static void refresh(DisplayChangeListener *dcl) { graphic_hw_update(dcl->con); }
static const DisplayChangeListenerOps listener_ops = {
    .dpy_name = "lisa-sim",
    .dpy_refresh = refresh, .dpy_gfx_update = publish, .dpy_gfx_switch = switch_surface,
};

void lisa_display_init(QemuConsole *console, const char *directory, const char *shared)
{
    DisplaySurface *surface = qemu_console_surface(console);
    unsigned width = surface_width(surface), height = surface_height(surface);
    if (!width || !height || width > 2048 || height > 2048) {
        error_report("Unsupported host display size"); exit(1);
    }
    size_t bytes = width * height * 4, length = sizeof(DisplayMap) + 3 * (16 + bytes);
    DisplayMap *p;
    LisaDisplay *s = g_new0(LisaDisplay, 1);
    if (shared) {
        p = lisa_named_mapping(shared, length, true, &s->mapping);
    } else {
        g_autofree char *path = g_build_filename(directory, "framebuffer", NULL);
        int fd = lisa_open_shared_file(path, true);
        if (fd < 0 || ftruncate(fd, length)) { perror("Create host display map"); exit(1); }
        p = lisa_shared_mapping(fd, length);
        close(fd);
    }
    if (!p) { perror("Map host display"); exit(1); }
    p->width = width; p->height = height; p->stride = width * 4; p->bytes = bytes;
    __atomic_store_n(&p->magic, DISPLAY_MAGIC, __ATOMIC_RELEASE);
    s->map = p; s->surface = surface;
    s->listener = (DisplayChangeListener){.con = console, .ops = &listener_ops, .update_interval = 16};
    s->capture_timer = timer_new_ns(QEMU_CLOCK_REALTIME, capture_expire, s);
    qemu_register_reset(capture_reset, s);
    object_property_add(qdev_get_machine(), "x-lisem-display-capture", "str",
                        capture_get, capture_set, capture_release, s);
    register_displaychangelistener(&s->listener);
}
