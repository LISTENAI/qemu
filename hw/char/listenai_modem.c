/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared AT modem transport and host socket runtime. Cellular service is an
 * ideal external environment; application bytes always use real host sockets.
 * The controller and pin routing belong to the chip, not this independently
 * assembled device.
 */
#include "qemu/osdep.h"
#include "hw/char/listenai_modem.h"
#include "hw/qdev-properties.h"
#include "io/channel-socket.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"

#define MODEM_CLASS(s) LISTENAI_MODEM_GET_CLASS(s)

typedef struct Pending {
    ListenAIModem *s;
    unsigned id;
    uint64_t epoch, generation;
    char host[256], ip[INET_ADDRSTRLEN];
} Pending;

static void arm_serial(ListenAIModem *s)
{
    if (s->output->len && !timer_pending(s->serial_timer)) {
        timer_mod(s->serial_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                       DIV_ROUND_UP(10000000000ULL, s->baud));
    }
}

void listenai_modem_output(ListenAIModem *s, const void *data, size_t len)
{
    if (s->failed) {
        return;
    }
    if (len > MODEM_SERIAL_LIMIT - (s->output->len - s->output_offset)) {
        s->failed = true;
        error_report(
            "Modem UART output overflow; reset the module before reuse");
        return;
    }
    if (s->output_offset && s->output->len + len > MODEM_SERIAL_LIMIT) {
        g_byte_array_remove_range(s->output, 0, s->output_offset);
        s->output_offset = 0;
    }
    g_byte_array_append(s->output, data, len);
    arm_serial(s);
}

void listenai_modem_reply(ListenAIModem *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    listenai_modem_output(s, text, strlen(text));
    g_free(text);
}

static void serial_tick(void *opaque)
{
    ListenAIModem *s = opaque;
    if (s->output->len && s->uart.deliver &&
        s->uart.deliver(s->uart.controller, s->output->data[s->output_offset],
                        s->baud)) {
        if (s->payload_after && !--s->payload_after) {
            s->after_cr = false;
        }
        if (++s->output_offset == s->output->len) {
            g_byte_array_set_size(s->output, 0);
            s->output_offset = 0;
        }
        if (s->pending_baud && !--s->baud_after) {
            s->baud = s->pending_baud;
            s->pending_baud = 0;
        }
        s->tx_bytes++;
    }
    arm_serial(s);
}

void listenai_modem_close(ListenAIModemEndpoint *e)
{
    e->generation++;
    e->opening = e->notified = false;
    if (e->socket) {
        qio_channel_close(QIO_CHANNEL(e->socket), NULL);
        object_unref(OBJECT(e->socket));
        e->socket = NULL;
    }
    g_byte_array_set_size(e->rx, 0);
    g_byte_array_set_size(e->tx, 0);
    g_queue_clear_full(&e->packets, (GDestroyNotify)g_bytes_unref);
}

static void disconnect(ListenAIModem *s, unsigned i)
{
    ListenAIModemEndpoint *e = &s->ep[i];
    if (e->socket || e->opening) {
        listenai_modem_close(e);
        MODEM_CLASS(s)->closed(s, i);
    }
}

static void pending_free(gpointer opaque)
{
    Pending *p = opaque;
    p->s->tasks--;
    object_unref(OBJECT(p->s));
    g_free(p);
}

static Pending *pending_new(ListenAIModem *s, unsigned id)
{
    if (s->tasks >= MODEM_CLASS(s)->socket_count + 1) {
        return NULL;
    }
    Pending *p = g_new0(Pending, 1);
    p->s = s;
    p->id = id;
    p->epoch = s->epoch;
    s->tasks++;
    object_ref(OBJECT(s));
    return p;
}

static void opened(QIOTask *task, gpointer opaque)
{
    Pending *p = opaque;
    ListenAIModem *s = p->s;
    ListenAIModemEndpoint *e = &s->ep[p->id];
    QIOChannelSocket *socket = QIO_CHANNEL_SOCKET(qio_task_get_source(task));
    Error *err = NULL;
    bool failed = qio_task_propagate_error(task, &err);
    bool current = p->epoch == s->epoch && e->generation == p->generation &&
                   e->opening && s->sim && s->pdp &&
                   qemu_clock_get_ms(QEMU_CLOCK_REALTIME) < e->deadline;
    if (!current || failed ||
        qio_channel_set_blocking(QIO_CHANNEL(socket), false, &err) < 0) {
        qio_channel_close(QIO_CHANNEL(socket), NULL);
        if (current) {
            e->opening = false;
            MODEM_CLASS(s)->opened(s, p->id, false);
        }
    } else {
        e->opening = false;
        e->socket = socket;
        object_ref(OBJECT(socket));
        MODEM_CLASS(s)->opened(s, p->id, true);
    }
    error_free(err);
}

static void dns_worker(QIOTask *task, gpointer opaque)
{
    Pending *p = opaque;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(p->host, NULL, &hints, &result) == 0) {
        struct sockaddr_in *address = (struct sockaddr_in *)result->ai_addr;
        inet_ntop(AF_INET, &address->sin_addr, p->ip, sizeof(p->ip));
        freeaddrinfo(result);
    }
}

static void dns_done(QIOTask *task, gpointer opaque)
{
    Pending *p = opaque;
    ListenAIModem *s = p->s;
    if (p->epoch != s->epoch || p->generation != s->dns_generation ||
        !s->dns_pending ||
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= s->dns_deadline) {
        return;
    }
    s->dns_pending = false;
    MODEM_CLASS(s)->resolved(s, *p->ip && s->pdp && s->sim ? p->ip : NULL);
}

static void network_tick(void *opaque)
{
    ListenAIModem *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (s->dns_pending && now >= s->dns_deadline) {
        s->dns_pending = false;
        MODEM_CLASS(s)->resolved(s, NULL);
    }
    if (s->payload_len && now >= s->payload_deadline) {
        s->payload_len = s->payload_used = s->payload_after = 0;
        MODEM_CLASS(s)->error(s);
    }
    for (unsigned i = 0; i < MODEM_CLASS(s)->socket_count; i++) {
        ListenAIModemEndpoint *e = &s->ep[i];
        if (e->opening && now >= e->deadline) {
            listenai_modem_close(e);
            MODEM_CLASS(s)->opened(s, i, false);
        }
        if (!e->socket) {
            continue;
        }
        if (e->tx->len) {
            Error *err = NULL;
            ssize_t n = qio_channel_write(
                QIO_CHANNEL(e->socket), (char *)e->tx->data, e->tx->len, &err);
            error_free(err);
            if (n == QIO_CHANNEL_ERR_BLOCK) {
                /* Keep bytes until the host socket accepts them. */
            } else if (n <= 0 || (e->udp && n != e->tx->len)) {
                disconnect(s, i);
                continue;
            } else {
                g_byte_array_remove_range(e->tx, 0, n);
            }
        }
        if (s->output->len - s->output_offset > MODEM_SERIAL_LIMIT / 2 ||
            e->rx->len >= MODEM_NET_LIMIT ||
            (e->udp && e->packets.length >= MODEM_UDP_PACKETS)) {
            continue;
        }
        uint8_t bytes[65536];
        Error *err = NULL;
        size_t limit =
            e->udp ? sizeof(bytes) : MIN(4096, MODEM_NET_LIMIT - e->rx->len);
        ssize_t n = qio_channel_read(QIO_CHANNEL(e->socket), (char *)bytes,
                                     limit, &err);
        error_free(err);
        if (n == QIO_CHANNEL_ERR_BLOCK) {
            continue;
        }
        if (n < 0 || (!n && !e->udp)) {
            /* Preserve bytes already received until the dialect drains them. */
            qio_channel_close(QIO_CHANNEL(e->socket), NULL);
            object_unref(OBJECT(e->socket));
            e->socket = NULL;
            MODEM_CLASS(s)->closed(s, i);
            continue;
        }
        if (e->udp) {
            if (n > MODEM_CLASS(s)->max_udp_receive) {
                error_report("Modem unsupported UDP datagram length: %zd", n);
                disconnect(s, i);
                continue;
            }
            g_queue_push_tail(&e->packets, g_bytes_new(bytes, n));
        } else {
            g_byte_array_append(e->rx, bytes, n);
        }
        if (!e->notified) {
            e->notified = true;
            MODEM_CLASS(s)->readable(s, i);
        }
    }
    timer_mod(s->network_timer, now + 2);
}

bool listenai_modem_resolve(ListenAIModem *s, const char *host,
                            unsigned timeout_ms)
{
    if (!s->network || !s->sim || !s->pdp || s->dns_pending || !*host ||
        strlen(host) > 253 || !timeout_ms) {
        return false;
    }
    Pending *p = pending_new(s, 0);
    if (!p) {
        return false;
    }
    p->generation = ++s->dns_generation;
    g_strlcpy(p->host, host, sizeof(p->host));
    s->dns_pending = true;
    s->dns_deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout_ms;
    QIOTask *task = qio_task_new(OBJECT(s), dns_done, p, pending_free);
    qio_task_run_in_thread(task, dns_worker, p, NULL, NULL);
    return true;
}

bool listenai_modem_open(ListenAIModem *s, unsigned id, const char *host,
                         unsigned port, bool udp, unsigned timeout_ms)
{
    if (id >= MODEM_CLASS(s)->socket_count || !s->network || !s->sim ||
        !s->pdp || !*host || strlen(host) > 253 || !port || port > 65535 ||
        !timeout_ms) {
        return false;
    }
    ListenAIModemEndpoint *e = &s->ep[id];
    if (e->socket || e->opening) {
        return false;
    }
    Pending *p = pending_new(s, id);
    if (!p) {
        return false;
    }
    listenai_modem_close(e);
    p->generation = e->generation;
    e->udp = udp;
    e->opening = true;
    e->port = port;
    g_strlcpy(e->host, host, sizeof(e->host));
    e->deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout_ms;
    g_autofree char *service = g_strdup_printf("%u", port);
    SocketAddress addr = {.type = SOCKET_ADDRESS_TYPE_INET,
                          .u.inet = {.host = e->host,
                                     .port = service,
                                     .has_ipv4 = true,
                                     .ipv4 = true,
                                     .has_ipv6 = true,
                                     .ipv6 = false} };
    QIOChannelSocket *socket = qio_channel_socket_new();
    if (udp) {
        char local_host[] = "0.0.0.0";
        char local_port[] = "0";
        SocketAddress local = {
            .type = SOCKET_ADDRESS_TYPE_INET,
            .u.inet = {.host = local_host, .port = local_port} };
        qio_channel_socket_dgram_async(socket, &local, &addr, opened, p,
                                       pending_free, NULL);
    } else {
        qio_channel_socket_connect_async(socket, &addr, opened, p, pending_free,
                                         NULL);
    }
    object_unref(OBJECT(socket));
    return true;
}

void listenai_modem_deactivate(ListenAIModem *s)
{
    s->pdp = false;
    s->epoch++;
    s->dns_pending = false;
    if (s->payload_len) {
        s->payload_len = s->payload_used = s->payload_after = 0;
        MODEM_CLASS(s)->error(s);
    }
    for (unsigned i = 0; i < MODEM_CLASS(s)->socket_count; i++) {
        disconnect(s, i);
    }
}

bool listenai_modem_payload(ListenAIModem *s, unsigned id, unsigned length,
                            unsigned timeout_ms, const char *prompt)
{
    if (id >= MODEM_CLASS(s)->socket_count || !length || !timeout_ms ||
        length > MODEM_CLASS(s)->max_payload || s->payload_len) {
        return false;
    }
    ListenAIModemEndpoint *e = &s->ep[id];
    if (!e->socket || length > MODEM_NET_LIMIT - e->tx->len ||
        (e->udp && e->tx->len)) {
        return false;
    }
    s->payload_id = id;
    s->payload_len = length;
    s->payload_used = 0;
    s->payload_deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + timeout_ms;
    listenai_modem_output(s, prompt, strlen(prompt));
    s->payload_after = s->output->len - s->output_offset;
    return true;
}

static void receive(void *opaque, uint8_t byte, unsigned baud)
{
    ListenAIModem *s = opaque;
    if (s->failed || !baud || abs((int)baud - (int)s->baud) > s->baud / 50) {
        return;
    }
    s->rx_bytes++;
    if (s->after_cr) {
        s->after_cr = false;
        if (byte == '\n') {
            return;
        }
    }
    if (s->payload_len) {
        s->payload[s->payload_used++] = byte;
        if (s->payload_used == s->payload_len) {
            ListenAIModemEndpoint *e = &s->ep[s->payload_id];
            if (e->socket && s->payload_len <= MODEM_NET_LIMIT - e->tx->len) {
                g_byte_array_append(e->tx, s->payload, s->payload_len);
                MODEM_CLASS(s)->sent(s, s->payload_id, s->payload_len);
            } else {
                MODEM_CLASS(s)->error(s);
            }
            s->payload_len = s->payload_used = 0;
        }
        return;
    }
    if (s->echo) {
        listenai_modem_output(s, &byte, 1);
    }
    if (byte == '\r') {
        s->after_cr = true;
        s->command[s->command_len] = 0;
        if (s->line_overflow) {
            MODEM_CLASS(s)->error(s);
        } else if (s->command_len) {
            MODEM_CLASS(s)->command(s);
        }
        s->command_len = 0;
        s->line_overflow = false;
    } else if (byte != '\n') {
        if (s->command_len + 1 < sizeof(s->command)) {
            s->command[s->command_len++] = byte;
        } else {
            s->line_overflow = true;
        }
    }
}

ListenAIUARTPeer *listenai_modem_uart(DeviceState *dev)
{
    return &LISTENAI_MODEM(dev)->uart;
}

QDict *listenai_modem_snapshot(DeviceState *dev)
{
    ListenAIModem *s = LISTENAI_MODEM(dev);
    QDict *state = qdict_new();
    qdict_put_str(state, "model", object_get_typename(OBJECT(s)));
    qdict_put_bool(state, "sim_present", s->sim);
    qdict_put_bool(state, "registered", s->sim);
    qdict_put_bool(state, "pdp_active", s->pdp);
    qdict_put_bool(state, "host_network", s->network);
    qdict_put_bool(state, "serial_overflow", s->failed);
    qdict_put_int(state, "baud", s->baud);
    qdict_put_int(state, "rx_bytes", s->rx_bytes);
    qdict_put_int(state, "tx_bytes", s->tx_bytes);
    unsigned connected = 0;
    for (unsigned i = 0; s->ep && i < MODEM_CLASS(s)->socket_count; i++) {
        connected += s->ep[i].socket != NULL;
    }
    qdict_put_int(state, "connected_sockets", connected);
    return state;
}

static char *status_get(Object *obj, Error **errp)
{
    QDict *state = listenai_modem_snapshot(DEVICE(obj));
    GString *text = qobject_to_json(QOBJECT(state));
    qobject_unref(state);
    return g_string_free(text, false);
}

static bool sim_get(Object *obj, Error **errp)
{
    return LISTENAI_MODEM(obj)->sim;
}

static void sim_set(Object *obj, bool present, Error **errp)
{
    ListenAIModem *s = LISTENAI_MODEM(obj);
    if (present == s->sim) {
        return;
    }
    s->sim = present;
    /* QOM properties can be configured before realize allocates endpoints. */
    if (!s->ep) {
        return;
    }
    listenai_modem_deactivate(s);
    if (MODEM_CLASS(s)->sim_changed) {
        MODEM_CLASS(s)->sim_changed(s);
    }
}

static void reset(DeviceState *dev)
{
    ListenAIModem *s = LISTENAI_MODEM(dev);
    s->epoch++;
    for (unsigned i = 0; i < MODEM_CLASS(s)->socket_count; i++) {
        listenai_modem_close(&s->ep[i]);
    }
    timer_del(s->serial_timer);
    g_byte_array_set_size(s->output, 0);
    s->output_offset = s->pending_baud = s->baud_after = 0;
    s->command_len = s->payload_len = s->payload_used = s->payload_after = 0;
    s->line_overflow = s->failed = s->dns_pending = s->after_cr = false;
    s->baud = MODEM_CLASS(s)->initial_baud;
    s->echo = true;
    s->pdp = false;
    MODEM_CLASS(s)->reset(s);
}

static void realize(DeviceState *dev, Error **errp)
{
    ListenAIModem *s = LISTENAI_MODEM(dev);
    ListenAIModemClass *mc = MODEM_CLASS(s);
    assert(mc->socket_count && mc->max_payload && mc->max_udp_receive &&
           mc->initial_baud && mc->command && mc->error && mc->opened &&
           mc->closed && mc->resolved && mc->readable && mc->sent && mc->reset);
    s->ep = g_new0(ListenAIModemEndpoint, mc->socket_count);
    s->payload = g_malloc(mc->max_payload);
    for (unsigned i = 0; i < MODEM_CLASS(s)->socket_count; i++) {
        s->ep[i].rx = g_byte_array_new();
        s->ep[i].tx = g_byte_array_new();
        g_queue_init(&s->ep[i].packets);
    }
    reset(dev);
    timer_mod(s->network_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2);
}

static void init(Object *obj)
{
    ListenAIModem *s = LISTENAI_MODEM(obj);
    s->sim = true;
    s->uart.opaque = s;
    s->uart.receive = receive;
    s->output = g_byte_array_new();
    s->serial_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, serial_tick, s);
    s->network_timer = timer_new_ms(QEMU_CLOCK_REALTIME, network_tick, s);
    object_property_add_str(obj, "status", status_get, NULL);
    object_property_add_bool(obj, "sim-present", sim_get, sim_set);
}

static void finalize(Object *obj)
{
    ListenAIModem *s = LISTENAI_MODEM(obj);
    timer_free(s->serial_timer);
    timer_free(s->network_timer);
    if (s->ep) {
        for (unsigned i = 0; i < MODEM_CLASS(s)->socket_count; i++) {
            listenai_modem_close(&s->ep[i]);
            g_byte_array_unref(s->ep[i].rx);
            g_byte_array_unref(s->ep[i].tx);
        }
    }
    g_free(s->ep);
    g_free(s->payload);
    g_byte_array_unref(s->output);
}

static const Property properties[] = {
    DEFINE_PROP_BOOL("host-network", ListenAIModem, network, false),
};

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = realize;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, properties);
    dc->desc = "Abstract AT modem transport";
}

static const TypeInfo type = {
    .name = TYPE_LISTENAI_MODEM,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ListenAIModem),
    .class_size = sizeof(ListenAIModemClass),
    .abstract = true,
    .instance_init = init,
    .instance_finalize = finalize,
    .class_init = class_init,
};
static void register_types(void)
{
    type_register_static(&type);
}
type_init(register_types)
