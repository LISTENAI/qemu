/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DISPLAY_JD79651C_H
#define HW_DISPLAY_JD79651C_H
#include "hw/qdev-core.h"
#include "qobject/qdict.h"
#define TYPE_JD79651C "jd79651c"
void jd79651c_redrive(DeviceState *dev);
void jd79651c_save(DeviceState *dev, const char *path);
void jd79651c_report(DeviceState *dev, FILE *stream);
QDict *jd79651c_display_state(DeviceState *dev);
void jd79651c_failure_callback(DeviceState *dev,
                              void (*fn)(void *, const char *), void *opaque);
#endif
