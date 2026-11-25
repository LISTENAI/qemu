/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_GPIO_LISTENAI_EXPANDER_H
#define HW_GPIO_LISTENAI_EXPANDER_H
#include "hw/qdev-core.h"

/* Board-facing GPIO capabilities; protocol-specific I2C remains in devices. */
void listenai_expander_outputs(DeviceState *dev, unsigned bank,
                              uint32_t *driven, uint32_t *levels);
bool listenai_expander_active(DeviceState *dev, unsigned pin, bool level);
double listenai_expander_duty(DeviceState *dev, unsigned pin);
uint32_t listenai_expander_valid_pins(DeviceState *dev);
unsigned listenai_expander_adc_channels(DeviceState *dev);
bool listenai_expander_voltage(DeviceState *dev, unsigned channel, unsigned uv);
void listenai_expander_observe(DeviceState *dev,
                              void (*observe)(void *, unsigned, uint32_t, uint32_t),
                              void *opaque);
#endif
