/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS UART interrupt, observation and DMA-request routing. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/irq.h"
#include "system/system.h"

#define DMA_MODE (1u << 22)
static const unsigned dma_requests[3][2] = { { 8, 9 }, { 2, 3 }, { 0, 1 } };

static void uart_dma(void *opaque, bool receive, bool transmit)
{
    ArcsUART *u = opaque;
    uint32_t mux = u->soc->sysctl.common_regs[0x94 / 4];
    unsigned selection = u->index == 2 ? 1 : 0;
    qemu_set_irq(u->soc->uart_dma[2 * u->index], receive &&
        ((mux >> (2 * dma_requests[u->index][0])) & 3) == selection);
    qemu_set_irq(u->soc->uart_dma[2 * u->index + 1], transmit &&
        ((mux >> (2 * dma_requests[u->index][1])) & 3) == selection);
}

void arcs_uart_update_dma(ArcsSoC *s)
{
    for (unsigned i = 0; i < 3; i++) {
        ArcsUART *u = &s->uart[i];
        ListenAIUART *device = &u->device;
        bool enabled = (device->control & DMA_MODE) && (device->control & 1);
        uart_dma(u, enabled && !(device->control & (1u << 25)) && device->count,
                 enabled);
    }
}

static void uart_irq(void *opaque, bool level)
{
    ArcsUART *u = opaque;
    arcs_soc_irq(u->soc, 40 + u->index, level);
}

static void uart_tx(void *opaque, uint8_t byte)
{
    ArcsUART *u = opaque;
    if (u->observe) { u->observe(u->observe_opaque, u->index, byte); }
}

uint32_t arcs_uart_read(ArcsSoC *s, unsigned index, unsigned offset)
{
    uint32_t value;
    if (!listenai_uart_read(&s->uart[index].device, offset, &value)) {
        arcs_soc_fail(s, 0x46a00000 + index * 0x100000 + offset, 4, false, 0);
    }
    return value;
}

void arcs_uart_write(ArcsSoC *s, unsigned index, unsigned offset, uint32_t value)
{
    if (!listenai_uart_write(&s->uart[index].device, offset, value)) {
        arcs_soc_fail(s, 0x46a00000 + index * 0x100000 + offset, 4, true, value);
    }
}

void arcs_uart_reset(ArcsSoC *s, unsigned index)
{
    listenai_uart_reset(&s->uart[index].device);
}

void arcs_uart_init(ArcsSoC *s)
{
    qdev_init_gpio_out_named(DEVICE(s), s->uart_dma, "uart-dma", 6);
    for (unsigned i = 0; i < 3; i++) {
        ArcsUART *u = &s->uart[i];
        u->soc = s;
        u->index = i;
        u->device.opaque = u;
        u->device.irq = uart_irq;
        u->device.dma = uart_dma;
        u->device.tx = uart_tx;
        listenai_uart_init(&u->device, serial_hd(i));
        for (unsigned direction = 0; direction < 2; direction++) {
            qdev_connect_gpio_out_named(DEVICE(s), "uart-dma", 2 * i + direction,
                qdev_get_gpio_in_named(DEVICE(s), "cpdma-request", dma_requests[i][direction]));
        }
    }
}
