/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_GPIO_H
#define HW_RISCV_ARCS_GPIO_H
#include "hw/gpio/listenai_gpio.h"
typedef struct ArcsSoC ArcsSoC;
typedef ListenaiPinmux ArcsPinmux;
typedef ListenaiGPIO ArcsGPIO;
void arcs_gpio_init(ArcsSoC *soc);
void arcs_gpio_reset(ArcsSoC *soc);
void arcs_gpio_outputs_snapshot(ArcsGPIO *s, uint32_t *driven, uint32_t *levels);
void arcs_pinmux_output(ArcsPinmux *s, unsigned pin, unsigned function, bool value);
bool arcs_pinmux_function(ArcsPinmux *s, unsigned pin, unsigned function);
#endif
