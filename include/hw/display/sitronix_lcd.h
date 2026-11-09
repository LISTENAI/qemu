/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DISPLAY_SITRONIX_LCD_H
#define HW_DISPLAY_SITRONIX_LCD_H
#include "hw/qdev-core.h"
#include "qobject/qdict.h"
#define TYPE_ST7789 "st7789"
#define TYPE_SC919 "sc919"
#define TYPE_SITRONIX_LCD "sitronix-lcd"
void sitronix_lcd_qspi_lanes(DeviceState *dev, unsigned lanes);
void sitronix_lcd_redrive(DeviceState *dev);
void sitronix_lcd_backlight(DeviceState *dev, double duty);
void sitronix_lcd_save(DeviceState *dev, const char *path);
void sitronix_lcd_report(DeviceState *dev, FILE *file);
QDict *sitronix_lcd_display_state(DeviceState *dev);
void sitronix_lcd_failure_callback(DeviceState *dev,
                                  void (*fn)(void *, const char *), void *opaque);
#endif
