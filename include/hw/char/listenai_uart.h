/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CHAR_LISTENAI_UART_H
#define HW_CHAR_LISTENAI_UART_H
#include "chardev/char-fe.h"
#include "qemu/timer.h"

/* Shared UART register/FIFO contract; SoCs own clock, pins and DMA routing. */
typedef struct ListenAIUART {
    CharBackend chr;
    QEMUTimer *idle;
    void *opaque;
    void (*irq)(void *opaque, bool level);
    void (*dma)(void *opaque, bool receive, bool transmit);
    void (*tx)(void *opaque, uint8_t byte);
    uint32_t control, mask, triggers, commands, dma_done;
    uint8_t fifo[64];
    unsigned head, count;
    bool timeout_pending, dma_received;
    bool dma_idle_requires_fifo;
    uint64_t tx_bytes, rx_bytes, discarded_bytes;
} ListenAIUART;

void listenai_uart_init(ListenAIUART *u, Chardev *chr);
void listenai_uart_reset(ListenAIUART *u);
void listenai_uart_dma_done(ListenAIUART *u, bool receive);
bool listenai_uart_rx_request(const ListenAIUART *u);
bool listenai_uart_read(ListenAIUART *u, unsigned offset, uint32_t *value);
bool listenai_uart_write(ListenAIUART *u, unsigned offset, uint32_t value);
#endif
