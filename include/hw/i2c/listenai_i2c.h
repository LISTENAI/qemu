/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_I2C_LISTENAI_I2C_H
#define HW_I2C_LISTENAI_I2C_H
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qemu/timer.h"

typedef struct ListenaiI2CCallbacks {
    void *opaque;
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
    void (*irq)(void *opaque, bool level);
    /* Input clock in Hz as an exact numerator/denominator ratio. */
    void (*clock)(void *opaque, uint64_t *numerator, uint64_t *denominator);
} ListenaiI2CCallbacks;
typedef struct ListenaiI2CProfile {
    unsigned fifo_depth;
    uint32_t configuration, identification;
    bool directional_half;
} ListenaiI2CProfile;

typedef struct ListenaiI2C {
    MemoryRegion io;
    hwaddr base;
    ListenaiI2CCallbacks callbacks;
    ListenaiI2CProfile profile;
    uint32_t regs[0x34 / 4];
    uint8_t fifo[16];
    unsigned head, count, remaining;
    I2CBus *bus;
    QEMUTimer *event;
    bool active, address_phase, receiving, stop, stop_only;
    bool (*route_valid)(void *opaque);
    void *route_opaque;
} ListenaiI2C;
void listenai_i2c_init(ListenaiI2C *s, DeviceState *owner, const char *name,
                       hwaddr base, ListenaiI2CCallbacks callbacks,
                       const ListenaiI2CProfile *profile);
void listenai_i2c_reset(ListenaiI2C *s);
#endif
