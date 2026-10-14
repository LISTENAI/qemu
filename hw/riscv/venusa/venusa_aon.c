/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/runstate.h"

static void fail(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value)
{
    venusa_fail(opaque, address, size, write, value);
}

static void timer_irq(void *opaque, bool level)
{
    VenusASoC *s = opaque;
    if (level) { s->aon[0x14 / 4] |= 1; }
    venusa_irq(s, 64, level);
}

static void aon_wdt_irq(void *opaque, bool level)
{
    VenusASoC *s = opaque;
    if (level) { s->aon[0x14 / 4] |= 2; }
    venusa_irq(s, 65, level);
}

static void aon_wdt_reset(void *opaque, bool reset_pmu)
{
    /* Power-domain reset sequencing requires its own modeled reset path. */
    venusa_fail(opaque, 0x45400000, 4, true, reset_pmu ? 9 : 8);
}

static void wdt_irq(void *opaque, bool level)
{
    VenusAPeripheral *p = opaque;
    venusa_irq(p->soc, 36 + p->index, level);
}

static void wdt_fail(void *opaque, hwaddr address, unsigned size, bool write,
                     uint64_t value)
{
    VenusAPeripheral *p = opaque;
    venusa_fail(p->soc, address, size, write, value);
}

static void wdt_reset(void *opaque)
{
    VenusAPeripheral *p = opaque;
    VenusASoC *s = p->soc;
    uint32_t expansion = s->syscfg[0x0c / 4];
    unsigned common_bit = p->index ? 3 : 0;
    if (expansion & (1u << (12 + p->index))) {
        venusa_fail(s, 0x4310000c, 4, true, expansion);
    }
    if (expansion & (1u << common_bit)) {
        s->reset_cause = 1u << (21 - p->index);
        s->reset_preserve_aon = true;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    s->aon[0x4c / 4] |= 1u << (21 - p->index);
    venusa_reset_cpu(s, p->index);
    if (expansion & (1u << (p->index ? 9 : 6))) {
        venusa_reset_cpu(s, 1 - p->index);
    }
    listenai_wdt_reset(&s->wdt[p->index]);
}

void venusa_aon_init(VenusASoC *s)
{
    ListenaiAONCallbacks callbacks = { .opaque = s, .fail = fail,
                                      .irq = timer_irq };
    listenai_aon_timer_init(&s->aon_timer, OBJECT(s), "venusa-aon-timer",
                            0x45300000, callbacks);
    callbacks.irq = aon_wdt_irq;
    callbacks.reset = aon_wdt_reset;
    listenai_aon_wdt_init(&s->aon_wdt, OBJECT(s), "venusa-aon-wdt",
                          0x45400000, callbacks);
    for (unsigned i = 0; i < 2; i++) {
        s->wdt_context[i] = (VenusAPeripheral){ s, i };
        ListenaiWDT *w = &s->wdt[i];
        w->ignore_locked_writes = true;
        w->opaque = &s->wdt_context[i];
        w->irq = wdt_irq; w->fail = wdt_fail; w->reset = wdt_reset;
        char name[32];
        snprintf(name, sizeof(name), "venusa-core%u-wdt", i);
        listenai_wdt_init(w, OBJECT(s), name, 0x44400000 + 0x100000 * i, 32000);
    }
}

void venusa_aon_reset(VenusASoC *s)
{
    if (!s->reset_preserve_aon) {
        listenai_aon_timer_reset(&s->aon_timer);
        listenai_aon_timer_clock(&s->aon_timer, (s->aon[0x54 / 4] & 4) != 0);
        listenai_aon_wdt_reset(&s->aon_wdt, 0);
    }
    for (unsigned i = 0; i < 2; i++) {
        listenai_wdt_reset(&s->wdt[i]);

    }
}
