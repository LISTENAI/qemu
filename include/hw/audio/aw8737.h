/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_AW8737_H
#define HW_AUDIO_AW8737_H
#include "hw/qdev-core.h"
#include "qobject/qdict.h"
#define TYPE_AW8737 "aw8737a"
bool aw8737_enabled(DeviceState *dev);
QDict *aw8737_snapshot(DeviceState *dev);
#endif
