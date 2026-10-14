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
static void adc_irq(void *opaque, bool level)
{
    venusa_irq(opaque, 23, level);
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
    VenusGPADCCallbacks adc_cb = { .opaque = s, .irq = adc_irq,
        .fail = calendar_fault, .reference_uv = adc_reference };
    venusa_gpadc_init(&s->gpadc, OBJECT(s), "venusa-gpadc", 0x43500000, adc_cb);
    memory_region_add_subregion(get_system_memory(), 0x43500000, &s->gpadc.io);
    qdev_init_gpio_in_named(DEVICE(s), adc_voltage, "adc-voltage-uv", 6);
    venusa_gpt_init(s);
    listenai_calendar_init(&s->calendar, OBJECT(s), "venusa-calendar", 0x43400000,
                          calendar_irq, calendar_fault, s);
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
        listenai_i2c_init(&s->i2c[i], DEVICE(s), name, 0x43b00000 + i * 0x100000, cb);
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
