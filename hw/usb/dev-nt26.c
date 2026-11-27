/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NT26-KCN B hardware manual Rev3.3, sections 3.5/3.6; Q-series AT Rev2.16.
 * Functional ECM composition: the NT26 SDK identifies 3505:0003, AT interface
 * 3, vendor subclass 0 and CDC Ethernet. Only these functions are exposed. */
#include "qemu/osdep.h"
#include <gmodule.h>
#include "hw/usb.h"
#include "hw/usb/nt26.h"
#include "desc.h"
#include "hw/irq.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qobject/qjson.h"
#include "qobject/qbool.h"
#include "qobject/qnum.h"
#include "qemu/module.h"

#define ECM_NOTIFY 1
#define ECM_DATA 2
#define AT_DATA 3
#define ETHER_MAX 1514
#define HS_PACKET 512

typedef struct NT26State {
    USBDevice parent;
    bool sim, powered, key_low, started_on_press, dtr, sleeping, echo;
    bool host_network, active, notify, rebooting, rx_zlp;
    uint8_t sleep_mode, dial_type;
    bool net_urc, physical_ri;
    unsigned pulse_ms, urc_delay_ms, urc_threshold, urc_count, urc_lost;
    struct {
        bool connected;
        int64_t due;
    } urc_queue[16];
    unsigned log_ctrl, log_port;
    uint16_t filter;
    QEMUTimer *power_timer, *restart, *urc_timer, *ri_end;
    int64_t press_ns;
    qemu_irq ri;
    GModule *library;
    void *context;
    void *(*create)(int);
    void (*destroy)(void *);
    int (*input)(void *, const uint8_t *, int);
    int (*receive)(void *, uint8_t *, int);
    int (*pump)(void *, int64_t);
    uint64_t (*drops)(void *);
    char line[256];
    bool bad_line;
    uint8_t response[8192], tx[ETHER_MAX], rx[ETHER_MAX];
    size_t line_used, response_used, tx_used, rx_used, rx_pos;
    uint64_t generation, frames_in, frames_out, errors;
} NT26State;
#define TYPE_NT26 TYPE_LIERDA_NT26
OBJECT_DECLARE_SIMPLE_TYPE(NT26State, NT26)

static USBDescEndpoint notify_ep[] = {
    {.bEndpointAddress = 0x80 | ECM_NOTIFY, .bmAttributes = 3,
     .wMaxPacketSize = 16, .bInterval = 8},
};
#define BULK_PAIR(ep) { \
    {.bEndpointAddress = 0x80 | (ep), .bmAttributes = 2, .wMaxPacketSize = HS_PACKET}, \
    {.bEndpointAddress = (ep), .bmAttributes = 2, .wMaxPacketSize = HS_PACKET} }
static USBDescEndpoint ecm_ep[] = BULK_PAIR(ECM_DATA);
static USBDescEndpoint at_ep[] = BULK_PAIR(AT_DATA);
static USBDescOther cdc[] = {
    {.data = (const uint8_t[]){5, 0x24, 0, 0x10, 1}}, /* CDC 1.10 */
    {.data = (const uint8_t[]){5, 0x24, 6, 0, 1}},    /* Union */
    {.data = (const uint8_t[]){13, 0x24, 15, 3, 0, 0, 0, 0,
                              ETHER_MAX & 255, ETHER_MAX >> 8, 0, 0, 0}},
};
static const USBDescIface interfaces[] = {
    {.bInterfaceNumber = 0, .bInterfaceClass = 2, .bInterfaceSubClass = 6,
     .ndesc = ARRAY_SIZE(cdc), .descs = cdc, .bNumEndpoints = 1, .eps = notify_ep},
    {.bInterfaceNumber = 1, .bInterfaceClass = 10},
    {.bInterfaceNumber = 1, .bAlternateSetting = 1, .bInterfaceClass = 10,
     .bNumEndpoints = 2, .eps = ecm_ep},
    /* Reserved interface; no diagnostic stream model. */
    {.bInterfaceNumber = 2, .bInterfaceClass = 0xff, .bInterfaceSubClass = 0xff},
    {.bInterfaceNumber = 3, .bInterfaceClass = 0xff, .bNumEndpoints = 2, .eps = at_ep},
};
static const USBDescDevice highspeed = {
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = (USBDescConfig[]){{.bNumInterfaces = 4, .bConfigurationValue = 1,
        .bmAttributes = 0xc0, .nif = ARRAY_SIZE(interfaces), .ifs = interfaces}},
};
static const USBDesc descriptor = {
    .id = {.idVendor = 0x3505, .idProduct = 3, .iManufacturer = 1, .iProduct = 2},
    .high = &highspeed,
    .str = (const char *const []){[1] = "Lierda", [2] = "NT26K0B1", [3] = "020000000026"},
};

static void end_ri(void *opaque)
{
    NT26State *s = opaque;
    qemu_irq_raise(s->ri);
}

static void deliver_urc(void *opaque)
{
    NT26State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    while (s->urc_count && s->urc_queue[0].due <= now) {
        const char *msg = s->urc_queue[0].connected ? "\r\n+QNETDEVSTATUS: 1\r\n" :
                                                    "\r\n+QNETDEVSTATUS: 0\r\n";
        size_t len = strlen(msg);
        if (s->response_used + len > sizeof(s->response)) {
            s->urc_lost++;
        } else {
            memcpy(s->response + s->response_used, msg, len);
            s->response_used += len;
        }
        s->urc_count--;
        memmove(s->urc_queue, s->urc_queue + 1, s->urc_count * sizeof(s->urc_queue[0]));
    }
    if (s->urc_count) {
        timer_mod(s->urc_timer, s->urc_queue[0].due);
    }
}

static void link_changed(NT26State *s, bool active)
{
    if (s->active == active) {
        return;
    }
    s->active = active;
    s->notify = true;
    if (!s->net_urc || !s->powered || s->rebooting) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->physical_ri && s->pulse_ms) {
        qemu_irq_lower(s->ri);
        timer_mod(s->ri_end, now + (int64_t)s->pulse_ms * 1000000);
    }
    if (s->urc_count == ARRAY_SIZE(s->urc_queue)) {
        s->urc_lost++;
        return;
    }
    unsigned i = s->urc_count++;
    s->urc_queue[i].connected = active;
    s->urc_queue[i].due = now + (int64_t)s->urc_delay_ms * 1000000;
    /* Output order follows the original transitions even if delay changes. */
    if (i && s->urc_queue[i].due < s->urc_queue[i - 1].due) {
        s->urc_queue[i].due = s->urc_queue[i - 1].due;
    }
    timer_mod(s->urc_timer, s->urc_queue[0].due);
}

static void stop_link(NT26State *s)
{
    link_changed(s, false);
    if (s->context) {
        s->destroy(s->context);
        s->context = NULL;
    }
    s->tx_used = s->rx_pos = s->rx_used = 0;
    s->rx_zlp = false;
}

static bool start_link(NT26State *s)
{
    if (!s->sim) {
        return false;
    }
    if (!s->active && s->host_network) {
        s->context = s->create(!g_strcmp0(getenv("ARCS_QEMU_NETWORK_LOOPBACK"), "1"));
        if (!s->context) {
            s->errors++;
            return false;
        }
    }
    link_changed(s, true);
    return true;
}

static void transport_reset(USBDevice *dev)
{
    NT26State *s = NT26(dev);
    s->generation++;
    /* A new USB generation must not receive Ethernet data from old sockets.
     * Keep the explicit run-level permission, retire the actual bridge. */
    if (s->context) {
        s->destroy(s->context);
        s->context = NULL;
    }
    if (s->active && s->host_network && s->powered && !s->rebooting && dev->attached) {
        s->context = s->create(!g_strcmp0(getenv("ARCS_QEMU_NETWORK_LOOPBACK"), "1"));
        if (!s->context) {
            s->active = false;
            s->errors++;
        }
    }
    timer_del(s->urc_timer);
    timer_del(s->ri_end);
    s->urc_count = 0;
    qemu_irq_raise(s->ri);
    usb_desc_attach(dev);
    s->bad_line = false;
    s->line_used = s->response_used = 0;
    s->filter = 0;
    s->tx_used = s->rx_used = s->rx_pos = 0;
    s->rx_zlp = false;
    s->notify = true;
}

static void update_attach(NT26State *s)
{
    USBDevice *dev = USB_DEVICE(s);
    bool attached = s->powered && !s->sleeping && !s->rebooting;
    if (attached && !dev->attached) {
        s->generation++;
        usb_device_attach(dev, &error_fatal);
    } else if (!attached && dev->attached) {
        usb_device_detach(dev);
        transport_reset(dev);
    }
}

static void power_on(void *opaque)
{
    NT26State *s = opaque;
    s->powered = true;
    s->started_on_press = true;
    s->echo = true;
    qemu_irq_raise(s->ri); /* MAIN_RI is idle high for the pulse configuration. */
    update_attach(s);
}

static void pwrkey(void *opaque, int line, int level)
{
    NT26State *s = opaque;
    bool low = !level;
    if (low == s->key_low) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->key_low = low;
    if (low) {
        s->press_ns = now;
        s->started_on_press = false;
        if (!s->powered) {
            timer_mod(s->power_timer, now + NANOSECONDS_PER_SECOND);
        }
    } else {
        timer_del(s->power_timer);
        if (s->powered && !s->started_on_press && now - s->press_ns >= 650000000) {
            s->powered = s->sleeping = s->rebooting = false;
            s->sleep_mode = 0;
            timer_del(s->restart);
            stop_link(s);
            update_attach(s);
        }
    }
}

static void dtr(void *opaque, int line, int level)
{
    NT26State *s = opaque;
    s->dtr = !!level;
    s->sleeping = s->sleep_mode && s->dtr;
    update_attach(s);
}

static void restart_tick(void *opaque)
{
    NT26State *s = opaque;
    s->rebooting = !s->rebooting;
    if (s->rebooting) {
        stop_link(s);
        s->sleep_mode = 0;
        s->sleeping = false;
        s->echo = true;
        timer_mod(s->restart, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
    }
    update_attach(s);
}

static const char *at_command(NT26State *s, const char *cmd, char *buf, size_t len)
{
    if (!strcmp(cmd, "AT")) {
        return "";
    }
    if (!strcmp(cmd, "ATE0") || !strcmp(cmd, "ATE1")) {
        s->echo = cmd[3] == '1';
        return "";
    }
    if (!strcmp(cmd, "AT+CGMI")) {
        return "Lierda";
    }
    if (!strcmp(cmd, "AT+CGMM")) {
        return "NT26K0B1";
    }
    if (!strcmp(cmd, "AT+CGMR")) {
        return "NT26K0B1 Lisem ECM subset";
    }
    if (!strcmp(cmd, "AT+CPIN?")) {
        return s->sim ? "+CPIN: READY" : "+CME ERROR: 10";
    }
    if (!strcmp(cmd, "AT+CEREG?") || !strcmp(cmd, "AT+CREG?")) {
        snprintf(buf, len, "%s: 0,%u", cmd[4] == 'E' ? "+CEREG" : "+CREG", s->sim);
        return buf;
    }
    if (!strcmp(cmd, "AT+CGATT?")) {
        return s->sim ? "+CGATT: 1" : "+CGATT: 0";
    }
    if (!strcmp(cmd, "AT+CSQ")) {
        return s->sim ? "+CSQ: 20,99" : "+CSQ: 99,99";
    }
    if (!strcmp(cmd, "AT+QCFG=\"usbnet\"")) {
        return "+QCFG: \"usbnet\",1";
    }
    if (!strcmp(cmd, "AT+QCFG=\"nat\"")) {
        return "+QCFG: \"nat\",1";
    }
    if (!strcmp(cmd, "AT+QCFG=\"usbnet\",1") || !strcmp(cmd, "AT+QCFG=\"nat\",1") ||
        !strcmp(cmd, "AT+ECSCLKEX=0")) {
        return "";
    }
    if (!strcmp(cmd, "AT+QSCLK=0") || !strcmp(cmd, "AT+QSCLK=1")) {
        s->sleep_mode = cmd[strlen(cmd) - 1] - '0';
        return "";
    }
    if (!strcmp(cmd, "AT+QNETDEVCTL?")) {
        snprintf(buf, len, "+QNETDEVCTL: %u,1,%u,%u", s->dial_type, s->net_urc, s->active);
        return buf;
    }
    if (g_str_has_prefix(cmd, "AT+QNETDEVCTL=")) {
        const char *args = cmd + strlen("AT+QNETDEVCTL=");
        if (strlen(args) != 5 || args[1] != ',' || args[2] != '1' || args[3] != ',' ||
            !strchr("013", args[0]) || !strchr("01", args[4])) {
            return "ERROR";
        }
        if (args[0] != '0' && !s->sim) {
            return "+CME ERROR: 10";
        }
        s->dial_type = args[0] - '0';
        s->net_urc = args[4] == '1';
        if (s->dial_type == 0) {
            stop_link(s);
            return "";
        }
        return start_link(s) ? "" : "ERROR";
    }
    if (!strcmp(cmd, "AT+QCFG=\"risignaltype\",\"physical\"")) {
        s->physical_ri = true;
        return "";
    }
    if (!strcmp(cmd, "AT+QCFG=\"urc/ri/other\",\"off\"")) {
        s->pulse_ms = 0;
        return "";
    }
    const char *pulse = "AT+QCFG=\"urc/ri/other\",\"pulse\",";
    unsigned value;
    const char *end;
    if (g_str_has_prefix(cmd, pulse) && !qemu_strtoui(cmd + strlen(pulse), &end, 10, &value) &&
        !strcmp(end, ",1") && value >= 5 && value <= 2000) {
        s->pulse_ms = value;
        return "";
    }
    const char *delay = "AT+QCFG=\"urc/delay\",";
    if (g_str_has_prefix(cmd, delay) && !qemu_strtoui(cmd + strlen(delay), NULL, 10, &value) &&
        value <= 120) {
        s->urc_delay_ms = value;
        return "";
    }
    const char *threshold = "AT+QICFG=\"buff/urcthresh\",";
    if (g_str_has_prefix(cmd, threshold) &&
        !qemu_strtoui(cmd + strlen(threshold), NULL, 10, &value) && value <= 65535) {
        s->urc_threshold = value;
        return "";
    }
    if (!strcmp(cmd, "AT+ECPCFG?")) {
        snprintf(buf, len, "+ECPCFG: \"logCtrl\":%u\r\n+ECPCFG: \"logPortSel\":%u",
                 s->log_ctrl, s->log_port);
        return buf;
    }
    const char *settings[] = {"AT+ECPCFG=\"logCtrl\",", "AT+ECPCFG=\"logPortSel\","};
    for (unsigned i = 0; i < ARRAY_SIZE(settings); i++) {
        if (g_str_has_prefix(cmd, settings[i])) {
            const char *v = cmd + strlen(settings[i]);
            if (strlen(v) == 1 && *v >= '0' && *v <= '2') {
                *(i ? &s->log_port : &s->log_ctrl) = *v - '0';
                return "";
            }
        }
    }
    if (!strcmp(cmd, "AT+CFUN=1,1")) {
        timer_mod(s->restart, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000000);
        return "";
    }
    return "ERROR";
}

static void at_data(NT26State *s, USBPacket *p)
{
    if (p->pid == USB_TOKEN_IN) {
        if (!s->response_used) {
            p->status = USB_RET_NAK;
            return;
        }
        size_t n = MIN(s->response_used, p->iov.size);
        usb_packet_copy(p, s->response, n);
        s->response_used -= n;
        memmove(s->response, s->response + n, s->response_used);
        return;
    }
    /* Reserve room for every terminator, echo and response before consuming. */
    uint8_t bytes[HS_PACKET];
    size_t n = p->iov.size;
    if (n > sizeof(bytes)) {
        p->status = USB_RET_STALL;
        return;
    }
    qemu_iovec_to_buf(&p->iov, 0, bytes, n);
    /* A one-byte malformed command plus its delimiter yields at most 12
     * response bytes. Longer query responses require longer command lines. */
    if (n && s->response_used + s->line_used + 128 + n * 12 > sizeof(s->response)) {
        p->status = USB_RET_NAK;
        return;
    }
    usb_packet_skip(p, n);
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] != '\r' && bytes[i] != '\n') {
            if (bytes[i] < 32 || bytes[i] > 126 || s->line_used == sizeof(s->line) - 1) {
                s->bad_line = true;
            } else {
                s->line[s->line_used++] = bytes[i];
            }
            continue;
        }
        if (!s->line_used && !s->bad_line) {
            continue;
        }
        char buf[96];
        s->line[s->line_used] = 0;
        bool echo = s->echo;
        const char *reply = s->bad_line ? "ERROR" : at_command(s, s->line, buf, sizeof(buf));
        bool error = !strcmp(reply, "ERROR") || g_str_has_prefix(reply, "+CME ERROR");
        size_t free = sizeof(s->response) - s->response_used;
        int used = snprintf((char *)s->response + s->response_used, free,
                            "%s%s\r\n%s%s%s\r\n", echo ? s->line : "", echo ? "\r" : "",
                            reply, *reply && !error ? "\r\n" : "", error ? "" : "OK");
        assert(used >= 0 && (size_t)used < free);
        s->response_used += used;
        s->line_used = 0;
        s->bad_line = false;
    }
}

static void data(USBDevice *dev, USBPacket *p)
{
    NT26State *s = NT26(dev);
    if (!dev->configuration) {
        p->status = USB_RET_STALL;
        return;
    }
    if (p->ep->nr == AT_DATA) {
        at_data(s, p);
        return;
    }
    if (p->ep->nr == ECM_NOTIFY && p->pid == USB_TOKEN_IN) {
        if (s->notify && p->iov.size >= 8) {
            uint8_t event[8] = {0xa1, 0, s->active, 0, 0, 0, 0, 0};
            usb_packet_copy(p, event, sizeof(event));
            s->notify = false;
        } else {
            p->status = USB_RET_NAK;
        }
        return;
    }
    if (p->ep->nr != ECM_DATA || !dev->altsetting[1]) {
        p->status = USB_RET_STALL;
        return;
    }
    if (!s->active || !s->context) {
        /* The internal USB link can remain up while the external route is
         * disconnected. Consume outgoing USB packets so a pending DMA/PIO
         * token cannot be replayed into a subsequently enabled bridge. */
        if (p->pid == USB_TOKEN_IN) {
            p->status = USB_RET_NAK;
        } else {
            usb_packet_skip(p, p->iov.size);
        }
        return;
    }
    if (s->pump(s->context, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) < 0) {
        goto error;
    }
    if (p->pid == USB_TOKEN_OUT) {
        size_t n = p->iov.size;
        if (n > sizeof(s->tx) - s->tx_used) {
            goto error;
        }
        usb_packet_copy(p, s->tx + s->tx_used, n);
        s->tx_used += n;
        if (n < HS_PACKET) {
            if (s->tx_used < 14 || s->input(s->context, s->tx, s->tx_used) < 0) {
                goto error;
            }
            s->frames_out++;
            s->tx_used = 0;
        }
        return;
    }
    if (!s->rx_used && !s->rx_zlp) {
        int len = s->receive(s->context, s->rx, sizeof(s->rx));
        if (len < 0) {
            goto error;
        }
        if (!len) {
            p->status = USB_RET_NAK;
            return;
        }
        const uint8_t mac[] = {2, 0, 0, 0, 0, 0x26};
        bool broadcast = !memcmp(s->rx, "\xff\xff\xff\xff\xff\xff", 6);
        bool directed = !memcmp(s->rx, mac, 6);
        bool multicast = !broadcast && (s->rx[0] & 1);
        if (!(s->filter & 1) && !(broadcast && (s->filter & 8)) &&
            !(directed && (s->filter & 4)) && !(multicast && (s->filter & 2))) {
            p->status = USB_RET_NAK;
            return;
        }
        s->rx_used = len;
        s->rx_zlp = len % HS_PACKET == 0;
        s->frames_in++;
    }
    size_t n = MIN(p->iov.size, s->rx_used - s->rx_pos);
    usb_packet_copy(p, s->rx + s->rx_pos, n);
    s->rx_pos += n;
    if (s->rx_pos == s->rx_used) {
        s->rx_used = s->rx_pos = 0;
        if (!n) {
            s->rx_zlp = false;
        }
    }
    return;
error:
    s->errors++;
    s->tx_used = 0;
    p->status = USB_RET_STALL;
}

static void control(USBDevice *dev, USBPacket *p, int request, int value,
                    int index, int len, uint8_t *buf)
{
    NT26State *s = NT26(dev);
    if (request == 0x2143 && !index && !len && !(value & ~31) && dev->configuration) {
        s->filter = value;
        return;
    }
    if ((request == 0x0009 && value > 1) ||
        usb_desc_handle_control(dev, p, request, value, index, len, buf) < 0) {
        p->status = USB_RET_STALL;
    }
}

QDict *nt26_snapshot(DeviceState *dev)
{
    NT26State *s = NT26(dev);
    QDict *d = qdict_new();
    qdict_put_str(d, "model", TYPE_LIERDA_NT26);
    qdict_put_str(d, "transport", "usb");
    qdict_put_bool(d, "powered", s->powered);
    qdict_put_bool(d, "usb_attached", USB_DEVICE(s)->attached);
    qdict_put_bool(d, "usb_configured", !!USB_DEVICE(s)->configuration);
    qdict_put_bool(d, "sim_present", s->sim);
    qdict_put_bool(d, "registered", s->powered && !s->rebooting && s->sim);
    qdict_put_bool(d, "pdp_active", s->active);
    qdict_put_bool(d, "host_network", s->host_network);
    qdict_put_bool(d, "network_enabled", s->host_network);
    qdict_put_int(d, "usb_generation", s->generation);
    qdict_put_bool(d, "dtr", s->dtr);
    qdict_put_bool(d, "pwrkey", !s->key_low);
    qdict_put_int(d, "ecm_tx_frames", s->frames_out);
    qdict_put_int(d, "ecm_rx_frames", s->frames_in);
    qdict_put_int(d, "ecm_errors", s->errors);
    qdict_put_int(d, "urc_lost", s->urc_lost);
    qdict_put_int(d, "bridge_drops", s->context ? s->drops(s->context) : 0);
    return d;
}

static char *get_status(Object *obj, Error **errp)
{
    QDict *d = nt26_snapshot(DEVICE(obj));
    GString *json = qobject_to_json(QOBJECT(d));
    qobject_unref(d);
    return g_string_free(json, false);
}

static bool load_bridge(NT26State *s, Error **errp)
{
    if (s->library) {
        return true;
    }
    const char *path = getenv("LISEM_MODEM_NETWORK_LIBRARY");
    if (!path || !(s->library = g_module_open(path, G_MODULE_BIND_LOCAL))) {
        error_setg(errp, "NT26 host networking requires the packaged Ethernet bridge");
        return false;
    }
#define BIND(field, symbol) \
    if (!g_module_symbol(s->library, "arcs_net_" symbol, (gpointer *)&s->field)) { \
        error_setg(errp, "Missing NT26 bridge symbol arcs_net_" symbol); \
        g_module_close(s->library); s->library = NULL; return false; \
    }
    BIND(create, "create");
    BIND(destroy, "destroy");
    BIND(input, "input");
    BIND(receive, "receive");
    BIND(pump, "pump");
    BIND(drops, "dropped");
#undef BIND
    return true;
}

static void change(NT26State *s, QDict *d, Error **errp)
{
    QNum *gen = d ? qobject_to(QNum, qdict_get(d, "usb_generation")) : NULL;
    uint64_t generation;
    if (!gen || !qnum_get_try_uint(gen, &generation) || generation != s->generation) {
        error_setg(errp, "Unknown or stale NT26 USB generation");
        return;
    }
    QBool *sim = qobject_to(QBool, qdict_get(d, "sim_present"));
    QBool *network = qobject_to(QBool, qdict_get(d, "network_enabled"));
    if (qdict_size(d) != 2 || (!sim && !network)) {
        error_setg(errp, "NT26 control requires one boolean SIM or bridge change");
        return;
    }
    if (sim) {
        bool present = qbool_get_bool(sim);
        if (present == s->sim) {
            return;
        }
        s->sim = present;
        if (!s->sim) {
            stop_link(s);
        } else if (s->dial_type == 3 && s->powered && !s->rebooting) {
            start_link(s);
        }
        return;
    }
    bool enable = qbool_get_bool(network);
    if (enable == s->host_network) {
        return;
    }
    if (enable && !load_bridge(s, errp)) {
        return;
    }
    if (enable && s->active) {
        s->context = s->create(!g_strcmp0(getenv("ARCS_QEMU_NETWORK_LOOPBACK"), "1"));
        if (!s->context) {
            error_setg(errp, "Failed to initialize NT26 Ethernet bridge");
            return;
        }
    } else if (!enable && s->context) {
        s->destroy(s->context);
        s->context = NULL;
    }
    s->rx_used = s->rx_pos = s->tx_used = 0;
    s->rx_zlp = false;
    s->host_network = enable;
}

static void run_control(Object *obj, const char *json, Error **errp)
{
    if (strlen(json) > 256) {
        error_setg(errp, "NT26 control payload too large");
        return;
    }
    QObject *root = qobject_from_json(json, errp);
    if (root) {
        change(NT26(obj), qobject_to(QDict, root), errp);
        qobject_unref(root);
    }
}

static void realize(USBDevice *dev, Error **errp)
{
    NT26State *s = NT26(dev);
    s->power_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, power_on, s);
    s->urc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, deliver_urc, s);
    s->ri_end = timer_new_ns(QEMU_CLOCK_VIRTUAL, end_ri, s);
    s->restart = timer_new_ns(QEMU_CLOCK_VIRTUAL, restart_tick, s);
    usb_desc_init(dev);
    dev->auto_attach = false;
}

static void unrealize(USBDevice *dev)
{
    NT26State *s = NT26(dev);
    stop_link(s);
    timer_free(s->power_timer);
    timer_free(s->restart);
    timer_free(s->urc_timer);
    timer_free(s->ri_end);
    if (s->library) {
        g_module_close(s->library);
    }
}

static void init(Object *obj)
{
    NT26State *s = NT26(obj);
    s->sim = true;
    s->urc_delay_ms = 120;
    object_property_add_str(obj, "status", get_status, NULL);
    object_property_add_str(obj, "run-control", NULL, run_control);
    qdev_init_gpio_in_named(DEVICE(obj), dtr, "dtr", 1);
    qdev_init_gpio_in_named(DEVICE(obj), pwrkey, "pwrkey", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->ri, "ri", 1);
}

static void class_init(ObjectClass *klass, const void *data_ptr)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    uc->realize = realize;
    uc->unrealize = unrealize;
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = transport_reset;
    uc->handle_control = control;
    uc->handle_data = data;
    uc->product_desc = "Lierda NT26K0B1";
    uc->usb_desc = &descriptor;

}

static const TypeInfo info = {
    .name = TYPE_LIERDA_NT26, .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(NT26State), .instance_init = init, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
