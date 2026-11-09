/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_SC919_TOUCH_H
#define HW_I2C_SC919_TOUCH_H
#include "hw/i2c/i2c.h"
#define TYPE_SC919_TOUCH "sc919-touch-v1"
void sc919_touch_add_input(Object *owner, DeviceState *dev, const char *name);
#endif
