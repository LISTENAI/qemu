/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DISPLAY_SERIAL_LCD_H
#define HW_DISPLAY_SERIAL_LCD_H
#include "hw/qdev-core.h"
#include "qobject/qdict.h"
#define TYPE_ST7789 "st7789"
#define TYPE_AXS15231E "axs15231e"
#define TYPE_SC919 "sc919"
#define TYPE_SERIAL_LCD "serial-lcd"
void serial_lcd_qspi_lanes(DeviceState *dev, unsigned lanes);
void serial_lcd_redrive(DeviceState *dev);
void serial_lcd_backlight(DeviceState *dev, double duty);
void serial_lcd_save(DeviceState *dev, const char *path);
void serial_lcd_report(DeviceState *dev, FILE *file);
QDict *serial_lcd_display_state(DeviceState *dev);
void serial_lcd_failure_callback(DeviceState *dev,
                                  void (*fn)(void *, const char *), void *opaque);
#endif
