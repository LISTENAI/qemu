/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_GXA574_H
#define HW_I2C_GXA574_H
#include "hw/i2c/i2c.h"

#define TYPE_GXA574 "gxa574"
OBJECT_DECLARE_SIMPLE_TYPE(GXA574State, GXA574)

void gxa574_outputs(DeviceState *dev, unsigned *driven, unsigned *levels);
uint32_t gxa574_valid_pins(void);
void gxa574_observe(DeviceState *dev,
                    void (*observe)(void *, unsigned, uint32_t, uint32_t),
                    void *opaque);

#endif
