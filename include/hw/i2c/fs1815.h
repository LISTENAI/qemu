/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_FS1815_H
#define HW_I2C_FS1815_H
#include "hw/i2c/i2c.h"
#define TYPE_FS1815 "fs1815cn-v1"
bool fs1815_enabled(DeviceState *dev);
int16_t fs1815_amplify(DeviceState *dev, int16_t input);
#endif
