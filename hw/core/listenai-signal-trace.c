/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Opt-in digital transitions, sampled at the device event's virtual time. */
#include "qemu/osdep.h"
#include "hw/core/listenai-signal-trace.h"
#include "hw/i2c/listenai_extio.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "qemu/timer.h"

static void changed(void *opaque, unsigned first, uint32_t driven, uint32_t levels)
{
    ListenaiSignalTraceBank *bank = opaque;
    ListenaiSignalTrace *s = bank->trace;
    if (!s->active) { return; }
    for (unsigned i = 0; i < 32; i++) {
        unsigned pin = bank->first + first + i;
        if (pin >= LISTENAI_TRACE_PINS || !s->selected[pin]) { continue; }
        bool d = !!(driven & (1u << i)), l = !!(levels & (1u << i));
        bool initial = !s->seen[pin];
        if (!initial && s->driven[pin] == d && s->level[pin] == l) { continue; }
        s->seen[pin] = true; s->driven[pin] = d; s->level[pin] = l;
        ListenaiSignalEvent *e = &s->events[(s->next - 1) % s->capacity];
        *e = (ListenaiSignalEvent){ .sequence = s->next++,
            .ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), .pin = pin,
            .driven = d, .level = l, .initial = initial };
        s->count = MIN(s->count + 1, s->capacity);
        if (s->next == INT64_MAX) { listenai_signal_trace_stop(s, "sequence-limit"); return; }
    }
}

void listenai_signal_trace_stop(ListenaiSignalTrace *s, const char *reason)
{
    if (!s || !s->active) { return; }
    s->active = false; s->reason = reason;
    s->ended_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < 2; i++) { listenai_gpio_observe(s->gpio[i], NULL, NULL); }
    if (s->extio) { listenai_extio_observe(s->extio, NULL, NULL); }
}

char *listenai_signal_trace_json(ListenaiSignalTrace *s)
{
    if (!s || !s->id[0]) { return g_strdup("{\"id\":null,\"status\":\"idle\",\"events\":[]}"); }
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"id\":\"%s\",\"status\":\"%s\",\"reason\":\"%s\","
        "\"started_ns\":%" PRId64 ",\"ended_ns\":%" PRId64 ",\"capacity\":%u,"
        "\"oldest\":%" PRIu64 ",\"next\":%" PRIu64 ",\"dropped\":%" PRIu64 ",\"events\":[",
        s->id, s->active ? "running" : "stopped", s->reason,
        s->started_ns, s->ended_ns, s->capacity, s->next - s->count,
        s->next, s->next - 1 - s->count);
    for (uint64_t sequence = s->next - s->count; sequence < s->next; sequence++) {
        ListenaiSignalEvent *e = &s->events[(sequence - 1) % s->capacity];
        g_string_append_printf(out, "%s{\"sequence\":%" PRIu64 ",\"virtual_ns\":%" PRId64
            ",\"endpoint\":%u,\"driven\":%s,\"level\":%s,\"initial\":%s}",
            sequence == s->next - s->count ? "" : ",", e->sequence, e->ns, e->pin,
            e->driven ? "true" : "false", e->level ? "true" : "false",
            e->initial ? "true" : "false");
    }
    g_string_append(out, "],\"endpoints\":[");
    bool comma = false;
    for (unsigned i = 0; i < LISTENAI_TRACE_PINS; i++) {
        if (s->selected[i]) { g_string_append_printf(out, "%s%u", comma ? "," : "", i); comma = true; }
    }
    g_string_append(out, "]}");
    return g_string_free(out, false);
}

static bool identifier(const char *id)
{
    if (!id || !*id || strlen(id) > 64) { return false; }
    for (const char *p = id; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-') { return false; }
    }
    return true;
}

static void request(ListenaiSignalTrace *s, const char *text, Error **errp)
{
    if (strlen(text) > 8192) { error_setg(errp, "Signal trace request too large"); return; }
    QObject *obj = qobject_from_json(text, errp);
    g_autoptr(QDict) q = qobject_to(QDict, obj);
    if (!q) {
        if (obj) { error_setg(errp, "Expected signal trace request object"); }
        qobject_unref(obj); return;
    }
    const char *action = qdict_get_try_str(q, "action"), *id = qdict_get_try_str(q, "id");
    if (!identifier(id) || !action) { goto invalid; }
    if (!strcmp(action, "stop")) {
        if (qdict_size(q) != 2 || strcmp(id, s->id)) { goto invalid; }
        listenai_signal_trace_stop(s, "requested"); return;
    }
    if (strcmp(action, "start") || qdict_size(q) != 4) { goto invalid; }
    QList *pins = qdict_get_qlist(q, "endpoints");
    QNum *size = qobject_to(QNum, qdict_get(q, "capacity"));
    int64_t capacity;
    if (!pins || !size || !qnum_get_try_int(size, &capacity) ||
        capacity < 1 || capacity > LISTENAI_TRACE_CAPACITY) { goto invalid; }
    bool selected[LISTENAI_TRACE_PINS] = {0}; unsigned count = 0;
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(pins, entry) {
        int64_t pin; QNum *n = qobject_to(QNum, qlist_entry_obj(entry));
        if (!n || !qnum_get_try_int(n, &pin) || pin < 0 || pin >= LISTENAI_TRACE_PINS ||
            !(s->valid[pin / 32] & (1u << (pin % 32))) || selected[pin]) { goto invalid; }
        selected[pin] = true; count++;
    }
    if (!count || count > capacity) { goto invalid; }
    if (!strcmp(id, s->id)) {
        if (s->active && s->capacity == capacity && !memcmp(selected, s->selected, sizeof(selected))) { return; }
        goto invalid;
    }
    if (s->active) { error_setg(errp, "A signal trace is already running"); return; }
    g_free(s->events); s->events = g_new0(ListenaiSignalEvent, capacity);
    s->capacity = capacity; s->count = 0; s->next = 1;
    memcpy(s->selected, selected, sizeof(selected)); memset(s->seen, 0, sizeof(s->seen));
    g_strlcpy(s->id, id, sizeof(s->id)); s->reason = "";
    s->started_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); s->ended_ns = 0; s->active = true;
    for (unsigned i = 0; i < 2; i++) { listenai_gpio_observe(s->gpio[i], changed, &s->banks[i]); }
    if (s->extio) { listenai_extio_observe(s->extio, changed, &s->banks[2]); }
    return;
invalid:
    error_setg(errp, "Invalid signal trace action, capture ID, capacity or endpoints");
}

static void get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    g_autofree char *value = listenai_signal_trace_json(opaque);
    visit_type_str(v, name, &value, errp);
}

static void set(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    g_autofree char *value = NULL;
    if (visit_type_str(v, name, &value, errp)) { request(opaque, value, errp); }
}

void listenai_signal_trace_init(ListenaiSignalTrace *s, Object *owner,
                               Object *gpio_owner, ListenaiGPIO *gpio,
                               unsigned pa, unsigned pb,
                               DeviceState *extio)
{
    assert(pa <= 32 && pb <= 32);
    /* Board child properties are released before its instance finalizer.
     * Keep producers alive until observers have been detached. */
    s->gpio_owner = object_ref(gpio_owner);
    s->gpio[0] = &gpio[0]; s->gpio[1] = &gpio[1];
    s->extio = extio ? DEVICE(object_ref(OBJECT(extio))) : NULL;
    s->valid[0] = pa == 32 ? UINT32_MAX : (1u << pa) - 1;
    s->valid[1] = pb == 32 ? UINT32_MAX : (1u << pb) - 1;
    s->valid[2] = extio ? listenai_extio_valid_pins() : 0;
    for (unsigned i = 0; i < 3; i++) {
        s->banks[i].trace = s; s->banks[i].first = i == 2 ? 64 : 0;
    }
    object_property_add(owner, "x-lisa-signal-trace", "str", get, set, NULL, s);
}

void listenai_signal_trace_destroy(ListenaiSignalTrace *s)
{
    listenai_signal_trace_stop(s, "destroyed");
    g_free(s->events); s->events = NULL;
    object_unref(OBJECT(s->extio)); s->extio = NULL;
    object_unref(s->gpio_owner); s->gpio_owner = NULL;
}
