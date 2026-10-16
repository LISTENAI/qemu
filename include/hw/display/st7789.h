/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DISPLAY_ST7789_H
#define HW_DISPLAY_ST7789_H
#include "hw/qdev-core.h"
#define TYPE_ST7789 "st7789"
void st7789_redrive(DeviceState *dev);
void st7789_backlight(DeviceState *dev, double duty);
void st7789_save(DeviceState *dev, const char *path);
void st7789_report(DeviceState *dev, FILE *file);
void st7789_failure_callback(DeviceState *dev,
                                  void (*fn)(void *, const char *), void *opaque);
#endif
