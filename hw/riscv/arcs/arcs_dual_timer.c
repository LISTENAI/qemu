/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS dual-timer register placement and ideal RC32k/2 clock. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"

static void timer_irq(void *opaque, bool level)
{
    ArcsDualTimer *s = opaque;
    arcs_soc_irq(s->soc, s->irq, level);
}

static void timer_fault(void *opaque, hwaddr address, unsigned size,
                        bool write, uint64_t value)
{
    ArcsDualTimer *s = opaque;
    arcs_soc_fail(s->soc, address, size, write, value);
}

void arcs_dual_timer_reset(ArcsSoC *soc)
{
    for (unsigned i = 0; i < 2; i++) {
        listenai_dual_timer_reset(&soc->sysctl.dual_timer[i].device);
    }
}

void arcs_dual_timer_init(ArcsSoC *soc)
{
    for (unsigned i = 0; i < 2; i++) {
        ArcsDualTimer *s = &soc->sysctl.dual_timer[i];
        s->soc = soc;
        s->irq = 31 + i;
        listenai_dual_timer_init(&s->device, OBJECT(soc),
                                i ? "arcs-dual-timer1" : "arcs-dual-timer0",
                                0x46200000 + i * 0x100000, 62500,
                                timer_irq, timer_fault, s);
    }
}
