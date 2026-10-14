/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA clock, power configuration and boot strap registers. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "exec/icount.h"
#include "qemu/error-report.h"

void venusa_hclk_ratio(VenusASoC *s, uint64_t *n, uint64_t *d)
{
    uint64_t source = 24000000, denominator = 1;
    unsigned select = s->buscfg[0] & 3;
    if (select == 2) {
        if (!(s->buscfg[2] & 1) || !(s->buscfg[2] & (1u << 23))) {
            *n = 0; *d = 1; return;
        }
        uint64_t multiplier = (uint64_t)(s->buscfg[6] & 0x1ff) << 27;
        if (s->buscfg[6] & (1u << 9)) { multiplier += s->buscfg[5] & 0x7ffffff; }
        source *= multiplier;
        denominator = (uint64_t)s->pll_core_div << 27;
    } else if (select == 3) {
        venusa_fail(s, 0x43000000, 4, true, s->buscfg[0]);
    }
    *n = source * s->hclk_n;
    *d = denominator * s->hclk_m;
}

void venusa_clock_update(VenusASoC *s)
{
    venusa_gpt_clock(s, (s->syscfg[0x28 / 4] & 0x40) != 0);
    if (!s->follow_hclk) { return; }
    uint64_t n, d;
    venusa_hclk_ratio(s, &n, &d);
    if (!n || !d || n / d > UINT32_MAX) { venusa_fail(s, 0x43000000, 4, true, s->buscfg[0]); }
    uint64_t a = n, b = d;
    while (b) { uint64_t r = a % b; a = b; b = r; }
    for (unsigned i = 0; i < 2; i++) { icount_clock_set_ratio(CPU(&s->cpu[i]), n / a, d / a); }
}

static uint32_t *configuration(VenusASoC *s, hwaddr address)
{
    if (address >= 0x450000cc && address <= 0x450000d4) {
        return &s->aon[(address - 0x45000000) / 4];
    }
    if (address >= 0x450000d8 && address <= 0x450000f0) {
        return &s->aon[(address - 0x45000000) / 4];
    }
    if (address == 0x43100010 || address == 0x43100084) {
        return &s->syscfg[(address - 0x43100000) / 4];
    }
    if (address == 0x4310000c) { return &s->syscfg[0x0c / 4]; }
    if (address == 0x431000a0) { return &s->syscfg[0xa0 / 4]; }
    if (address >= 0x43100018 && address <= 0x43100034) {
        return &s->syscfg[(address - 0x43100000) / 4];
    }
    if (address >= 0x43000000 && address <= 0x43000018) {
        return &s->buscfg[(address - 0x43000000) / 4];
    }
    if (address >= 0x45100000 && address < 0x45100038) {
        return &s->aon_iomux[(address - 0x45100000) / 4];
    }
    switch (address) {
    case 0x45000004: case 0x45000008: case 0x4500000c:
    case 0x45000010: case 0x45000018:
    case 0x45000030: case 0x45000034: case 0x45000038:
    case 0x45000040: case 0x45000044: case 0x45000048:
    case 0x45000054: case 0x45000058: case 0x45000080:
    case 0x45000084: case 0x45000090: case 0x45000094:
    case 0x45000088: case 0x450000a4: case 0x450000a8:
    case 0x450000ac: case 0x450000b0: case 0x450000c0:
    case 0x450000c4: case 0x450000c8:
        return &s->aon[(address - 0x45000000) / 4];
    }
    return NULL;
}

bool venusa_sysctl_read(VenusASoC *s, hwaddr address, uint32_t *value)
{
    uint32_t *reg = configuration(s, address);
    if (reg) {
        *value = *reg;
        return true;
    }
    switch (address) {
    case 0x43100014: *value = 0; return true;
    case 0x43100080: *value = s->calendar.wakeup; return true;
    case 0x45000014: *value = s->aon[0x14 / 4]; return true;
    case 0x45000024: *value = s->aon[0x14 / 4] & s->aon[0x18 / 4]; return true;
    case 0x45000000: *value = 0x63020000; return true;
    case 0x450000a0: *value = s->aon[0xa0 / 4] | 0x40; return true;
    case 0x431000a4: *value = 1; return true;
    case 0x45500090: *value = 0; return true; /* Unprogrammed boot option fuses. */
    case 0x4500004c: *value = s->aon[0x4c / 4]; return true;
    }
    return false;
}

bool venusa_sysctl_write(VenusASoC *s, hwaddr address, uint32_t value)
{
    uint32_t *reg = configuration(s, address);
    if (reg) {
        uint32_t previous = *reg;
        if (address == 0x43100010) {
            if (value != 0xcafe000a && value != 0xcafe000b) { return false; }
            s->core1_held = !(value & 1);
            *reg = value & 1;
            if (s->core1_held) {
                venusa_reset_cpu(s, 1);
                CPU(&s->cpu[1])->halted = true;
            } else {
                if (CPU(&s->cpu[1])->icount_hz) { icount_get(); }
                s->cpu[1].env.pc = s->syscfg[0x84 / 4];
                CPU(&s->cpu[1])->halted = false;
                qemu_cpu_kick(CPU(&s->cpu[1]));
            }
            return true;
        }
        *reg = value;
        if (address == 0x43000000 && (value & (1u << 25)) && !(previous & (1u << 25))) {
            s->hclk_n = (value >> 21) & 15;
            s->hclk_m = (value >> 16) & 31;
            if (!s->hclk_n || !s->hclk_m) { return false; }
        }
        if (address == 0x43000004 && (value & (1u << 9)) && !(previous & (1u << 9))) {
            s->pclk_n = (value >> 5) & 15;
            s->pclk_m = value & 31;
            if (!s->pclk_n || !s->pclk_m) { return false; }
        }
        if (address == 0x43000010 && (value & (1u << 23)) && !(previous & (1u << 23))) {
            s->pll_core_div = ((s->buscfg[3] >> 1) & 15) + 3;
        }
        if (address == 0x45000054) {
            listenai_aon_timer_clock(&s->aon_timer, (value & 4) != 0);
            /* WDT PCLK gating does not stop its independent RC32k counter. */
        }
        if (address == 0x45000058) {
            if (value & ~0xau) { return false; }
            if (value & 2) { listenai_aon_timer_reset(&s->aon_timer); }
            if (value & 8) { listenai_calendar_reset(&s->calendar); }
            *reg = 0;
        }
        if (address == 0x4310002c) {
            venusa_gpadc_clock(&s->gpadc, (value & 1) != 0);
        }
        if (address == 0x43100028) {
            if ((value & 0x20) && !(previous & 0x20)) {
                s->gpt_t0_div = value & 31;
                if (!s->gpt_t0_div) { return false; }
            }
            venusa_gpt_clock(s, (value & 0x40) != 0);
        }
        if (address == 0x45000090 && (value & 0x2000) && !(previous & 0x2000)) {
            warn_report("VenusA RTC calibration has no analog effect; result registers unsupported");
        }
        if (address == 0x431000a0) {
            memset(s->dma.requests, 0, sizeof(s->dma.requests));
            venusa_uart_update_dma(s);
            venusa_audio_dma_update(s);
            venusa_spi_dma_update(s);
            venusa_dvp_dma_update(s);
        }
        if (address == 0x4310001c || address == 0x43100020 ||
            (address >= 0x43000000 && address <= 0x43000018)) { venusa_spi_clock(s); }
        if (address == 0x43100034) { venusa_dvp_clock(s); }
        if (address == 0x43100034) { listenai_luna_clock(&s->luna, (value & 0x80) != 0); }
        if (address == 0x43100030 || address == 0x43100034) { venusa_audio_clock(s); }
        if (address == 0x43000008) {
            /* Functional PLL lock follows enable; analog settling is not modeled. */
            *reg = (value & ~(1u << 22)) | ((value & 1) ? 1u << 22 : 0);
        }
        if (address >= 0x43000000 && address <= 0x43000018) { venusa_clock_update(s); }
        return true;
    }
    if (address == 0x43100008) {
        if (value != 0xcafe000a || (s->syscfg[0x0c / 4] & 0x20)) { return false; }
        venusa_reset_cpu(s, 1);
        s->cpu[1].env.pc = s->syscfg[0x84 / 4];
        CPU(&s->cpu[1])->halted = s->core1_held;
        if (!s->core1_held) { qemu_cpu_kick(CPU(&s->cpu[1])); }
        return true;
    }
    if (address == 0x43100014) {
        const uint32_t supported = 7u | (1u << 21) | (1u << 27) | (3u << 10) | (3u << 12) | (1u << 26) | (3u << 29) | (1u << 15) | (3u << 5) | (1u << 9) | (15u << 17) | (1u << 14) | (3u << 7);
        if (value & ~supported) { return false; }
        for (unsigned i = 0; i < 2; i++) {
            if (value & (1u << (7 + i))) { listenai_spi_reset(&s->spi[i].device); }
        }
        if (value & (1u << 14)) { venusa_gpadc_reset(&s->gpadc); }
        if (value & (1u << 21)) { venusa_dvp_reset(s); }
        if (value & (1u << 27)) { venusa_luna_reset(s); }
        if (value & (1u << 17)) { venusa_audio_reset(s); }
        if (value & (1u << 18)) { venusa_codec_reset(s); }
        if (value & (1u << 19)) { venusa_i2s_reset(s, 1); }
        if (value & (1u << 20)) { venusa_i2s_reset(s, 0); }
        if (value & (1u << 9)) { venusa_gpt_reset(s); }
        for (unsigned i = 0; i < 2; i++) {
            if (value & (1u << (5 + i))) { listenai_i2c_reset(&s->i2c[i]); }
        }
        for (unsigned i = 0; i < 3; i++) {
            if (value & (1u << i)) { venusa_uart_reset(s, i); }
        }
        if (value & (1u << 10)) { listenai_gpio_reset(&s->gpio[1], s->board_inputs[1]); }
        if (value & (1u << 11)) { listenai_gpio_reset(&s->gpio[0], s->board_inputs[0]); }
        if (value & ((1u << 12) | (1u << 29))) { listenai_psram_reset(&s->psram_ctrl); }
        if (value & (1u << 15)) { listenai_dma_reset(&s->dma); }
        if (value & (1u << 26)) { listenai_sd_reset(&s->sd); }
        if (value & ((1u << 13) | (1u << 30))) {
            listenai_spimem_reset(&s->flash_ctrl);
            s->flash_ctrl.regs[0x7c / 4] = 0x1311;
        }
        return true;
    }
    if (address == 0x43100080) {
        if (value & ~1u) { return false; }
        s->calendar.wakeup = value;
        return true;
    }
    if (address == 0x450000a0) {
        s->aon[0xa0 / 4] = value & ~0x40u;
        return true;
    }
    if (address == 0x45000020) {
        s->aon[0x14 / 4] &= ~value;
        return true;
    }
    if (address == 0x4500004c) {
        s->aon[0x4c / 4] &= ~value;
        return true;
    }

    return false;
}

void venusa_sysctl_reset(VenusASoC *s)
{
    memset(s->syscfg, 0, sizeof(s->syscfg));
    memset(s->buscfg, 0, sizeof(s->buscfg));
    memset(s->aon, 0, sizeof(s->aon));
    memset(s->iomux, 0, sizeof(s->iomux));
    memset(s->aon_iomux, 0, sizeof(s->aon_iomux));
    listenai_pinmux_reset(&s->pinmux);
    for (unsigned i = 0; i < 2; i++) {
        listenai_gpio_reset(&s->gpio[i], s->board_inputs[i]);
    }
    s->hclk_n = s->hclk_m = s->pclk_n = s->pclk_m = 1;
    s->pll_core_div = 3;
    s->syscfg[0x10 / 4] = 1;
    s->syscfg[0x84 / 4] = 0x21000000;
    s->gpt_t0_div = 1;
    venusa_clock_update(s);
    s->aon[0x54 / 4] = 0x000bb9dc;
    s->aon[0x84 / 4] = 1;
    s->aon[0x4c / 4] = 1;
    s->aon[0xa0 / 4] = 0x18100;
}
