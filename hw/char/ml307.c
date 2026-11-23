/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ML307C AT dialect. Host transport and UART framing live in listenai_modem. */
#include "qemu/osdep.h"
#include "hw/char/listenai_modem.h"
#include "qemu/module.h"

#define TYPE_ML307 "ml307c-gc-cn"
#define SOCKETS 5
OBJECT_DECLARE_SIMPLE_TYPE(ML307State, ML307)

struct ML307State {
    ListenAIModem parent_obj;
    unsigned cereg;
};

/*
 * CSV for command arguments only. Payload bytes never pass through this parser.
 * Strict quoting and numeric validation avoid accepting unsupported dialects.
 */
static int arguments(char *text, char **fields, unsigned capacity)
{
    unsigned count = 0;
    while (true) {
        if (count == capacity) {
            return -1;
        }
        if (*text == '"') {
            fields[count++] = ++text;
            text = strchr(text, '"');
            if (!text) {
                return -1;
            }
            *text++ = 0;
            if (!*text) {
                return count;
            }
            if (*text++ != ',') {
                return -1;
            }
        } else {
            fields[count++] = text;
            char *end = strchr(text, ',');
            if (!end) {
                return count;
            }
            *end = 0;
            text = end + 1;
        }
    }
}

static bool uint_arg(const char *text, unsigned maximum, unsigned *result)
{
    if (!*text) {
        return false;
    }
    uint64_t n = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        n = n * 10 + *text - '0';
        if (n > maximum) {
            return false;
        }
    }
    *result = n;
    return true;
}

static void execute(ListenAIModem *s)
{
    char *cmd = s->command;
    char *fields[8];
    unsigned id, amount;
    int n;
    if (!strcmp(cmd, "AT")) {
        listenai_modem_reply(s, "\r\nOK\r\n");
    } else if (!strcmp(cmd, "ATE0") || !strcmp(cmd, "ATE1")) {
        s->echo = cmd[3] == '1';
        listenai_modem_reply(s, "\r\nOK\r\n");
    } else if (!strcmp(cmd, "AT+CGMM") || !strcmp(cmd, "ATI")) {
        listenai_modem_reply(s, "\r\nML307C-GC-CN\r\nOK\r\n");
    } else if (!strcmp(cmd, "AT+CGMI")) {
        listenai_modem_reply(s, "\r\nChina Mobile\r\nOK\r\n");
    } else if (!strcmp(cmd, "AT+CGMR")) {
        listenai_modem_reply(s, "\r\nLisem ML307C AT subset 1\r\nOK\r\n");
    } else if (g_str_has_prefix(cmd, "AT+IPR=") &&
               uint_arg(cmd + 7, 921600, &amount) &&
               (amount == 115200 || amount == 460800 || amount == 921600)) {
        listenai_modem_reply(s, "\r\nOK\r\n");
        s->pending_baud = amount;
        s->baud_after = s->output->len - s->output_offset;
    } else if (!strcmp(cmd, "AT+CPIN?")) {
        listenai_modem_reply(s, s->sim ? "\r\n+CPIN: READY\r\nOK\r\n"
                                       : "\r\n+CME ERROR: 10\r\n");
    } else if (g_str_has_prefix(cmd, "AT+CEREG=") &&
               uint_arg(cmd + 9, 2, &amount)) {
        ML307(s)->cereg = amount;
        listenai_modem_reply(s, "\r\nOK\r\n");
    } else if (!strcmp(cmd, "AT+CEREG?")) {
        listenai_modem_reply(s, "\r\n+CEREG: %u,%u\r\nOK\r\n", ML307(s)->cereg,
                             s->sim ? 1 : 0);
    } else if (!strcmp(cmd, "AT+CSQ")) {
        listenai_modem_reply(s, "\r\n+CSQ: %u,99\r\nOK\r\n", s->sim ? 25 : 99);
    } else if (!strcmp(cmd, "AT+MIPCALL?")) {
        listenai_modem_reply(s, "\r\n+MIPCALL: 1,%u,\"%s\"\r\nOK\r\n",
                             s->pdp ? 1 : 0, s->pdp ? "192.0.2.2" : "0.0.0.0");
    } else if (!strcmp(cmd, "AT+MIPCALL=1,1") && s->sim) {
        s->pdp = true;
        listenai_modem_reply(s, "\r\nOK\r\n+MIPCALL: 1,1,\"192.0.2.2\"\r\n");
    } else if (!strcmp(cmd, "AT+MIPCALL=0,1")) {
        listenai_modem_deactivate(s);
        listenai_modem_reply(s, "\r\nOK\r\n+MIPCALL: 1,0\r\n");
    } else if (g_str_has_prefix(cmd, "AT+MDNSGIP=")) {
        n = arguments(cmd + 11, fields, 8);
        if (n != 2 || strcmp(fields[1], "1") || !*fields[0] ||
            strlen(fields[0]) > 253 || !s->pdp || !s->network ||
            s->dns_pending) {
            goto invalid;
        }
        if (!listenai_modem_resolve(s, fields[0], 9000)) {
            goto invalid;
        }
    } else if (g_str_has_prefix(cmd, "AT+MIPCFG=")) {
        n = arguments(cmd + 10, fields, 8);
        if (n < 3 || !uint_arg(fields[1], SOCKETS, &id) || !id) {
            goto invalid;
        }
        if ((n == 4 && !strcmp(fields[0], "ssl") && !strcmp(fields[2], "0") &&
             !strcmp(fields[3], "0")) ||
            (n == 4 && !strcmp(fields[0], "encoding") &&
             !strcmp(fields[2], "0") && !strcmp(fields[3], "0")) ||
            (n == 3 && !strcmp(fields[0], "cid") && !strcmp(fields[2], "1"))) {
            listenai_modem_reply(s, "\r\nOK\r\n");
        } else {
            goto invalid;
        }
    } else if (g_str_has_prefix(cmd, "AT+MIPOPEN=")) {
        unsigned port;
        n = arguments(cmd + 11, fields, 8);
        if (n != 7 || !uint_arg(fields[0], SOCKETS, &id) || !id ||
            !uint_arg(fields[3], 65535, &port) || !port ||
            !uint_arg(fields[4], 120, &amount) || !amount ||
            strcmp(fields[6], "0") || !*fields[2] || strlen(fields[2]) > 253 ||
            !s->network || !s->pdp || !s->sim) {
            goto invalid;
        }
        bool udp = !strcmp(fields[1], "UDP");
        if ((!udp && strcmp(fields[1], "TCP")) ||
            strcmp(fields[5], udp ? "3" : "2")) {
            goto invalid;
        }
        ListenAIModemEndpoint *e = &s->ep[id - 1];
        if (e->socket || e->opening) {
            listenai_modem_reply(s, "\r\n+CME ERROR: 552\r\n");
            return;
        }
        if (!listenai_modem_open(s, id - 1, fields[2], port, udp,
                                 amount * 1000)) {
            goto invalid;
        }
        listenai_modem_reply(s, "\r\nOK\r\n");
    } else if (g_str_has_prefix(cmd, "AT+MIPSTATE=") &&
               uint_arg(cmd + 12, SOCKETS, &id) && id) {
        ListenAIModemEndpoint *e = &s->ep[id - 1];
        listenai_modem_reply(
            s, "\r\n+MIPSTATE: %u,\"%s\",\"%s\",%u,\"%s\"\r\nOK\r\n", id,
            e->udp ? "UDP" : "TCP", *e->host ? e->host : "0.0.0.0", e->port,
            e->socket ? "CONNECTED" : "INITIAL");
    } else if (g_str_has_prefix(cmd, "AT+MIPCLOSE=") &&
               uint_arg(cmd + 12, SOCKETS, &id) && id) {
        ListenAIModemEndpoint *e = &s->ep[id - 1];
        if (!e->socket && !e->opening) {
            listenai_modem_reply(s, "\r\n+CME ERROR: 553\r\n");
            return;
        }
        listenai_modem_close(e);
        listenai_modem_reply(s, "\r\nOK\r\n+MIPCLOSE: %u\r\n", id);
    } else if (g_str_has_prefix(cmd, "AT+MIPSEND=")) {
        n = arguments(cmd + 11, fields, 8);
        if (n != 2 || !uint_arg(fields[0], SOCKETS, &id) || !id ||
            !uint_arg(fields[1], 1460, &amount) || !amount) {
            goto invalid;
        }
        ListenAIModemEndpoint *e = &s->ep[id - 1];
        if (!e->socket || (e->udp && (amount > 730 || e->tx->len)) ||
            amount > MODEM_NET_LIMIT - e->tx->len) {
            goto invalid;
        }
        if (!listenai_modem_payload(s, id - 1, amount, 10000, "\r\n>")) {
            goto invalid;
        }
        listenai_modem_output(s, " ", 1);
    } else if (g_str_has_prefix(cmd, "AT+MIPRD=")) {
        n = arguments(cmd + 9, fields, 8);
        if (n != 2 || !uint_arg(fields[0], SOCKETS, &id) || !id ||
            !uint_arg(fields[1], 4096, &amount) || !amount) {
            goto invalid;
        }
        ListenAIModemEndpoint *e = &s->ep[id - 1];
        if (e->udp) {
            if (amount != 1) {
                goto invalid;
            }
            GBytes *packet = g_queue_pop_head(&e->packets);
            gsize length = 0;
            const void *bytes =
                packet ? g_bytes_get_data(packet, &length) : NULL;
            listenai_modem_reply(s, "\r\n+MIPRD: %u,%u,%zu,", id,
                                 e->packets.length, length);
            if (length) {
                listenai_modem_output(s, bytes, length);
            }
            if (packet) {
                g_bytes_unref(packet);
            }
            e->notified = !g_queue_is_empty(&e->packets);
        } else {
            amount = MIN(amount, e->rx->len);
            listenai_modem_reply(s, "\r\n+MIPRD: %u,%u,%u,", id,
                                 e->rx->len - amount, amount);
            listenai_modem_output(s, e->rx->data, amount);
            g_byte_array_remove_range(e->rx, 0, amount);
            e->notified = e->rx->len != 0;
        }
        listenai_modem_reply(s, "\r\nOK\r\n");
    } else {
    invalid:
        listenai_modem_reply(s, "\r\nERROR\r\n");
    }
}

static void command_error(ListenAIModem *s)
{
    listenai_modem_reply(s, "\r\nERROR\r\n");
}

static void opened(ListenAIModem *s, unsigned id, bool success)
{
    listenai_modem_reply(s, "\r\n+MIPOPEN: %u,%u\r\n", id + 1, success ? 0 : 1);
}

static void closed(ListenAIModem *s, unsigned id)
{
    listenai_modem_reply(s, "\r\n+MIPURC: \"disconn\",%u,1\r\n", id + 1);
}

static void resolved(ListenAIModem *s, const char *ip)
{
    if (ip) {
        listenai_modem_reply(s, "\r\n+MDNSGIP: 1,\"%s\"\r\nOK\r\n", ip);
    } else {
        command_error(s);
    }
}

static void readable(ListenAIModem *s, unsigned id)
{
    ListenAIModemEndpoint *e = &s->ep[id];
    listenai_modem_reply(s, "\r\n+MIPURC: \"%s\",%u,%u\r\n",
                         e->udp ? "rudp" : "rtcp", id + 1,
                         e->udp ? e->packets.length : e->rx->len);
}

static void sent(ListenAIModem *s, unsigned id, unsigned length)
{
    listenai_modem_reply(s, "\r\nOK\r\n+MIPSEND: %u,%u\r\n", id + 1, length);
}

static void reset(ListenAIModem *s)
{
    ML307(s)->cereg = 0;
}

static void class_init(ObjectClass *klass, const void *data)
{
    ListenAIModemClass *mc = LISTENAI_MODEM_CLASS(klass);
    mc->socket_count = SOCKETS;
    mc->max_payload = 1460;
    mc->max_udp_receive = 1460;
    mc->initial_baud = 115200;
    mc->command = execute;
    mc->error = command_error;
    mc->opened = opened;
    mc->closed = closed;
    mc->resolved = resolved;
    mc->readable = readable;
    mc->sent = sent;
    mc->reset = reset;
    /* Exact SIM hot-plug URCs remain unverified; queries are supported. */
    DEVICE_CLASS(klass)->desc = "ML307C-GC-CN UART AT/socket subset";
}

static const TypeInfo type = {
    .name = TYPE_ML307,
    .parent = TYPE_LISTENAI_MODEM,
    .instance_size = sizeof(ML307State),
    .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&type);
}
type_init(register_types)
