/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_LS8500_H
#define HW_I2C_LS8500_H
#include "hw/i2c/i2c.h"
#include "qobject/qdict.h"
#define TYPE_LS8500 "ls8500"
void ls8500_set_illuminance(DeviceState *dev, uint32_t lux);
QDict *ls8500_snapshot(DeviceState *dev);
#endif
