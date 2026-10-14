/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ADC_VENUSA_GPADC_H
#define HW_ADC_VENUSA_GPADC_H

#include "system/memory.h"

#define VENUSA_GPADC_CHANNELS 6
#define VENUSA_GPADC_FIFO_DEPTH 16

typedef struct VenusGPADCCallbacks {
    void *opaque;
    void (*irq)(void *opaque, bool level);
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write,
                 uint64_t value);
    /* Zero means this reference source is not connected. */
    uint32_t (*reference_uv)(void *opaque, unsigned selection);
} VenusGPADCCallbacks;

typedef struct VenusGPADC {
    MemoryRegion io;
    hwaddr base;
    VenusGPADCCallbacks callbacks;
    uint32_t regs[0xec / 4];
    uint32_t voltage_uv[VENUSA_GPADC_CHANNELS];
    uint16_t fifo[VENUSA_GPADC_CHANNELS][VENUSA_GPADC_FIFO_DEPTH];
    uint8_t head[VENUSA_GPADC_CHANNELS];
    uint8_t count[VENUSA_GPADC_CHANNELS];
    bool clock_enabled;
} VenusGPADC;

void venusa_gpadc_init(VenusGPADC *s, Object *owner, const char *name,
                      hwaddr base, VenusGPADCCallbacks callbacks);
void venusa_gpadc_reset(VenusGPADC *s);
void venusa_gpadc_clock(VenusGPADC *s, bool enabled);
bool venusa_gpadc_set_voltage(VenusGPADC *s, unsigned channel, uint32_t uv);

#endif
