/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_USB_LISTENAI_MUSB_H
#define HW_USB_LISTENAI_MUSB_H
#include "hw/sysbus.h"
#include "qemu/timer.h"
#define TYPE_LISTENAI_MUSB "listenai-musb"
typedef struct ListenAIMUSBHost ListenAIMUSBHost;
typedef struct ListenAIMUSB {
    Object parent_obj;
    void *opaque;
    void (*irq)(void *opaque, bool level);
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value);
    hwaddr base;
    bool clock_enabled;
    MemoryRegion io;
    uint8_t address, power, index, mask;
    uint16_t tx_mask, rx_mask;
    uint8_t endpoint[16][16], fifo_config[16][6];
    bool session;
    uint8_t interrupt;
    uint16_t tx_pending, rx_pending;
    uint8_t rx_fifo[64], tx_fifo[64];
    unsigned rx_count, rx_position, tx_count;
    uint8_t bulk_rx[16][1024], bulk_tx[16][1024];
    unsigned bulk_count[16], bulk_position[16], bulk_tx_count[16];
    uint32_t dma[6][3], dma_pending;
    ListenAIMUSBHost *host;
    qemu_irq host_connected;
} ListenAIMUSB;

OBJECT_DECLARE_SIMPLE_TYPE(ListenAIMUSB, LISTENAI_MUSB)
void listenai_musb_init(ListenAIMUSB *s, DeviceState *owner, hwaddr base,
                       void (*irq)(void *, bool),
                       void (*fail)(void *, hwaddr, unsigned, bool, uint64_t),
                       void *opaque, bool adb);
void listenai_musb_destroy(ListenAIMUSB *s);
void listenai_musb_reset(ListenAIMUSB *s);
void listenai_musb_clock(ListenAIMUSB *s, bool enabled);
char *listenai_musb_snapshot(ListenAIMUSB *s);
#endif
