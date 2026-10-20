/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_LISTENAI_EXTIO_H
#define HW_I2C_LISTENAI_EXTIO_H
#include "hw/i2c/i2c.h"
#define TYPE_LISTENAI_EXTIO "listenai-extio-ch32v003-v1.3"
OBJECT_DECLARE_SIMPLE_TYPE(ListenaiEXTIO, LISTENAI_EXTIO)
void listenai_extio_outputs(DeviceState *dev, unsigned bank, uint32_t *driven, uint32_t *levels);
double listenai_extio_duty(DeviceState *dev, unsigned pin);
bool listenai_extio_voltage(DeviceState *dev, unsigned channel, unsigned uv);
uint32_t listenai_extio_valid_pins(void);
void listenai_extio_observe(DeviceState *dev,
                           void (*observe)(void *, unsigned, uint32_t, uint32_t),
                           void *opaque);
#endif
