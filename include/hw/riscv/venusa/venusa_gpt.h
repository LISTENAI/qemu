/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_VENUSA_GPT_H
#define HW_RISCV_VENUSA_GPT_H
#include "system/memory.h"
#include "qemu/timer.h"
#include "qemu/int128.h"

typedef struct VenusASoC VenusASoC;
typedef struct VenusAGPT VenusAGPT;
typedef struct VenusAGPTChannel {
    VenusAGPT *gpt;
    unsigned index;
    QEMUTimer *event;
    int64_t epoch;
    Int128 phase;
    uint64_t rate_n, rate_d;
    uint32_t clock, control;
    uint16_t reload, shadow, count[2], prediv;
    uint8_t divider;
    uint16_t duty[4], duty_shadow[4];
    uint32_t pwm_position;
    bool shadow_pending;
    bool channel_on, counter_on, done[2], descending[2], initialized[2];
} VenusAGPTChannel;
struct VenusAGPT {
    MemoryRegion io;
    VenusASoC *soc;
    VenusAGPTChannel channel[2];
    uint32_t count_control, mask, pending;
    bool clock_enabled;
};
void venusa_gpt_init(VenusASoC *soc);
void venusa_gpt_reset(VenusASoC *soc);
void venusa_gpt_clock(VenusASoC *soc, bool enabled);
double venusa_gpt_duty(VenusAGPT *gpt, unsigned output);
#endif
