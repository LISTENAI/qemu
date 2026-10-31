/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Logical plaintext legacy central. All link deadlines use virtual time. */
#include "qemu/osdep.h"
#include "hw/core/listenai-ble-central.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qobject/qbool.h"

#define HISTORY 256
#define QUEUE 64
#define AA 0xa1b2c3d4u
#define ADV_AA 0x8e89bed6u

typedef struct Packet {
    uint64_t token;
    unsigned length;
    uint8_t bytes[29];
} Packet;

struct ListenaiBLECentral {
    ListenaiBLEReceive receive;
    void *opaque;
    int64_t *epoch;
    QEMUTimer *timer;
    bool scanning, connecting, connected, confirmed, pending, version_sent,
        trace;
    uint8_t address[6], address_type, sn, nesn;
    Packet queue[QUEUE], outstanding;
    unsigned head, count, air_length, air_channel, event;
    uint8_t air[39], acl[517];
    unsigned acl_length, acl_expected, acl_cid;
    uint32_t air_address;
    uint64_t first, last_rx, sequence, generation;
    char *history[HISTORY];
    const char *error;
};

static uint64_t now(ListenaiBLECentral *s)
{
    return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - *s->epoch) / 500;
}

static void record(ListenaiBLECentral *s, const char *kind, const uint8_t *p,
                   unsigned length, unsigned channel, uint64_t at)
{
    GString *row = g_string_new(NULL);
    const char *direction =
        !strcmp(kind, "tx") || !strcmp(kind, "delivered") ? "out" :
        !strcmp(kind, "rx") || !strcmp(kind, "advertisement") ||
        !strcmp(kind, "att") || !strcmp(kind, "llcp") ||
        !strcmp(kind, "l2cap") ? "in" : "local";
    const char *layer =
        !strcmp(kind, "att") ? "att" :
        !strcmp(kind, "l2cap") ? "l2cap" :
        !strcmp(kind, "tx") || !strcmp(kind, "rx") ||
        !strcmp(kind, "advertisement") || !strcmp(kind, "llcp") ? "link" :
        "control";
    g_string_append_printf(
        row,
        "{\"sequence\":%" PRIu64 ",\"generation\":%" PRIu64
        ",\"virtual_ns\":%" PRIu64
        ",\"kind\":\"%s\",\"direction\":\"%s\",\"layer\":\"%s\",\"channel\":%u,\"value\":\"",
        ++s->sequence, s->generation, *s->epoch + at * 500, kind, direction,
        layer, channel);
    for (unsigned i = 0; i < length; i++) {
        g_string_append_printf(row, "%02x", p[i]);
    }
    g_string_append(row, "\"}");
    unsigned slot = (s->sequence - 1) % HISTORY;
    g_free(s->history[slot]);
    s->history[slot] = g_string_free(row, false);
}

static void schedule(ListenaiBLECentral *s, uint64_t at)
{
    timer_mod(s->timer, *s->epoch + at * 500);
}

static void fail(ListenaiBLECentral *s, const char *reason)
{
    s->error = reason;
    s->confirmed = s->connected = s->connecting = false;
    s->pending = false;
    s->count = 0;
    timer_del(s->timer);
    record(s, reason, NULL, 0, 0, now(s));
}

static bool enqueue(ListenaiBLECentral *s, unsigned llid, const uint8_t *p,
                    unsigned n)
{
    if (n > 27 || s->count == QUEUE) {
        fail(s, "queue_full");
        return false;
    }
    Packet *q = &s->queue[(s->head + s->count++) % QUEUE];
    q->token = 0;
    q->bytes[0] = llid;
    q->bytes[1] = n;
    memcpy(q->bytes + 2, p, n);
    q->length = n + 2;
    return true;
}

static bool att(ListenaiBLECentral *s, const uint8_t *p, unsigned n)
{
    if (!n || n > 23) {
        return false;
    }
    uint8_t data[27];
    stw_le_p(data, n);
    stw_le_p(data + 2, 4);
    memcpy(data + 4, p, n);
    return enqueue(s, 2, data, n + 4);
}

static void input(void *opaque)
{
    ListenaiBLECentral *s = opaque;
    uint64_t at = now(s);
    if (s->air_length) {
        unsigned length = s->air_length;
        s->air_length = 0;
        bool accepted = s->receive(s->opaque, s->air, length, s->air_channel,
                                   s->air_address);
        record(s, accepted ? "tx" : "filtered", s->air, length, s->air_channel,
               at);
        if ((s->air[0] & 15) == 5 && s->connecting) {
            if (!accepted) {
                fail(s, "connect_filtered");
                return;
            }
            s->connected = true;
            s->connecting = false;
            s->event = 1;
            s->first = at + 2 * (352 + 1250 + 2500 + 625);
            s->last_rx = at;
            record(s, "connected", NULL, 0, s->air_channel, at);
            schedule(s, s->first);
        }
        return;
    }
    if (!s->connected) {
        return;
    }
    if (at - s->last_rx >= 4000000) {
        fail(s, "supervision_timeout");
        return;
    }
    if (!s->pending) {
        if (s->count) {
            s->outstanding = s->queue[s->head];
            s->head = (s->head + 1) % QUEUE;
            s->count--;
        } else {
            s->outstanding.token = 0;
            s->outstanding.length = 2;
            s->outstanding.bytes[0] = 1;
            s->outstanding.bytes[1] = 0;
        }
        s->pending = true;
    }
    uint8_t data[29];
    memcpy(data, s->outstanding.bytes, s->outstanding.length);
    data[0] |= s->sn << 3 | s->nesn << 2;
    unsigned ch = (s->event * 5) % 37;
    bool accepted = s->receive(s->opaque, data, s->outstanding.length, ch, AA);
    if (s->trace || !accepted) {
        record(s, accepted ? "tx" : "filtered", data, s->outstanding.length, ch,
               at);
    }
    schedule(s, s->first + (uint64_t)s->event++ * 60000);
}

static void payload(ListenaiBLECentral *s, const uint8_t *p, unsigned n)
{
    unsigned llid = p[0] & 3;
    if (llid == 3) {
        uint8_t reply[12];
        unsigned length = 0;
        if (n < 3) {
            fail(s, "invalid_llcp");
            return;
        }
        record(s, "llcp", p + 2, n - 2, 0, now(s));
        switch (p[2]) {
        case 0x14: {
            static const uint8_t r[] = {0x15, 27, 0, 0x48, 1, 27, 0, 0x48, 1};
            memcpy(reply, r, sizeof(r));
            length = sizeof(r);
            break;
        }
        case 8:
        case 0x0e:
            memset(reply, 0, 9);
            reply[0] = 9;
            length = 9;
            break;
        case 0x0c:
            if (!s->version_sent) {
                static const uint8_t r[] = {0x0c, 9, 0xff, 0xff, 1, 0};
                memcpy(reply, r, sizeof(r));
                length = sizeof(r);
                s->version_sent = true;
            }
            break;
        case 0x12:
            reply[0] = 0x13;
            length = 1;
            break;
        case 7:
        case 9:
        case 0x0d:
        case 0x11:
        case 0x13:
        case 0x15:
            break;
        case 0x0f: {
            static const uint8_t r[] = {0x11, 0x0f, 0x20};
            memcpy(reply, r, sizeof(r));
            length = sizeof(r);
            break;
        }
        case 2:
            fail(s, "remote_disconnect");
            return;
        case 3:
        case 0x0a:
            fail(s, "encryption_required");
            return;
        default:
            reply[0] = 7;
            reply[1] = p[2];
            length = 2;
            break;
        }
        if (length) {
            enqueue(s, 3, reply, length);
        }
        return;
    }
    if (llid == 2) {
        if (s->acl_expected || n < 6) {
            fail(s, "invalid_l2cap_start");
            return;
        }
        s->acl_expected = lduw_le_p(p + 2);
        s->acl_length = 0;
        if (!s->acl_expected || s->acl_expected > sizeof(s->acl)) {
            fail(s, "l2cap_length");
            return;
        }
        s->acl_cid = lduw_le_p(p + 4);
        if (s->acl_cid != 4 && s->acl_cid != 5) {
            record(s, "l2cap", p + 2, n - 2, 0, now(s));
            fail(s, s->acl_cid == 6 ? "pairing_required"
                                    : "unsupported_l2cap_cid");
            return;
        }
        p += 6;
        n -= 6;
    } else {
        p += 2;
        n -= 2;
    }
    if (!s->acl_expected || n > s->acl_expected - s->acl_length) {
        fail(s, "invalid_l2cap_fragment");
        return;
    }
    memcpy(s->acl + s->acl_length, p, n);
    s->acl_length += n;
    if (s->acl_length != s->acl_expected) {
        return;
    }
    s->acl_expected = 0;
    if (s->acl_cid == 5) {
        record(s, "l2cap", s->acl, s->acl_length, 0, now(s));
        if (s->acl_length < 4 || !s->acl[1] ||
            lduw_le_p(s->acl + 2) != s->acl_length - 4) {
            fail(s, "invalid_l2cap_signaling");
            return;
        }
        uint8_t reply[] = {6, 0, 5, 0, 1, s->acl[1], 2, 0, 0, 0};
        if (s->acl[0] == 0x12 && s->acl_length == 12) {
            /* Reject this optional update; the current interval is retained. */
            reply[4] = 0x13;
            reply[8] = 1;
        } else if (s->acl[0] == 1 || s->acl[0] == 0x13) {
            return;
        }
        enqueue(s, 2, reply, sizeof(reply));
        return;
    }
    if (s->acl_length > 23) {
        fail(s, "att_mtu_exceeded");
        return;
    }
    record(s, "att", s->acl, s->acl_length, 0, now(s));
    if (s->acl[0] == 2 && s->acl_length == 3) {
        uint8_t reply[] = {3, 23, 0};
        att(s, reply, sizeof(reply));
    } else if (s->acl[0] == 0x1d) {
        uint8_t reply = 0x1e;
        att(s, &reply, 1);
    } else if (s->acl[0] == 4 || s->acl[0] == 6 || s->acl[0] == 8 ||
               s->acl[0] == 0x10 || s->acl[0] == 0x0a || s->acl[0] == 0x12) {
        /* This central exposes no local attribute database. */
        if (s->acl_length < 3) {
            fail(s, "invalid_att_request");
            return;
        }
        uint8_t reply[] = {1, s->acl[0], s->acl[1], s->acl[2], 0x0a};
        att(s, reply, sizeof(reply));
    }
}

void listenai_ble_central_transmit(ListenaiBLECentral *s, const uint8_t *p,
                                   unsigned n, unsigned ch, uint32_t address,
                                   uint64_t at)
{
    if (!s || n < 2 || n != p[1] + 2) {
        return;
    }
    if (address == ADV_AA) {
        if (!s->scanning && !s->connecting) {
            return;
        }
        record(s, "advertisement", p, n, ch, at);
        if (((p[0] & 15) != 0 && ((p[0] & 15) != 6 || s->connecting)) ||
            n < 8 || timer_pending(s->timer)) {
            return;
        }
        unsigned type = s->connecting ? 5 : 3;
        if (s->connecting && (memcmp(s->address, p + 2, 6) ||
                              s->address_type != !!(p[0] & 0x40))) {
            return;
        }
        s->air[0] = type | ((p[0] & 0x40) << 1);
        s->air[1] = type == 5 ? 34 : 12;
        static const uint8_t own[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
        memcpy(s->air + 2, own, 6);
        memcpy(s->air + 8, p + 2, 6);
        if (type == 5) {
            static const uint8_t params[] = {
                0xd4, 0xc3, 0xb2, 0xa1, 0x56, 0x34, 0x12, 1,   2,   0,  24,
                0,    0,    0,    200,  0,    255,  255,  255, 255, 31, 5};
            memcpy(s->air + 14, params, sizeof(params));
        }
        s->air_length = s->air[1] + 2;
        s->air_channel = ch;
        s->air_address = ADV_AA;
        schedule(s, at + (n + 8) * 16 + 300);
        return;
    }
    if (!s->connected || address != AA || ch != ((s->event - 1) * 5) % 37) {
        return;
    }
    s->last_rx = at;
    if (!s->confirmed) {
        s->confirmed = true;
        record(s, "link_confirmed", NULL, 0, ch, at);
    }
    if (s->trace) {
        record(s, "rx", p, n, ch, at);
    }
    if (s->pending && ((p[0] >> 2) & 1) != s->sn) {
        bool disconnected =
            s->outstanding.bytes[0] == 3 && s->outstanding.bytes[2] == 2;
        s->sn ^= 1;
        s->pending = false;
        if (s->outstanding.token) {
            record(s, "delivered", NULL, 0, ch, at);
            unsigned slot = (s->sequence - 1) % HISTORY;
            char *old = s->history[slot];
            old[strlen(old) - 1] = 0;
            s->history[slot] = g_strdup_printf("%s,\"token\":%" PRIu64 "}", old,
                                               s->outstanding.token);
            g_free(old);
        }
        if (disconnected) {
            fail(s, "disconnected");
            return;
        }
    }
    if (((p[0] >> 3) & 1) != s->nesn) {
        return;
    }
    s->nesn ^= 1;
    if (p[1]) {
        payload(s, p, n);
    }
}

void listenai_ble_central_reset(ListenaiBLECentral *s)
{
    timer_del(s->timer);
    s->scanning = s->connecting = s->connected = s->confirmed = false;
    s->pending = s->version_sent = s->trace = false;
    s->head = s->count = s->air_length = s->acl_length = s->acl_expected = 0;
    s->sn = s->nesn = 0;
    s->error = NULL;
    s->generation++;
    record(s, "reset", NULL, 0, 0, now(s));
}

ListenaiBLECentral *listenai_ble_central_new(ListenaiBLEReceive receive,
                                             void *opaque, int64_t *epoch)
{
    ListenaiBLECentral *s = g_new0(ListenaiBLECentral, 1);
    s->receive = receive;
    s->opaque = opaque;
    s->epoch = epoch;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, input, s);
    return s;
}

void listenai_ble_central_destroy(ListenaiBLECentral *s)
{
    if (!s) {
        return;
    }
    timer_free(s->timer);
    for (unsigned i = 0; i < HISTORY; i++) {
        g_free(s->history[i]);
    }
    g_free(s);
}

static bool unhex(const char *text, uint8_t *out, unsigned length)
{
    if (!text || strlen(text) != length * 2) {
        return false;
    }
    for (unsigned i = 0; i < length; i++) {
        int a = g_ascii_xdigit_value(text[2 * i]),
            b = g_ascii_xdigit_value(text[2 * i + 1]);
        if (a < 0 || b < 0) {
            return false;
        }
        out[i] = a * 16 + b;
    }
    return true;
}

char *listenai_ble_central_command(ListenaiBLECentral *s, const char *command,
                                   Error **errp)
{
    QObject *object = qobject_from_json(command, errp);
    if (!object) {
        return NULL;
    }
    QDict *d = qobject_to(QDict, object);
    const char *action = d ? qdict_get_try_str(d, "action") : NULL;
    GString *reply = NULL;
    if (!action) {
        goto invalid;
    }
    if (!strcmp(action, "scan")) {
        if (s->connected || s->connecting) {
            goto invalid;
        }
        s->scanning = true;
        s->error = NULL;
    } else if (!strcmp(action, "connect")) {
        uint8_t address[6];
        if (s->connected || s->connecting ||
            !unhex(qdict_get_try_str(d, "address"), address, 6)) {
            goto invalid;
        }
        int64_t type = qdict_get_try_int(d, "address_type", 0);
        if (type < 0 || type > 1) {
            goto invalid;
        }
        memcpy(s->address, address, 6);
        timer_del(s->timer);
        s->air_length = 0;
        s->address_type = type;
        s->confirmed = false;
        s->sn = s->nesn = 0;
        s->pending = s->version_sent = false;
        s->error = NULL;
        s->head = s->count = s->acl_expected = s->acl_length = 0;
        s->connecting = true;
        s->scanning = false;
        s->generation++;
    } else if (!strcmp(action, "att")) {
        const char *hex = qdict_get_try_str(d, "value");
        uint8_t bytes[23];
        if (!s->connected || !hex || strlen(hex) > 46 || !strlen(hex) ||
            (strlen(hex) & 1) || !unhex(hex, bytes, strlen(hex) / 2)) {
            goto invalid;
        }
        if (s->count == QUEUE) {
            goto invalid;
        }
        if (!att(s, bytes, strlen(hex) / 2)) {
            goto invalid;
        }
        int64_t token = qdict_get_try_int(d, "token", 0);
        s->queue[(s->head + s->count - 1) % QUEUE].token = token;
    } else if (!strcmp(action, "disconnect")) {
        if (s->connecting) {
            goto invalid;
        }
        /* Native ownership is checked before this command. A remote end may
         * arrive after its last status query; return that actual stopped state.
         */
        if (s->connected) {
            uint8_t end[] = {2, 0x13};
            enqueue(s, 3, end, sizeof(end));
        }
    } else if (!strcmp(action, "trace")) {
        if (!qdict_haskey(d, "enabled") ||
            !qobject_to(QBool, qdict_get(d, "enabled"))) {
            goto invalid;
        }
        s->trace = qdict_get_try_bool(d, "enabled", false);
    } else if (!strcmp(action, "cancel")) {
        listenai_ble_central_reset(s);
    } else if (!strcmp(action, "events")) {
        int64_t cursor = qdict_get_try_int(d, "cursor", 0);
        if (cursor < 0 || (uint64_t)cursor > s->sequence) {
            goto invalid;
        }
        uint64_t oldest = s->sequence > HISTORY ? s->sequence - HISTORY + 1 : 1;
        uint64_t first = MAX((uint64_t)cursor + 1, oldest),
                 last = MIN(s->sequence, first + 63);
        reply = g_string_new(NULL);
        g_string_append_printf(
            reply, "{\"lost\":%" PRIu64 ",\"cursor\":%" PRIu64 ",\"events\":[",
            first - (cursor + 1), last);
        for (uint64_t seq = first; seq <= last; seq++) {
            if (seq != first) {
                g_string_append_c(reply, ',');
            }
            g_string_append(reply, s->history[(seq - 1) % HISTORY]);
        }
        g_string_append(reply, "]}");
    } else if (strcmp(action, "status")) {
        goto invalid;
    }
    if (!reply) {
        reply = g_string_new(NULL);
        g_string_append_printf(
            reply,
            "{\"generation\":%" PRIu64 ",\"sequence\":%" PRIu64
            ",\"scanning\":%s,\"connecting\":%s,\"connected\":%s,\"confirmed\":%s,\"mtu\":23,\"queued\":%u,\"error\":",
            s->generation, s->sequence, s->scanning ? "true" : "false",
            s->connecting ? "true" : "false", s->connected ? "true" : "false",
            s->confirmed ? "true" : "false", s->count);
        if (s->error) {
            g_string_append_printf(reply, "\"%s\"}", s->error);
        } else {
            g_string_append(reply, "null}");
        }
    }
    qobject_unref(object);
    return g_string_free(reply, false);
invalid:
    qobject_unref(object);
    error_setg(errp, "Invalid BLE central operation or state");
    return NULL;
}
