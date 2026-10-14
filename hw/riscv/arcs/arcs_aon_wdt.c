/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "system/runstate.h"
static void fail(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}
static void irq(void *opaque, bool level)
{
    arcs_soc_irq(opaque, 53, level);
}
static void reset(void *opaque, bool reset_pmu)
{
    ArcsSoC *soc = opaque;
    soc->sysctl.reset_status = 1u << 1;
    soc->sysctl.aon_wdt_reset_cause = 0x20000;
    soc->sysctl.warm_reset = true;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}
void arcs_aon_wdt_reset(ArcsSoC *soc)
{
    listenai_aon_wdt_reset(&soc->sysctl.aon_wdt, soc->sysctl.aon_wdt_reset_cause);
}
void arcs_aon_wdt_init(ArcsSoC *soc)
{
    ListenaiAONCallbacks cb = { .opaque = soc, .fail = fail, .irq = irq, .reset = reset };
    listenai_aon_wdt_init(&soc->sysctl.aon_wdt, OBJECT(soc),
                          "arcs-aon-wdt", 0x48500000, cb);
}
