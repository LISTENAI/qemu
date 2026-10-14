/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SD_LISTENAI_SD_H
#define HW_SD_LISTENAI_SD_H

#include "hw/sysbus.h"

typedef void (*ListenAISDFault)(void *opaque, hwaddr address, unsigned size,
                               bool write, uint64_t value);
typedef struct ListenAISD {
    DeviceState *host;
    MemoryRegion io;
    hwaddr address;
    uint8_t regs[0x180];
    ListenAISDFault fault;
    void *opaque;
} ListenAISD;

void listenai_sd_init(ListenAISD *sd, Object *owner, const char *name,
                      hwaddr address, uint64_t capabilities, bool no_card_detect,
                      BlockBackend *backend, qemu_irq irq,
                      ListenAISDFault fault, void *opaque);
void listenai_sd_reset(ListenAISD *sd);
void listenai_sd_enable_unadvertised_sdma(ListenAISD *sd);
#endif
