/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_AXS15231E_TOUCH_H
#define HW_I2C_AXS15231E_TOUCH_H
#include "hw/i2c/i2c.h"
#define TYPE_AXS15231E_TOUCH "axs15231e-touch-ve4"
void axs15231e_touch_add_input(Object *owner, DeviceState *dev, const char *name);
#endif
