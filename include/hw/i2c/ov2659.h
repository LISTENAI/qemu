/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_OV2659_H
#define HW_I2C_OV2659_H

#include "hw/i2c/i2c.h"
#include "qapi/error.h"

#define TYPE_OV2659 "ov2659"
OBJECT_DECLARE_SIMPLE_TYPE(OV2659State, OV2659)

typedef struct OV2659Output {
    unsigned width, height, hts, vts;
    unsigned pll_multiplier, pll_divisor;
    bool invert_href, invert_vsync, colorbar;
} OV2659Output;

bool ov2659_begin_frame(DeviceState *dev, OV2659Output *out, Error **errp);
uint8_t ov2659_sample(void *opaque, unsigned byte, unsigned line);
bool ov2659_set_frame(DeviceState *dev, const uint8_t *rgb, size_t length,
                      Error **errp);
void ov2659_clear_frame(DeviceState *dev);
void ov2659_set_clock(DeviceState *dev, bool enabled);
void ov2659_set_notify(DeviceState *dev, void (*notify)(void *, bool),
                       void *opaque);
#endif
