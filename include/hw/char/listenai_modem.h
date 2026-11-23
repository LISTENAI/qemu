/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CHAR_LISTENAI_MODEM_H
#define HW_CHAR_LISTENAI_MODEM_H

#include "hw/char/listenai_uart.h"
#include "hw/qdev-core.h"
#include "io/channel-socket.h"
#include "qemu/timer.h"
#include "qobject/qdict.h"

#define TYPE_LISTENAI_MODEM "listenai-at-modem"
OBJECT_DECLARE_TYPE(ListenAIModem, ListenAIModemClass, LISTENAI_MODEM)

/* Host resource bounds, independent of a dialect's command/payload limits. */
#define MODEM_NET_LIMIT 32768
#define MODEM_SERIAL_LIMIT 65536
#define MODEM_UDP_PACKETS 16

typedef struct ListenAIModemEndpoint {
    QIOChannelSocket *socket;
    uint64_t generation;
    bool opening, udp, notified;
    int64_t deadline;
    GByteArray *rx, *tx;
    GQueue packets;
    char host[256];
    unsigned port;
} ListenAIModemEndpoint;

/*
 * Shared runtime. Only the concrete dialect interprets AT commands and emits
 * wire responses. Endpoint indices here are zero-based, not wire socket IDs.
 * Dialects may drain RX queues and inspect state; host socket operations,
 * cancellation and payload delivery go through the helpers below.
 */
struct ListenAIModem {
    DeviceState parent_obj;
    ListenAIModemEndpoint *ep;
    QEMUTimer *serial_timer, *network_timer;
    GByteArray *output;
    ListenAIUARTPeer uart;
    char command[1024];
    unsigned command_len, baud, payload_id, payload_len, payload_used;
    unsigned output_offset, pending_baud, baud_after, payload_after;
    uint8_t *payload;
    bool line_overflow, echo, network, sim, pdp, failed, dns_pending, after_cr;
    unsigned tasks;
    uint64_t epoch, dns_generation, rx_bytes, tx_bytes;
    int64_t dns_deadline, payload_deadline;
};

struct ListenAIModemClass {
    DeviceClass parent_class;
    unsigned socket_count, max_payload, max_udp_receive, initial_baud;
    void (*command)(ListenAIModem *s);
    void (*error)(ListenAIModem *s);
    void (*opened)(ListenAIModem *s, unsigned id, bool success);
    void (*closed)(ListenAIModem *s, unsigned id);
    void (*resolved)(ListenAIModem *s, const char *ipv4); /* NULL on failure */
    void (*readable)(ListenAIModem *s, unsigned id);
    void (*sent)(ListenAIModem *s, unsigned id, unsigned length);
    void (*reset)(ListenAIModem *s);
    void (*sim_changed)(ListenAIModem *s); /* optional, after teardown */
};

ListenAIUARTPeer *listenai_modem_uart(DeviceState *dev);
QDict *listenai_modem_snapshot(DeviceState *dev);
void listenai_modem_output(ListenAIModem *s, const void *data, size_t len);
void listenai_modem_reply(ListenAIModem *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);
/*
 * Accepted requests complete asynchronously through class callbacks. Reset,
 * SIM changes and PDP deactivation invalidate late completions.
 */
bool listenai_modem_resolve(ListenAIModem *s, const char *host,
                            unsigned timeout_ms);
bool listenai_modem_open(ListenAIModem *s, unsigned id, const char *host,
                         unsigned port, bool udp, unsigned timeout_ms);
/* prompt is the prefix after which raw input can start (no implicit suffix). */
bool listenai_modem_payload(ListenAIModem *s, unsigned id, unsigned length,
                            unsigned timeout_ms, const char *prompt);
void listenai_modem_close(ListenAIModemEndpoint *e);
void listenai_modem_deactivate(ListenAIModem *s);

#endif
