/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_BL6138_H
#define HW_I2C_BL6138_H
#include "hw/i2c/i2c.h"
#define TYPE_BL6138 "bl6138"
void bl6138_redrive(DeviceState *dev);
void bl6138_add_input(Object *owner, DeviceState *dev, const char *name);
#endif
