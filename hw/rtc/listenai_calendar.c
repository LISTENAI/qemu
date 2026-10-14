/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Calendar, interval and alarm interrupts on the virtual clock. */
#include "qemu/osdep.h"
#include "hw/rtc/listenai_calendar.h"
#include "system/address-spaces.h"

static int64_t calendar_now(ListenAICalendar *s)
{
    return s->epoch +
           (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->started) / 1000000000;
}

static void calendar_irq(ListenAICalendar *s)
{
    s->irq(s->opaque, ((s->pending & 1) && s->interval_enabled) ||
                     ((s->pending & 2) && s->alarm_enabled));
}

static void calendar_arm(ListenAICalendar *s)
{
    static const unsigned periods[] = {0, 1, 60, 3600};
    int64_t now = calendar_now(s), next = INT64_MAX;
    unsigned period = periods[s->regs[0] & 3];
    if (s->interval_enabled && period) { next = now + period - now % period; }
    if (s->alarm_enabled && s->alarm > now) {
        next = MIN(next, s->alarm);
    }
    timer_del(s->event);
    if (next != INT64_MAX) {
        timer_mod(s->event, s->started +
                  (next - s->epoch) * INT64_C(1000000000));
    }
}

static void calendar_expire(void *opaque)
{
    static const unsigned periods[] = {0, 1, 60, 3600};
    ListenAICalendar *s = opaque;
    int64_t now = calendar_now(s);
    unsigned period = periods[s->regs[0] & 3];
    if (s->interval_enabled && period && !(now % period)) { s->pending |= 1; }
    if (s->alarm_enabled && now == s->alarm) { s->pending |= 2; }
    calendar_irq(s); calendar_arm(s);
}

void listenai_calendar_reset(ListenAICalendar *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->epoch = 946684800; /* 2000-01-01T00:00:00Z, never host time. */
    s->started = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->wakeup = false;
    s->alarm_enabled = s->interval_enabled = false;
    s->pending = 0;
    s->zero_date = false;
    s->weekday = 6;
    s->alarm = 0;
    timer_del(s->event);
    calendar_irq(s);
}

static uint64_t calendar_read(void *opaque, hwaddr off, unsigned size)
{
    ListenAICalendar *s = opaque;
    if (size != 4 || (off & 3) || off > 0x24) {
        s->fault(s->opaque, s->address + off, size, false, 0);
        abort();
    }
    if (off == 4) { return 0; } /* Synchronous load commands. */
    if (off == 8) {
        return s->pending | (s->wakeup ? 0x100 : 0) |
               (s->interval_enabled ? 0x1000 : 0) |
               ((s->pending & 1) ? 0x10000 : 0) |
               (s->alarm_enabled ? 0x100000 : 0);
    }
    if (off == 0x14 || off == 0x18) {
        int64_t seconds = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->started) / 1000000000;
        GDateTime *date = g_date_time_new_from_unix_utc(s->epoch + seconds);
        assert(date);
        uint32_t value;
        if (off == 0x14) {
            value = (g_date_time_get_hour(date) << 16) |
                (g_date_time_get_minute(date) << 8) | g_date_time_get_second(date);
        } else if (s->zero_date && s->epoch + seconds < 946684800) {
            /* Month zero has 32 raw day values (0..31); midnight after day31
             * carries to January 1, year zero. Weekday counts independently. */
            int64_t days = (s->epoch + seconds - (946684800 - 32 * 86400)) / 86400;
            value = (((s->weekday + (s->epoch % 86400 + seconds) / 86400) % 7) << 24) |
                    days;
        } else {
            value = (((s->weekday + (s->epoch % 86400 + seconds) / 86400) % 7) << 24) |
                ((g_date_time_get_year(date) - 2000) << 16) |
                (g_date_time_get_month(date) << 8) | g_date_time_get_day_of_month(date);
        }
        g_date_time_unref(date);
        return value;
    }
    return s->regs[off / 4];
}

static void calendar_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenAICalendar *s = opaque;
    if (size != 4 || (off & 3) || off > 0x24) { goto fail; }
    switch (off) {
    case 0: s->regs[0] = value & 3; calendar_arm(s); return;
    case 4:
        if (value & ~0x30371u) { goto fail; }
        if (value & 1) {
            uint32_t lo = s->regs[3], hi = s->regs[4];
            if ((lo & 63) > 59) { goto fail; }
            bool zero = s->allow_zero_date && !(hi & 0xffff00);
            GDateTime *date = g_date_time_new_utc(2000 + ((hi >> 16) & 127),
                zero ? 1 : (hi >> 8) & 15, zero ? 1 : hi & 31,
                (lo >> 16) & 31, (lo >> 8) & 63, lo & 63);
            if (!date) { goto fail; }
            s->zero_date = zero;
            s->epoch = g_date_time_to_unix(date);
            if (zero) { s->epoch -= (32 - (hi & 31)) * INT64_C(86400); }
            s->weekday = (hi >> 24) & 7;
            if (s->weekday > 6) { g_date_time_unref(date); goto fail; }
            g_date_time_unref(date);
            s->started = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        if (value & 0x10) {
            uint32_t lo = s->regs[7], hi = s->regs[8];
            bool zero = s->allow_zero_date && !(hi & 0xffff00);
            GDateTime *date = g_date_time_new_utc(2000 + ((hi >> 16) & 127),
                zero ? 1 : (hi >> 8) & 15, zero ? 1 : hi & 31,
                (lo >> 16) & 31, (lo >> 8) & 63, lo & 63);
            if (!date) { goto fail; }
            s->alarm = g_date_time_to_unix(date);
            if (zero) { s->alarm -= (32 - (hi & 31)) * INT64_C(86400); }
            g_date_time_unref(date);
        }
        if (value & 0x20) { s->alarm_enabled = true; }
        if (value & 0x40) { s->alarm_enabled = false; }
        if (value & 0x100) { s->pending &= ~2u; }
        if (value & 0x200) { s->pending &= ~1u; }
        if (value & 0x10000) { s->interval_enabled = true; }
        if (value & 0x20000) { s->interval_enabled = false; }
        calendar_irq(s); calendar_arm(s);
        return;
    case 0xc: case 0x10: case 0x1c: case 0x20: s->regs[off / 4] = value; return;
    case 0x24: s->regs[off / 4] = value & 0x7ffffff; return;
    default: break;
    }
fail:
    s->fault(s->opaque, s->address + off, size, true, value);
    abort();
}

static const MemoryRegionOps calendar_ops = {
    .read = calendar_read, .write = calendar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_calendar_init(ListenAICalendar *s, Object *owner,
                            const char *name, hwaddr address,
                            ListenAICalendarIRQ irq,
                            ListenAICalendarFault fault, void *opaque)
{
    s->address = address;
    s->irq = irq;
    s->fault = fault;
    s->opaque = opaque;
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, calendar_expire, s);
    memory_region_init_io(&s->io, owner, &calendar_ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), address, &s->io);
}
