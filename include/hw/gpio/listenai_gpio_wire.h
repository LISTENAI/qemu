/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPIO_LISTENAI_GPIO_WIRE_H
#define HW_GPIO_LISTENAI_GPIO_WIRE_H
#include "hw/qdev-core.h"
/*
 * Query only realized wire devices. IRQ tracing/interception is not a wire.
 * Realization registers a single-driver connection; unrealization removes it.
 * These queries describe connectivity, not the protocol carried by the pads.
 */
bool listenai_gpio_wire_connected(DeviceState *gpio, unsigned source);
bool listenai_gpio_wire_connects(DeviceState *gpio, unsigned source,
                                 unsigned destination);
#endif
