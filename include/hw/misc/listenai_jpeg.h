/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_LISTENAI_JPEG_H
#define HW_MISC_LISTENAI_JPEG_H
#include "hw/sysbus.h"
#include "qemu/timer.h"
#define TYPE_LISTENAI_JPEG "listenai-jpeg"
OBJECT_DECLARE_SIMPLE_TYPE(ListenaiJPEG, LISTENAI_JPEG)

typedef struct ListenaiJPEGWindow {
    MemoryRegion io;
    ListenaiJPEG *jpeg;
    unsigned offset;
} ListenaiJPEGWindow;

struct ListenaiJPEG {
    SysBusDevice parent_obj;
    ListenaiJPEGWindow windows[9];
    hwaddr base;
    void *opaque;
    void (*fail)(void *, hwaddr, unsigned, bool, uint64_t);
    void (*fatal)(void *, const char *);
    QEMUTimer *event;
    qemu_irq irq, ecs_request, pixel_request;
    uint32_t regs[0x6000 / 4];
    GByteArray *input;
    uint8_t *output;
    uint32_t received, consumed, output_size;
    int64_t remaining_ns, deadline;
    bool clock, active, input_started, output_started, decoded;
    const char *failure;
    int host_error;
};
bool listenai_jpeg_output_info(ListenaiJPEG *, uint32_t *, uint32_t *, bool *);
bool listenai_jpeg_request(ListenaiJPEG *, bool pixels);
void listenai_jpeg_reset(ListenaiJPEG *);
void listenai_jpeg_clock(ListenaiJPEG *, bool);
#endif
