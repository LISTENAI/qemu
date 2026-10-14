/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
static void fail(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}
static void irq(void *opaque, bool level)
{
    arcs_soc_irq(opaque, 52, level);
}
void arcs_aon_timer_clock(ArcsSoC *soc, bool enabled)
{
    listenai_aon_timer_clock(&soc->sysctl.aon_timer, enabled);
}
void arcs_aon_timer_reset(ArcsSoC *soc)
{
    listenai_aon_timer_reset(&soc->sysctl.aon_timer);
}
void arcs_aon_timer_init(ArcsSoC *soc)
{
    ListenaiAONCallbacks cb = { .opaque = soc, .fail = fail, .irq = irq };
    listenai_aon_timer_init(&soc->sysctl.aon_timer, OBJECT(soc),
                            "arcs-aon-timer", 0x48400000, cb);
}
