/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS MUSB address and interrupt routing. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"

static void irq(void *opaque, bool level)
{
    arcs_soc_irq(opaque, 24, level);
}

static void fail(void *opaque, hwaddr address, unsigned size,
                 bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}

void arcs_usb_init(ArcsSoC *s)
{
    listenai_musb_init(&s->usb, DEVICE(s), 0x41000000, irq, fail, s, true);
}

void arcs_usb_reset(ArcsSoC *s) { listenai_musb_reset(&s->usb); }
void arcs_usb_finalize(ArcsSoC *s) { listenai_musb_destroy(&s->usb); }
char *arcs_usb_snapshot(ArcsSoC *s) { return listenai_musb_snapshot(&s->usb); }
