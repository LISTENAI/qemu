/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPIO_LISTENAI_GPIO_WIRE_H
#define HW_GPIO_LISTENAI_GPIO_WIRE_H
#include "hw/qdev-core.h"
/* A real wire consumer, independent of IRQ tracing/interception endpoints. */
bool listenai_gpio_wire_connected(DeviceState *gpio, unsigned source);
#endif
