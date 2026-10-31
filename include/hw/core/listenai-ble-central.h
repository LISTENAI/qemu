/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_LISTENAI_BLE_CENTRAL_H
#define HW_LISTENAI_BLE_CENTRAL_H

#include "qapi/error.h"

typedef struct ListenaiBLECentral ListenaiBLECentral;

typedef bool (*ListenaiBLEReceive)(void *, const uint8_t *, unsigned, unsigned,
                                   uint32_t);

ListenaiBLECentral *listenai_ble_central_new(ListenaiBLEReceive receive,
                                             void *opaque, int64_t *epoch);
void listenai_ble_central_destroy(ListenaiBLECentral *s);
void listenai_ble_central_reset(ListenaiBLECentral *s);
void listenai_ble_central_transmit(ListenaiBLECentral *s, const uint8_t *pdu,
                                   unsigned length, unsigned channel,
                                   uint32_t address, uint64_t half_us);
char *listenai_ble_central_command(ListenaiBLECentral *s, const char *command,
                                   Error **errp);

#endif
