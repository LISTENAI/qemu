/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/char/listenai_uart.h"
#include "qapi/error.h"
#define DMA_MODE (1u << 22)

static bool rx_enabled(ListenAIUART *u)
{
    return (u->control & 1) && !(u->control & (1u << 25));
}

bool listenai_uart_rx_request(const ListenAIUART *u)
{
    return (u->control & DMA_MODE) && (u->control & 1) &&
           !(u->control & (1u << 25)) && u->count > (u->triggers & 63);
}

static uint32_t interrupts(ListenAIUART *u)
{
    return !(u->control & 1) ? 0 : 4 |
        ((u->control & DMA_MODE) ? ((u->timeout_pending ? 0x80 : 0) | u->dma_done) :
         (u->count > (u->triggers & 63) ? 2 : 0) |
         (u->timeout_pending ? 8 : 0));
}

static void update(ListenAIUART *u)
{
    bool dma = (u->control & DMA_MODE) && (u->control & 1);
    if (u->irq) {
        u->irq(u->opaque, (interrupts(u) & u->mask) != 0);
    }
    if (u->dma) {
        u->dma(u->opaque, listenai_uart_rx_request(u), dma);
    }
}

void listenai_uart_dma_done(ListenAIUART *u, bool receive)
{
    u->dma_done |= receive ? 0x40 : 0x20;
    if (receive) {
        /* Done ends the preceding receive activity, including a queued tail.
         * A later character arrival starts a new idle qualification. */
        u->dma_received = false;
        timer_del(u->idle);
    }
    update(u);
}

static void idle(void *opaque)
{
    ListenAIUART *u = opaque;
    u->timeout_pending = (u->control & DMA_MODE) ?
        u->dma_received && (!u->dma_idle_requires_fifo || u->count) : u->count != 0;
    u->dma_received = false;
    update(u);
}

static int can_receive(void *opaque)
{
    ListenAIUART *u = opaque;
    return rx_enabled(u) ? 64 - u->count : 64;
}

static void receive(void *opaque, const uint8_t *bytes, int size)
{
    ListenAIUART *u = opaque;
    if (!rx_enabled(u)) {
        u->discarded_bytes += size;
        return;
    }
    assert(size <= 64 - u->count);
    for (int i = 0; i < size; i++) {
        u->fifo[(u->head + u->count++) & 63] = bytes[i];
    }
    u->rx_bytes += size;
    u->timeout_pending = false;
    u->dma_received = (u->control & DMA_MODE) != 0;
    timer_mod(u->idle, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    update(u);
}

bool listenai_uart_read(ListenAIUART *u, unsigned offset, uint32_t *value)
{
    switch (offset) {
    case 0: *value = u->control; break;
    case 4: *value = 0x80001000 | u->count; break;
    case 8:
        *value = 0;
        if (u->count) {
            *value = u->fifo[u->head];
            u->head = (u->head + 1) & 63;
            u->count--;
        }
        if (!u->count && !(u->control & DMA_MODE)) {
            u->timeout_pending = false;
        }
        update(u);
        qemu_chr_fe_accept_input(&u->chr);
        break;
    case 12: *value = u->mask; break;
    case 16: *value = (interrupts(u) << 16) | (interrupts(u) & u->mask); break;
    case 20: *value = u->triggers; break;
    case 24: case 28: *value = u->commands; break;
    case 32: *value = 0; break;
    default: return false;
    }
    return true;
}

bool listenai_uart_write(ListenAIUART *u, unsigned offset, uint32_t value)
{
    switch (offset) {
    case 0:
        if (value & ((1u << 21) | (1u << 23))) {
            return false;
        }
        if ((value & DMA_MODE) && !u->dma) {
            return false;
        }
        u->control = value;
        qemu_chr_fe_accept_input(&u->chr);
        break;
    case 4: break;
    case 8:
        if (u->control & 1) {
            uint8_t byte = value & ((u->control & 2) ? 255 : 127);
            if (u->control & (1u << 24)) {
                if (u->count == 64) {
                    return false;
                }
                receive(u, &byte, 1);
            } else {
                if (u->tx) { u->tx(u->opaque, byte); }
                qemu_chr_fe_write_all(&u->chr, &byte, 1);
            }
            u->tx_bytes++;
        }
        break;
    case 12: u->mask = value; break;
    case 16:
        u->dma_done &= ~(value & 0x60);
        if (value & ((u->control & DMA_MODE) ? 0x80 : 8)) {
            u->timeout_pending = false;
        }
        break;
    case 20: u->triggers = value; break;
    case 24:
        if (value & 64) {
            u->head = u->count = 0;
            qemu_chr_fe_accept_input(&u->chr);
        }
        u->commands |= value & 63;
        break;
    case 28: u->commands &= ~(value & 63); break;
    case 32: if (value) { return false; } break;
    default: return false;
    }
    update(u);
    return true;
}

void listenai_uart_reset(ListenAIUART *u)
{
    timer_del(u->idle);
    u->control = u->mask = u->triggers = u->commands = u->dma_done = 0;
    u->head = u->count = 0;
    u->timeout_pending = u->dma_received = false;
    update(u);
    qemu_chr_fe_accept_input(&u->chr);
}

void listenai_uart_init(ListenAIUART *u, Chardev *chr)
{
    u->idle = timer_new_ns(QEMU_CLOCK_VIRTUAL, idle, u);
    qemu_chr_fe_init(&u->chr, chr, &error_fatal);
    qemu_chr_fe_set_handlers(&u->chr, can_receive, receive, NULL, NULL, u, NULL, true);
}
