/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA clock tree, IRQ and common-DMA routing for the shared SPI engine. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/riscv/venusa/venusa.h"
#include "hw/irq.h"

static uint64_t spi_clock(void *opaque)
{
    VenusASPI *p = opaque;
    VenusASoC *s = p->soc;
    uint32_t cfg = s->syscfg[0x1c / 4 + p->index];
    uint64_t n = 24000000, d = 1;
    if (!(cfg & (1u << 23))) { return 0; }
    if (cfg & (1u << 22)) {
        if (!(s->buscfg[2] & 1) || !(s->buscfg[2] & (1u << 26))) { return 0; }
        /* Fractional PLL requires a wider rational rate representation. */
        if (s->buscfg[6] & (1u << 9)) {
            venusa_fail(s, 0x43000018, 4, false, s->buscfg[6]);
        }
        n *= s->buscfg[6] & 0x1ff;
        d = ((s->buscfg[3] >> 12) & 15) + 6;
        if (d > 16) { venusa_fail(s, 0x4300000c, 4, false, s->buscfg[3]); }
    }
    n *= p->n;
    d *= p->m;
    if (!n || !d || LISTENAI_SPI_CLOCK_SCALE % d) {
        venusa_fail(s, 0x4310001c + p->index * 4, 4, false, cfg);
    }
    uint64_t rate, high;
    mulu64(&rate, &high, n, LISTENAI_SPI_CLOCK_SCALE / d);
    if (high) { venusa_fail(s, 0x4310001c + p->index * 4, 4, false, cfg); }
    return rate;
}
static void fault(void *opaque, hwaddr address, unsigned size,
                  bool write, uint64_t value)
{
    venusa_fail(((VenusASPI *)opaque)->soc, address, size, write, value);
}
static void interrupt(void *opaque, bool level)
{
    VenusASPI *p = opaque;
    venusa_irq(p->soc, 32 + p->index, level);
}
static void chip_select(void *opaque, bool level)
{
    qemu_set_irq(((VenusASPI *)opaque)->cs, level);
}
void venusa_spi_dma_update(VenusASoC *s)
{
    uint32_t mux = s->syscfg[0xa0 / 4];
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        unsigned rx = 6 + 2 * i, tx = rx + 1;
        if (!(mux & (1u << rx))) { listenai_dma_request(&s->dma, rx, p->rx_request); }
        if (!(mux & (1u << tx))) { listenai_dma_request(&s->dma, tx, p->tx_request); }
    }
}
static void requests(void *opaque, bool receive, bool transmit)
{
    VenusASPI *p = opaque;
    p->rx_request = receive;
    p->tx_request = transmit;
    venusa_spi_dma_update(p->soc);
}
static bool dma_active(void *opaque)
{
    return ((VenusASPI *)opaque)->soc->dma.servicing;
}
void venusa_spi_clock(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        uint32_t cfg = s->syscfg[0x1c / 4 + i];
        if ((cfg & (1u << 31)) && !(p->clock_config & (1u << 31))) {
            p->n = (cfg >> 28) & 7;
            p->m = (cfg >> 24) & 15;
        }
        p->clock_config = cfg;
        if (p->device.service) { listenai_spi_clock_changed(&p->device); }
    }
}
void venusa_spi_reset(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        p->n = p->m = 1; p->clock_config = 0;
        if (p->device.service) { listenai_spi_reset(&p->device); }
    }
}
void venusa_spi_init(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        char name[32];
        p->soc = s; p->index = i; p->n = p->m = 1;
        p->device.tx_depth = p->device.rx_depth = 8;
        p->device.feature = 0x4322;
        p->device.receive_supported = p->device.masked_status = true;
        snprintf(name, sizeof(name), "venusa-spi%u", i);
        ListenaiSPICallbacks cb = { .opaque = p, .fail = fault,
            .irq = interrupt, .cs = chip_select, .dma = requests,
            .dma_active = dma_active, .clock_rate = spi_clock };
        listenai_spi_init(&p->device, DEVICE(s), name,
                         0x43e00000 + i * 0x100000, cb);
        snprintf(name, sizeof(name), "spi%u-cs", i);
        qdev_init_gpio_out_named(DEVICE(s), &p->cs, name, 1);
    }
}
