/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_BF3901_H
#define HW_I2C_BF3901_H
#include "hw/i2c/i2c.h"
#define TYPE_BF3901 "bf3901"
OBJECT_DECLARE_SIMPLE_TYPE(BF3901State, BF3901)
void bf3901_set_clock(DeviceState *dev, uint64_t hz);
bool bf3901_set_frame(DeviceState *dev, const uint8_t *rgb, size_t length,
                      Error **errp);
void bf3901_clear_frame(DeviceState *dev);
void bf3901_set_prepare(DeviceState *dev, bool (*fn)(void *, Error **),
                        void *opaque);
#endif
