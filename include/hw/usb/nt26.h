/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_USB_NT26_H
#define HW_USB_NT26_H
#include "hw/qdev-core.h"
#include "qobject/qdict.h"
#define TYPE_LIERDA_NT26 "lierda-nt26k0b1"
QDict *nt26_snapshot(DeviceState *dev);
#endif
