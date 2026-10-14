/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS four-channel DMA interrupt and peripheral request wiring. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"

static void dma_fault(void *opaque, hwaddr address, unsigned size,
                      bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}

static void dma_irq(void *opaque, uint32_t pending_channels)
{
    arcs_soc_irq(opaque, 20, pending_channels != 0);
}

static void request(void *opaque, int number, int level)
{
    listenai_dma_request(&ARCS_SOC(opaque)->dma, number, level);
}

void arcs_dma_init(ArcsSoC *soc)
{
    listenai_dma_init(&soc->dma, OBJECT(soc), "arcs-cpdma", 0x40000000,
                     4, dma_irq, dma_fault, soc);
    qdev_init_gpio_in_named(DEVICE(soc), request, "cpdma-request", 16);
}

void arcs_dma_reset(ArcsSoC *soc)
{
    listenai_dma_reset(&soc->dma);
}
