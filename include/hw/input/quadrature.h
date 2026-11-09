/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_INPUT_QUADRATURE_H
#define HW_INPUT_QUADRATURE_H
#include "hw/qdev-core.h"
#include "qobject/qdict.h"
#define TYPE_QUADRATURE "quadrature-encoder"
QDict *quadrature_snapshot(DeviceState *dev);
void quadrature_command(DeviceState *dev, QDict *command, Error **errp);
void quadrature_drive(DeviceState *dev);
#endif
