/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "system/system.h"

static void uart_dma(void *opaque, bool receive, bool transmit)
{
    VenusAUART *u = opaque;
    uint32_t mux = u->soc->syscfg[0xa0 / 4];
    unsigned rx = 2 * u->index, tx = rx + 1;
    if (!(mux & (1u << rx))) { listenai_dma_request(&u->soc->dma, rx, receive); }
    if (!(mux & (1u << tx))) { listenai_dma_request(&u->soc->dma, tx, transmit); }
    if (u->index == 1) {
        if (mux & (1u << 6)) { listenai_dma_request(&u->soc->dma, 6, receive); }
        if (mux & (1u << 7)) { listenai_dma_request(&u->soc->dma, 7, transmit); }
    }
}

static void uart_dma_done(void *opaque, unsigned request, bool receive)
{
    VenusASoC *s = opaque;
    uint32_t mux = s->syscfg[0xa0 / 4];
    if (request < 6 && !(mux & (1u << request)) &&
        (request & 1) == !receive) {
        listenai_uart_dma_done(&s->uart[request / 2].device, receive);
    } else if ((request == 6 || request == 7) &&
               (mux & (1u << request)) && (request & 1) == !receive) {
        listenai_uart_dma_done(&s->uart[1].device, receive);
    }
}

void venusa_uart_update_dma(VenusASoC *s)
{
    for (unsigned i = 0; i < 3; i++) {
        ListenAIUART *d = &s->uart[i].device;
        bool enable = (d->control & (1u << 22)) && (d->control & 1);
        uart_dma(&s->uart[i], listenai_uart_rx_request(d), enable);
    }
}

static void uart_irq(void *opaque, bool level)
{
    VenusAUART *u = opaque;
    venusa_irq(u->soc, 26 + u->index, level);
}

uint32_t venusa_uart_read(VenusASoC *s, unsigned index, unsigned offset)
{
    uint32_t value;
    if (!listenai_uart_read(&s->uart[index].device, offset, &value)) {
        venusa_fail(s, 0x43800000 + index * 0x100000 + offset, 4, false, 0);
    }
    return value;
}

void venusa_uart_write(VenusASoC *s, unsigned index, unsigned offset, uint32_t value)
{
    if (!listenai_uart_write(&s->uart[index].device, offset, value)) {
        venusa_fail(s, 0x43800000 + index * 0x100000 + offset, 4, true, value);
    }
}

void venusa_uart_reset(VenusASoC *s, unsigned index)
{
    listenai_uart_reset(&s->uart[index].device);
}

void venusa_uart_init(VenusASoC *s)
{
    s->dma.done = uart_dma_done;
    for (unsigned i = 0; i < 3; i++) {
        VenusAUART *u = &s->uart[i];
        u->soc = s;
        u->index = i;
        u->device.opaque = u;
        u->device.irq = uart_irq;
        u->device.dma = uart_dma;
        u->device.dma_idle_requires_fifo = true;
        listenai_uart_init(&u->device, serial_hd(i));
    }
}
