/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDC ACM host transport. Payload is an opaque byte stream; only USB
 * descriptors, class control and FIFO packet boundaries are interpreted. */
#include "qemu/osdep.h"
#include "hw/usb/listenai_musb_host.h"
#include "qapi/error.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qemu/bswap.h"
#include "hw/irq.h"

#define CDC_TICK_NS 100000

void listenai_musb_cdc_close(ListenAIMUSBHost *h, const char *reason)
{
    h->cdc.error = reason;
    if (h->cdc.connected) {
        qemu_chr_fe_disconnect(&h->cdc.chr);
    }
}

/* Require one ACM function and its Union descriptor. Never guess fixed
 * interface or endpoint numbers, including after a firmware mode change. */
static bool endpoints(ListenAIMUSBHost *h)
{
    USBCDC *c = &h->cdc;
    int interface = -1, control = -1, data = -1;
    bool acm = false;
    for (unsigned i = 0; i < h->config_length; i += h->config[i]) {
        const uint8_t *d = h->config + i;
        if (d[1] == 4) {
            interface = d[2];
            if (d[3] == 0 && d[5] == 2 && d[6] == 2) {
                if (control >= 0) { return false; }
                control = interface;
            }
        } else if (d[1] == 0x24 && interface == control && control >= 0) {
            if (d[0] < 3) { return false; }
            if (d[2] == 2) {
                if (d[0] < 4) { return false; }
                acm = (d[3] & 2) != 0;
            } else if (d[2] == 6) {
                if (d[0] != 5 || d[3] != control || data >= 0) { return false; }
                data = d[4];
            }
        }
    }
    if (control < 0 || data < 0 || control == data || !acm) { return false; }
    c->control = control;
    bool active = false;
    for (unsigned i = 0; i < h->config_length; i += h->config[i]) {
        const uint8_t *d = h->config + i;
        if (d[1] == 4) {
            interface = d[2];
            active = d[3] == 0;
            if (interface == data && active && d[5] != 10) { return false; }
        } else if (d[1] == 5 && active) {
            unsigned ep = d[2] & 15, mps = lduw_le_p(d + 4);
            if (interface == data && (d[3] & 3) == 2) {
                if (!ep || !mps || mps > (h->high_speed ? 512 : 64)) { return false; }
                if (d[2] & 0x80) {
                    if (c->in) { return false; }
                    c->in = ep; c->in_max = mps;
                } else {
                    if (c->out) { return false; }
                    c->out = ep; c->out_max = mps;
                }
            } else if (interface == control && (d[3] & 3) == 3 && (d[2] & 0x80)) {
                if (!ep || !mps || mps > 64 || c->notification) { return false; }
                c->notification = ep;
                c->notification_max = mps;
            }
        }
    }
    return c->in && c->out && c->notification != c->in;
}

static void opened(ListenAIMUSBHost *h)
{
    h->cdc.ready = true;
    h->cdc.opening = false;
}

static void tick(void *opaque)
{
    ListenAIMUSBHost *h = opaque;
    ListenAIMUSB *s = h->controller;
    USBCDC *c = &h->cdc;
    if (!c->connected) { return; }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(c->timer, now + CDC_TICK_NS);
    if (!s->clock_enabled) { return; }
    if (!c->ready) {
        if (now >= c->deadline) { listenai_musb_cdc_close(h, "cdc-open-timeout"); return; }
        if (!h->configuration) {
            if (!h->enumerating && (s->power & 0x40)) { listenai_musb_bus_reset(h, true); }
            return;
        }
        if (!c->opening) {
            if (!endpoints(h)) { listenai_musb_cdc_close(h, "no-compatible-cdc-acm-interface"); return; }
            c->opening = true;
            /* Match enumeration's inter-transfer cadence. The device must
             * receive the SET_CONFIGURATION status-complete IRQ before a
             * new SETUP replaces EP0's FIFO and CSR state. */
            c->control_at = now + 1000000;
        } else if (c->control_at && now >= c->control_at) {
            c->control_at = 0;
            /* SET_CONTROL_LINE_STATE(DTR|RTS); completion waits for the
             * firmware's real EP0 status packet before releasing payload. */
            listenai_musb_control(h, 0x21, 0x22, 3, c->control, opened);
        }
        return;
    }
    if (c->outgoing_count) {
        int n = qemu_chr_fe_write(&c->chr, c->outgoing, c->outgoing_count);
        if (!c->connected) { return; }
        if (n > 0) {
            c->outgoing_count -= n;
            memmove(c->outgoing, c->outgoing + n, c->outgoing_count);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            listenai_musb_cdc_close(h, "socket-write-failed"); return;
        }
    }
    listenai_musb_dma_service(s);
    unsigned in = c->in, out = c->out;
    if ((s->endpoint[in][2] & 0x10) || (s->endpoint[out][6] & 0x20)) {
        listenai_musb_cdc_close(h, "endpoint-stalled"); return;
    }
    unsigned tx_max = lduw_le_p(s->endpoint[in]) & 0x7ff;
    unsigned rx_max = lduw_le_p(s->endpoint[out] + 4) & 0x7ff;
    if ((tx_max && tx_max != c->in_max) || (rx_max && rx_max != c->out_max)) {
        listenai_musb_cdc_close(h, "endpoint-max-packet-mismatch"); return;
    }
    if (s->endpoint[in][2] & 1) {
        unsigned n = s->bulk_tx_count[in];
        if (n > c->in_max) { listenai_musb_cdc_close(h, "invalid-tx-packet-length"); return; }
        if (tx_max && n <= sizeof(c->outgoing) - c->outgoing_count) {
            memcpy(c->outgoing + c->outgoing_count, s->bulk_tx[in], n);
            c->outgoing_count += n; c->bytes_in += n;
            s->bulk_tx_count[in] = 0; s->endpoint[in][2] &= ~3;
            if (!(s->endpoint[in][3] & 4)) { s->tx_pending |= 1 << in; }
            listenai_musb_irq(s);
        }
    }
    if (rx_max && c->incoming_count && !(s->endpoint[out][6] & 1)) {
        unsigned n = MIN(c->incoming_count, c->out_max);
        memcpy(s->bulk_rx[out], c->incoming, n);
        c->incoming_count -= n;
        memmove(c->incoming, c->incoming + n, c->incoming_count);
        s->bulk_count[out] = n; s->bulk_position[out] = 0;
        s->endpoint[out][6] |= 1; c->bytes_out += n;
        if (!(s->endpoint[out][7] & 8) || n < c->out_max) { s->rx_pending |= 1 << out; }
        listenai_musb_dma_service(s); listenai_musb_irq(s);
        qemu_chr_fe_accept_input(&c->chr);
    }
    unsigned ep = c->notification;
    if (ep && (s->endpoint[ep][2] & 1)) {
        /* ACM notifications are not serial payload. Acknowledge the USB
         * packet and expose a count without injecting it into the stream. */
        if (s->bulk_tx_count[ep] > c->notification_max) { listenai_musb_cdc_close(h, "invalid-notification-length"); return; }
        c->notifications++;
        s->bulk_tx_count[ep] = 0; s->endpoint[ep][2] &= ~3;
        s->tx_pending |= 1 << ep; listenai_musb_irq(s);
    }
}

static int can_read(void *opaque)
{
    ListenAIMUSBHost *h = opaque;
    return h->cdc.connected ? sizeof(h->cdc.incoming) - h->cdc.incoming_count : 0;
}

static void receive(void *opaque, const uint8_t *data, int size)
{
    USBCDC *c = &((ListenAIMUSBHost *)opaque)->cdc;
    assert(size >= 0 && size <= sizeof(c->incoming) - c->incoming_count);
    memcpy(c->incoming + c->incoming_count, data, size); c->incoming_count += size;
}

static void event(void *opaque, QEMUChrEvent event)
{
    ListenAIMUSBHost *h = opaque;
    ListenAIMUSB *s = h->controller;
    USBCDC *c = &h->cdc;
    if (event != CHR_EVENT_OPENED && event != CHR_EVENT_CLOSED) { return; }
    c->connected = event == CHR_EVENT_OPENED;
    c->incoming_count = c->outgoing_count = 0;
    h->connected = c->connected;
    qemu_set_irq(s->host_connected, h->connected);
    h->enumerating = false; h->phase = HOST_IDLE;
    h->status = h->connected ? "connected" : "disconnected";
    timer_del(h->timer); timer_del(c->timer);
    listenai_musb_descriptors_clear(h); listenai_musb_cancel_dma(s);
    /* A socket edge cancels host queues, not a packet already delivered to
     * the controller. Firmware may be between RXCOUNT and its FIFO reads.
     * The next USB bus reset clears endpoint state before re-enumeration. */
    s->interrupt |= h->connected ? 0x10 : 0x20;
    if (h->connected) {
        c->error = NULL;
        c->deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 30000000000LL;
        timer_mod(c->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CDC_TICK_NS);
    }
    listenai_musb_sof_update(h); listenai_musb_irq(s);
}

static char *get(Object *obj, Error **errp)
{
    ListenAIMUSBHost *h = LISTENAI_MUSB(obj)->host;
    USBCDC *c = &h->cdc;
    QDict *root = qdict_new();
    qdict_put_bool(root, "enabled", c->listen != NULL);
    if (c->listen) { qdict_put_str(root, "listen", c->listen); }
    else { qdict_put_null(root, "listen"); }
    qdict_put_bool(root, "connected", c->connected);
    qdict_put_bool(root, "ready", c->ready);
    if (c->error) { qdict_put_str(root, "error", c->error); }
    else { qdict_put_null(root, "error"); }
    qdict_put_str(root, "status", h->status);
    qdict_put_str(root, "speed", h->high_speed ? "high" : "full");
    qdict_put_int(root, "configuration", h->configuration);
    qdict_put_int(root, "pending_to_device", c->incoming_count);
    qdict_put_int(root, "pending_to_host", c->outgoing_count);
    qdict_put_int(root, "bytes_from_device", c->bytes_in);
    qdict_put_int(root, "bytes_to_device", c->bytes_out);
    qdict_put_int(root, "notifications", c->notifications);
    GString *json = qobject_to_json(QOBJECT(root));
    qobject_unref(root); return g_string_free(json, false);
}

static void set(Object *obj, const char *command, Error **errp)
{
    ListenAIMUSBHost *h = LISTENAI_MUSB(obj)->host;
    USBCDC *c = &h->cdc;
    if (!strcmp(command, "disable")) {
        listenai_musb_cdc_close(h, NULL);
        qemu_chr_fe_deinit(&c->chr, true);
        g_clear_pointer(&c->listen, g_free);
    } else if (!strcmp(command, "enable")) {
        if (h->controller->downstream) {
            error_setg(errp, "USB controller is wired to a board device"); return;
        }
        if (c->listen) { return; }
        if (h->connected || h->listen) { error_setg(errp, "USB host is already in use"); return; }
        Chardev *chr = qemu_chr_new("lisem-usb-cdc", "tcp:127.0.0.1:0,server=on,wait=off,nodelay=on", NULL);
        if (!chr) { error_setg(errp, "Cannot create CDC loopback listener"); return; }
        if (!qemu_chr_fe_init(&c->chr, chr, errp)) { object_unparent(OBJECT(chr)); return; }
        const char *address = strstr(chr->filename, "tcp:");
        if (!address || !strchr(address, ',')) {
            qemu_chr_fe_deinit(&c->chr, true); error_setg(errp, "Cannot resolve CDC listener address"); return;
        }
        address += 4;
        c->listen = g_strndup(address, strchr(address, ',') - address);
        c->error = NULL;
        qemu_chr_fe_set_handlers(&c->chr, can_read, receive, event, NULL, h, NULL, true);
    } else { error_setg(errp, "Unknown CDC bridge command"); }
}

void listenai_musb_cdc_init(ListenAIMUSB *s)
{
    s->host->cdc.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s->host);
    object_property_add_str(OBJECT(s), "cdc", get, set);
}

void listenai_musb_cdc_finalize(ListenAIMUSBHost *h)
{
    qemu_chr_fe_deinit(&h->cdc.chr, true);
    g_free(h->cdc.listen);
    timer_free(h->cdc.timer);
}
