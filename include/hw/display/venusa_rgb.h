/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DISPLAY_VENUSA_RGB_H
#define HW_DISPLAY_VENUSA_RGB_H
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/int128.h"

/* Digital RGB controller, independent of any external panel. */
typedef struct VenusARGBCallbacks {
    void *opaque;
    void (*irq)(void *opaque, bool level);
    void (*fail)(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value);
    void (*request_changed)(void *opaque);
} VenusARGBCallbacks;

typedef struct VenusARGB {
    MemoryRegion io;
    hwaddr base;
    VenusARGBCallbacks cb;
    QEMUTimer *event;
    uint32_t regs[10];
    uint8_t fifo[128];
    unsigned head, count, fifo_capacity;
    uint8_t *pixels;
    unsigned width, height, x, y, phase, batch;
    uint64_t clock_n, clock_d, frames, invalid_frames, underflow_pixels;
    Int128 clock_work;
    int64_t event_epoch;
    bool clock_enabled, active, valid_frame, starved, syncing;
    char *capture_path;
} VenusARGB;

void venusa_rgb_init(VenusARGB *s, Object *owner, hwaddr base, VenusARGBCallbacks cb);
void venusa_rgb_reset(VenusARGB *s);
void venusa_rgb_clock(VenusARGB *s, bool enabled, uint64_t numerator, uint64_t denominator);
bool venusa_rgb_request(VenusARGB *s);
#endif
