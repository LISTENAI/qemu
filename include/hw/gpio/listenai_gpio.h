/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPIO_LISTENAI_GPIO_H
#define HW_GPIO_LISTENAI_GPIO_H

#include "hw/sysbus.h"

typedef struct ListenaiGPIO ListenaiGPIO;
typedef struct ListenaiGPIOCallbacks {
    void *opaque;
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
    void (*irq)(void *opaque, unsigned first_pad, bool level);
    void (*output)(void *opaque, unsigned pad, bool level);
} ListenaiGPIOCallbacks;

typedef struct ListenaiPinmux {
    MemoryRegion io;
    uint32_t base, count;
    uint32_t config[64], peripheral_valid[64], peripheral_level[64];
    ListenaiGPIOCallbacks callbacks;
    ListenaiGPIO *banks[8];
    unsigned bank_count;
} ListenaiPinmux;

struct ListenaiGPIO {
    MemoryRegion io;
    ListenaiPinmux *pinmux;
    uint32_t base, first_pad, inputs, pending;
    uint32_t regs[0x78 / 4];
    ListenaiGPIOCallbacks callbacks;
    void (*observe)(void *opaque, unsigned first, uint32_t driven, uint32_t levels);
    void *observe_opaque;
};

void listenai_pinmux_init_unmapped(ListenaiPinmux *s, unsigned count,
                                  ListenaiGPIOCallbacks callbacks);
uint32_t listenai_pinmux_config(ListenaiPinmux *s, unsigned pin);
void listenai_pinmux_configure(ListenaiPinmux *s, unsigned pin, uint32_t value);
void listenai_pinmux_init(ListenaiPinmux *s, Object *owner, const char *name,
                         hwaddr base, unsigned count,
                         ListenaiGPIOCallbacks callbacks);
void listenai_gpio_init(ListenaiGPIO *s, Object *owner, const char *name,
                       hwaddr base, unsigned first_pad, ListenaiPinmux *pinmux,
                       ListenaiGPIOCallbacks callbacks);
void listenai_gpio_input(ListenaiGPIO *s, unsigned pin, bool value);
void listenai_gpio_reset(ListenaiGPIO *s, uint32_t inputs);
void listenai_pinmux_reset(ListenaiPinmux *s);
void listenai_gpio_outputs_snapshot(ListenaiGPIO *s, uint32_t *driven,
                                   uint32_t *levels);
void listenai_gpio_observe(ListenaiGPIO *s,
                          void (*observe)(void *, unsigned, uint32_t, uint32_t),
                          void *opaque);
void listenai_pinmux_output(ListenaiPinmux *s, unsigned pin, unsigned function,
                           bool value);
/* Withdraw one peripheral output without changing PinMux or its saved level. */
void listenai_pinmux_release_output(ListenaiPinmux *s, unsigned pin,
                                    unsigned function);
bool listenai_pinmux_function(ListenaiPinmux *s, unsigned pin,
                             unsigned function);
#endif
