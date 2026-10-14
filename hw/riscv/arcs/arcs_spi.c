/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS SPI clock/IRQ/DMA routing. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/irq.h"
#include "system/address-spaces.h"

static uint64_t clock_rate(ArcsSPI *s)
{
    uint32_t cfg = s->soc->sysctl.common_regs[0x14 / 4 + s->index];
    uint32_t *pll = s->soc->sysctl.pll_regs;
    uint64_t num = 24000000, den = 1;
    if (!(cfg & (1u << 23))) { return 0; }
    if (cfg & (1u << 22)) {
        if (!(pll[2] & 1)) { return 0; }
        if (pll[6] & 0x100) {
            arcs_soc_fail(s->soc, 0x46100018, 4, false, pll[6]);
        }
        num *= pll[6] & 255;
        /* LS2684 SPI uses SYSPLL /12 for all reserved PERI_DIV_SEL
         * values. Verified with long TX transfers and PLL relock. */
        den = 12;
    }
    num *= (cfg >> 28) & 7;
    den *= (cfg >> 24) & 15;
    if (!num || !den) {
        arcs_soc_fail(s->soc, 0x46000014 + 4 * s->index, 4, false, cfg);
    }
    assert(LISTENAI_SPI_CLOCK_SCALE % den == 0);
    return num * (LISTENAI_SPI_CLOCK_SCALE / den);
}

static void spi_fail(void *opaque, hwaddr address, unsigned size,
                     bool write, uint64_t value)
{
    ArcsSPI *s = opaque;
    arcs_soc_fail(s->soc, address, size, write, value);
}
static void spi_irq(void *opaque, bool level)
{
    ArcsSPI *s = opaque;
    arcs_soc_irq(s->soc, 46 + s->index, level);
}
static void spi_cs(void *opaque, bool level)
{
    ArcsSPI *s = opaque;
    qemu_set_irq(s->soc->spi_cs[s->index], level);
}
static void spi_dma(void *opaque, bool receive, bool transmit)
{
    ArcsSPI *s = opaque;
    qemu_set_irq(s->soc->spi_dma[s->index], transmit);
}
static bool spi_dma_active(void *opaque)
{
    return ((ArcsSPI *)opaque)->soc->dma.servicing;
}
static uint64_t spi_clock(void *opaque)
{
    return clock_rate(opaque);
}
void arcs_spi_clock_changed(ArcsSoC *soc, unsigned index)
{
    listenai_spi_clock_changed(&soc->spi[index].device);
}

void arcs_spi_init(ArcsSoC *soc)
{
    qdev_init_gpio_out_named(DEVICE(soc), soc->spi_cs, "spi-cs", 3);
    qdev_init_gpio_out_named(DEVICE(soc), soc->spi_dma, "spi-dma", 3);
    for (unsigned i = 0; i < 3; i++) {
        ArcsSPI *s = &soc->spi[i];
        char name[16];
        s->soc = soc; s->index = i;
        s->device.safe_reads = soc->safe_mmio_reads;
        snprintf(name, sizeof(name), "arcs-spi%u", i);
        ListenaiSPICallbacks callbacks = { .opaque = s, .fail = spi_fail,
            .irq = spi_irq, .cs = spi_cs, .dma = spi_dma,
            .dma_active = spi_dma_active, .clock_rate = spi_clock };
        listenai_spi_init(&s->device, DEVICE(soc), name,
                          0x47000000 + i * 0x100000, callbacks);
        static const unsigned dma_tx_request[] = { 11, 7, 5 };
        qdev_connect_gpio_out_named(DEVICE(soc), "spi-dma", i,
            qdev_get_gpio_in_named(DEVICE(soc), "cpdma-request", dma_tx_request[i]));
    }

}
void arcs_spi_reset(ArcsSoC *soc, unsigned i)
{
    listenai_spi_reset(&soc->spi[i].device);
}

