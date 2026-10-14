/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RTC_LISTENAI_CALENDAR_H
#define HW_RTC_LISTENAI_CALENDAR_H
#include "system/memory.h"
#include "qemu/timer.h"

typedef void (*ListenAICalendarIRQ)(void *opaque, bool level);
typedef void (*ListenAICalendarFault)(void *opaque, hwaddr address, unsigned size,
                                    bool write, uint64_t value);
typedef struct ListenAICalendar {
    MemoryRegion io;
    hwaddr address;
    uint32_t regs[10];
    int64_t epoch, started, alarm;
    QEMUTimer *event;
    uint32_t pending;
    uint8_t weekday;
    bool alarm_enabled, interval_enabled, wakeup;
    bool allow_zero_date, zero_date;
    ListenAICalendarIRQ irq;
    ListenAICalendarFault fault;
    void *opaque;
} ListenAICalendar;

void listenai_calendar_init(ListenAICalendar *s, Object *owner,
                            const char *name, hwaddr address,
                            ListenAICalendarIRQ irq,
                            ListenAICalendarFault fault, void *opaque);
void listenai_calendar_reset(ListenAICalendar *s);
#endif
