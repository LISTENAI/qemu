/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CORE_LISTENAI_SIGNAL_TRACE_H
#define HW_CORE_LISTENAI_SIGNAL_TRACE_H

#include "hw/gpio/listenai_gpio.h"
#include "qom/object.h"

#define LISTENAI_TRACE_PINS 96
#define LISTENAI_TRACE_CAPACITY 4096

typedef struct ListenaiSignalEvent {
    uint64_t sequence;
    int64_t ns;
    unsigned pin;
    bool driven, level, initial;
} ListenaiSignalEvent;

typedef struct ListenaiSignalTrace ListenaiSignalTrace;
typedef struct ListenaiSignalTraceBank {
    ListenaiSignalTrace *trace;
    unsigned first;
} ListenaiSignalTraceBank;

struct ListenaiSignalTrace {
    Object *gpio_owner;
    ListenaiGPIO *gpio[2];
    DeviceState *extio;
    ListenaiSignalTraceBank banks[3];
    uint32_t valid[3];
    bool selected[LISTENAI_TRACE_PINS], seen[LISTENAI_TRACE_PINS];
    bool driven[LISTENAI_TRACE_PINS], level[LISTENAI_TRACE_PINS];
    ListenaiSignalEvent *events;
    unsigned capacity, count;
    uint64_t next;
    int64_t started_ns, ended_ns;
    char id[65];
    const char *reason;
    bool active;
};

void listenai_signal_trace_init(ListenaiSignalTrace *s, Object *owner,
                               Object *gpio_owner, ListenaiGPIO *gpio,
                               unsigned pa, unsigned pb,
                               DeviceState *extio);
void listenai_signal_trace_stop(ListenaiSignalTrace *s, const char *reason);
char *listenai_signal_trace_json(ListenaiSignalTrace *s);
void listenai_signal_trace_destroy(ListenaiSignalTrace *s);

#endif
