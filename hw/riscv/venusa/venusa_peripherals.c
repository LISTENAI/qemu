/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/riscv/venusa/venusa.h"

static void fault(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value)
{
    VenusAPeripheral *p = opaque;
    venusa_fail(p->soc, address, size, write, value);
}
static void timer_irq(void *opaque, bool level)
{
    VenusAPeripheral *p = opaque;
    venusa_irq(p->soc, 20 + p->index, level);
}
static void calendar_irq(void *opaque, bool level)
{
    venusa_irq(opaque, 22, level);
}
static void calendar_fault(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value)
{
    venusa_fail(opaque, address, size, write, value);
}
static void i2c_irq(void *opaque, bool level)
{
    VenusAPeripheral *p = opaque;
    venusa_irq(p->soc, 29 + p->index, level);
}
static void i2c_clock(void *opaque, uint64_t *n, uint64_t *d)
{
    VenusAPeripheral *p = opaque;
    VenusASoC *s = p->soc;
    venusa_hclk_ratio(s, n, d);
    *n *= s->pclk_n;
    *d *= s->pclk_m;
    if (!(s->syscfg[0x34 / 4] & (1u << p->index)) || !*n) {
        venusa_fail(s, 0x43b00000 + p->index * 0x100000, 4, true, 0);
    }
    uint64_t a = *n, b = *d;
    while (b) { uint64_t r = a % b; a = b; b = r; }
    *n /= a; *d /= a;
}
/* Controller, SCL/SDA, functions 7/8. The last SDIO pads swap selectors. */
static const uint64_t i2c_routes[2][2][2] = {
    {{UINT64_C(0xa96a3895555), UINT64_C(0x500000000000)},
     {UINT64_C(0x15695c76aaaa), UINT64_C(0xa00000000000)}},
    {{UINT64_C(0xa00000000000), UINT64_C(0x52d4712aaaa)},
     {UINT64_C(0x400000000000), UINT64_C(0xad2b8ed5555)}},
};

int venusa_i2c_bus(unsigned scl, unsigned sda)
{
    int found = -1;
    if (scl >= 54 || sda >= 54 || scl == sda) { return -1; }
    for (unsigned i = 0; i < 2; i++) {
        if (((i2c_routes[i][0][0] | i2c_routes[i][0][1]) & (UINT64_C(1) << scl)) &&
            ((i2c_routes[i][1][0] | i2c_routes[i][1][1]) & (UINT64_C(1) << sda))) {
            /* A board without a controller link must identify one bus. */
            if (found >= 0) { return -1; }
            found = i;
        }
    }
    return found;
}

bool venusa_i2c_route(VenusASoC *s, unsigned scl, unsigned sda)
{
    int bus = venusa_i2c_bus(scl, sda);
    if (bus < 0) { return false; }
    unsigned pads[] = {scl, sda};
    for (unsigned signal = 0; signal < 2; signal++) {
        bool selected = false;
        for (unsigned f = 0; f < 2; f++) {
            selected |= (i2c_routes[bus][signal][f] & (UINT64_C(1) << pads[signal])) &&
                        listenai_pinmux_function(&s->pinmux, pads[signal], 7 + f);
        }
        if (!selected) { return false; }
    }
    return true;
}

static void adc_irq(void *opaque, bool level)
{
    venusa_irq(opaque, 23, level);
}
static bool adc_connected(void *opaque, unsigned channel)
{
    VenusASoC *s = opaque;
    /* VIN0..3 are PB0..3, AON function 4 with analog selector zero. */
    return channel >= 4 || (s->aon_iomux[channel] & 0x1ff) == 4;
}

static uint32_t adc_reference(void *opaque, unsigned selection)
{
    VenusASoC *s = opaque;
    /* Ideal board supplies; the internal VDD_IO divider is programmable. */
    if (selection < 2) {
        return 1200000;
    }
    return selection == 2 ? 3300000 / ((s->aon[0x84 / 4] & 2) ? 2 : 3) : 0;
}
static void adc_voltage(void *opaque, int channel, int voltage)
{
    VenusASoC *s = opaque;
    if (voltage < 0 || !venusa_gpadc_set_voltage(&s->gpadc, channel, voltage)) {
        venusa_fail(s, 0x4350000c, 4, true, voltage);
    }
}
void venusa_peripherals_init(VenusASoC *s)
{
    VenusAGPADCCallbacks adc_cb = { .opaque = s, .irq = adc_irq,
        .fail = calendar_fault, .reference_uv = adc_reference,
        .input_connected = adc_connected };
    venusa_gpadc_init(&s->gpadc, OBJECT(s), "venusa-gpadc", 0x43500000, adc_cb);
    memory_region_add_subregion(get_system_memory(), 0x43500000, &s->gpadc.io);
    qdev_init_gpio_in_named(DEVICE(s), adc_voltage, "adc-voltage-uv", 6);
    venusa_gpt_init(s);
    listenai_calendar_init(&s->calendar, OBJECT(s), "venusa-calendar", 0x43400000,
                          calendar_irq, calendar_fault, s);
    s->calendar.allow_zero_date = true;
    for (unsigned i = 0; i < 2; i++) {
        s->dual_context[i] = (VenusAPeripheral){ s, i };
        s->i2c_context[i] = (VenusAPeripheral){ s, i };
        char name[32];
        snprintf(name, sizeof(name), "venusa-dual-timer%u", i);
        listenai_dual_timer_init(&s->dual_timer[i], OBJECT(s), name,
            0x43200000 + i * 0x100000, 62500, timer_irq, fault, &s->dual_context[i]);
        snprintf(name, sizeof(name), "venusa-i2c%u", i);
        ListenaiI2CCallbacks cb = { .opaque = &s->i2c_context[i], .fail = fault,
            .irq = i2c_irq, .clock = i2c_clock };
        static const ListenaiI2CProfile profile = {
            .fifo_depth = 16, .configuration = 3,
            .identification = 0x02021011, .directional_half = true,
        };
        listenai_i2c_init(&s->i2c[i], DEVICE(s), name,
                         0x43b00000 + i * 0x100000, cb, &profile);
    }
}
void venusa_peripherals_reset(VenusASoC *s)
{
    venusa_gpadc_reset(&s->gpadc);
    venusa_gpadc_clock(&s->gpadc, (s->syscfg[0x2c / 4] & 1) != 0);
    venusa_gpt_reset(s);
    venusa_gpt_clock(s, (s->syscfg[0x28 / 4] & 0x40) != 0);
    if (!s->reset_preserve_aon) { listenai_calendar_reset(&s->calendar); }
    for (unsigned i = 0; i < 2; i++) {
        listenai_dual_timer_reset(&s->dual_timer[i]);
        listenai_i2c_reset(&s->i2c[i]);
    }
}
