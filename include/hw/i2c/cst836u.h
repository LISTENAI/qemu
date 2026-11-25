/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_CST836U_H
#define HW_I2C_CST836U_H
#include "hw/i2c/i2c.h"
#define TYPE_CST836U "cst836u"
void cst836u_add_input(Object *owner, DeviceState *dev, const char *name);
#endif
