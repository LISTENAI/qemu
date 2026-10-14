/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_SYSCTL_H
#define HW_RISCV_ARCS_SYSCTL_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "hw/timer/listenai_aon.h"
#include "hw/rtc/listenai_calendar.h"
#include "hw/timer/listenai_dual_timer.h"
typedef struct ArcsSoC ArcsSoC;
typedef struct ArcsConfigIO {
    ArcsSoC *soc;
    MemoryRegion io;
    uint32_t base;
} ArcsConfigIO;
typedef ListenaiAONTimer ArcsAONTimer;
typedef ListenaiAONWDT ArcsAONWDT;
typedef struct ArcsDualTimer {
    ListenAIDualTimer device;
    ArcsSoC *soc;
    unsigned irq;
} ArcsDualTimer;
typedef struct ArcsSysctl {
    ArcsAONTimer aon_timer;
    ArcsAONWDT aon_wdt;
    ArcsDualTimer dual_timer[2];
    ArcsConfigIO common, pll, aon, ap;
    MemoryRegion remap[2][4];
    ListenAICalendar calendar;
    ArcsConfigIO wdt[2];
    uint32_t wdt_control[2];
    bool wdt_unlocked[2];
    bool wdt_expired[2], wdt_reset_stage[2];
    QEMUTimer *wdt_timer[2];
    uint32_t common_regs[0xa0 / 4], pll_regs[17], aon_regs[0x194 / 4];
    uint32_t ap_regs[8];
    uint32_t cp_entry, rc_result;
    uint32_t reset_status, aon_wdt_reset_cause;
    QEMUTimer *rc_timer;
    bool warm_reset, rc_done;
    bool follow_hclk;
    uint32_t hclk_n, hclk_m;
    uint64_t hclk_changes;
} ArcsSysctl;

void arcs_aon_timer_init(ArcsSoC *s);
void arcs_aon_timer_reset(ArcsSoC *s);
void arcs_aon_timer_clock(ArcsSoC *s, bool enabled);
void arcs_aon_wdt_init(ArcsSoC *s);
void arcs_aon_wdt_reset(ArcsSoC *s);
void arcs_dual_timer_init(ArcsSoC *s);
void arcs_dual_timer_reset(ArcsSoC *s);
uint64_t arcs_hclk_hz(ArcsSoC *s);
void arcs_hclk_ratio(ArcsSoC *s, uint64_t *num, uint64_t *den);
void arcs_sysctl_init(ArcsSoC *s);
void arcs_sysctl_reset(ArcsSoC *s);
#endif
