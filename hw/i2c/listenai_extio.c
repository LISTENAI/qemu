/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed EXTIO 1.3 protocol, GPIO, ADC sampling and TIM1 CH3 output. */
#include "qemu/osdep.h"
#include "hw/i2c/listenai_extio.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "qemu/module.h"

struct ListenaiEXTIO {
    I2CSlave parent_obj;
    uint8_t reg[0x77], address;
    bool pointer, data_written, pwm_high;
    uint32_t external, external_valid;
    unsigned voltage_uv[8], adc_channel, sampled;
    bool adc_sampling;
    qemu_irq outputs[32];
    QEMUTimer *adc_timer, *pwm_timer;
    uint64_t pwm_epoch, pwm_cycles, adc_epoch, adc_cycles;
};

static unsigned valid(unsigned bank)
{
    static const unsigned masks[] = {6, 0, 0x9f, 0x7f};
    return masks[bank];
}

static unsigned base(unsigned bank)
{
    return (bank + 1) * 16;
}

static unsigned word(ListenaiEXTIO *s, unsigned reg)
{
    return (s->reg[reg] << 8) | s->reg[reg + 1];
}

static bool gpio_level(ListenaiEXTIO *s, unsigned pin, bool *driven)
{
    unsigned bank = pin / 8, bit = pin % 8, b = base(bank), mask = 1u << bit;
    bool out = s->reg[b + 1] & mask;
    *driven = (s->reg[b] & mask) && (!(s->reg[b + 2] & mask) || !out);
    if (pin == 19 && s->reg[0x70]) { *driven = true; return s->pwm_high; }
    if (*driven) { return out; }
    if (s->external_valid & (1u << pin)) { return !!(s->external & (1u << pin)); }
    return (s->reg[b + 4] & mask) && (s->reg[b + 5] & mask);
}

static void publish(ListenaiEXTIO *s)
{
    for (unsigned pin = 0; pin < 32; pin++) {
        bool driven;
        if (valid(pin / 8) & (1u << (pin % 8))) {
            qemu_set_irq(s->outputs[pin], gpio_level(s, pin, &driven));
        }
    }
}

void listenai_extio_outputs(DeviceState *dev, unsigned bank, uint32_t *driven, uint32_t *levels)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(dev);
    assert(bank < 4);
    *driven = *levels = 0;
    for (unsigned bit = 0; bit < 8; bit++) {
        bool output;
        if (!(valid(bank) & (1u << bit))) { continue; }
        bool level = gpio_level(s, bank * 8 + bit, &output);
        if (output) { *driven |= 1u << bit; }
        if (level) { *levels |= 1u << bit; }
    }
}

double listenai_extio_duty(DeviceState *dev, unsigned pin)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(dev);
    if (pin == 19 && s->reg[0x70]) { return (double)word(s, 0x75) / word(s, 0x71); }
    bool driven;
    bool level = gpio_level(s, pin, &driven);
    return driven ? level : 0;
}

bool listenai_extio_voltage(DeviceState *dev, unsigned channel, unsigned uv)
{
    if (channel >= 8 || uv > 3300000) { return false; }
    LISTENAI_EXTIO(dev)->voltage_uv[channel] = uv;
    return true;
}

static uint64_t deadline(ListenaiEXTIO *s, uint64_t cycles)
{
    return s->pwm_epoch + ((__uint128_t)cycles * 1000000000 + 47999999) / 48000000;
}

static void pwm_step(void *opaque)
{
    ListenaiEXTIO *s = opaque;
    unsigned period = word(s, 0x71), high = word(s, 0x75), prescaler = word(s, 0x73);
    s->pwm_high = !s->pwm_high;
    s->pwm_cycles += (uint64_t)prescaler * (s->pwm_high ? high : period - high);
    publish(s);
    timer_mod(s->pwm_timer, deadline(s, s->pwm_cycles));
}

static bool pwm_configure(ListenaiEXTIO *s)
{
    timer_del(s->pwm_timer);
    if (!s->reg[0x70]) { publish(s); return true; }
    unsigned period = word(s, 0x71), high = word(s, 0x75), prescaler = word(s, 0x73);
    if (!period || !prescaler || high > period) { return false; }
    s->pwm_high = high != 0;
    s->pwm_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->pwm_cycles = (uint64_t)prescaler * high;
    publish(s);
    if (high && high < period) { timer_mod(s->pwm_timer, deadline(s, s->pwm_cycles)); }
    return true;
}

static unsigned sample_cycles(ListenaiEXTIO *s)
{
    static const unsigned cycles[] = {3, 9, 15, 30, 43, 57, 73, 241};
    return cycles[(s->reg[0x50] >> 1) & 7];
}

static void adc_next(ListenaiEXTIO *s)
{
    while (s->adc_channel < 8 && !(s->reg[0x51] & (1u << s->adc_channel))) { s->adc_channel++; }
    if (s->adc_channel == 8) {
        s->reg[0x52] = 1;
        if (!(s->reg[0x50] & 1)) { return; }
        s->adc_channel = 0;
        while (!(s->reg[0x51] & (1u << s->adc_channel))) { s->adc_channel++; }
    }
    s->adc_sampling = true;
    s->adc_cycles += sample_cycles(s);
    timer_mod(s->adc_timer, s->adc_epoch +
              ((__uint128_t)s->adc_cycles * 1000000000 + 5999999) / 6000000);
}

static void adc_step(void *opaque)
{
    ListenaiEXTIO *s = opaque;
    if (s->adc_sampling) {
        s->sampled = MIN(1023, (uint64_t)s->voltage_uv[s->adc_channel] * 1024 / 3300000);
        s->adc_sampling = false;
        s->adc_cycles += 11;
        timer_mod(s->adc_timer, s->adc_epoch +
                  ((__uint128_t)s->adc_cycles * 1000000000 + 5999999) / 6000000);
        return;
    }
    s->reg[0x60 + s->adc_channel * 2] = s->sampled >> 8;
    s->reg[0x61 + s->adc_channel * 2] = s->sampled;
    s->adc_channel++;
    adc_next(s);
}

static void reset(DeviceState *dev)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(dev);
    timer_del(s->adc_timer); timer_del(s->pwm_timer);
    memset(s->reg, 0, sizeof(s->reg));
    s->reg[1] = 1; s->reg[2] = 13; s->reg[3] = 2;
    s->reg[0x72] = 100; s->reg[0x73] = 0x12; s->reg[0x74] = 0xc0; s->reg[0x76] = 70;
    s->pointer = true; s->data_written = false; s->pwm_high = false;
    publish(s);
}

static bool readable(unsigned reg)
{
    if (reg >= 1 && reg <= 4) { return true; }
    if (reg == 0x50 || reg == 0x51 || reg == 0x52 || (reg >= 0x60 && reg <= 0x76)) { return true; }
    unsigned bank = reg / 16 - 1, offset = reg % 16;
    return reg >= 0x10 && reg < 0x46 && bank != 1 && offset <= 5;
}

static int event(I2CSlave *dev, enum i2c_event event)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(dev);
    if (event == I2C_START_SEND) { s->pointer = true; s->data_written = false; }
    if (event == I2C_START_RECV && !readable(s->address)) { s->reg[3] = 4; return 1; }
    return 0;
}

static int extio_send(I2CSlave *dev, uint8_t value)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(dev);
    if (s->pointer) { s->address = value; s->pointer = false; return 0; }
    if (s->data_written) { s->reg[3] = 4; return 1; }
    s->data_written = true;
    unsigned reg = s->address, bank = reg / 16 - 1, offset = reg % 16;
    if (reg == 6 && value == 1) { reset(DEVICE(s)); return 0; }
    if (reg >= 0x10 && reg < 0x46 && bank != 1 && offset <= 5 && offset != 3) {
        if (value & ~valid(bank)) { goto invalid; }
        s->reg[reg] = value;
        if (offset == 0) { s->reg[reg + 4] &= ~value; }
        if (offset == 4) { s->reg[reg - 4] &= ~value; }
        publish(s);
    } else if ((reg == 0x50 && !(value & ~15)) || reg == 0x51) {
        timer_del(s->adc_timer); s->reg[reg] = value; s->reg[0x52] = 0;
        s->adc_channel = 0;
        s->adc_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); s->adc_cycles = 0;
        static const unsigned pins[] = {2,1,20,26,27,29,30,28};
        for (unsigned i = 0; i < 8; i++) {
            if (s->reg[0x51] & (1u << i)) {
                unsigned b = base(pins[i] / 8), mask = 1u << (pins[i] % 8);
                s->reg[b] &= ~mask; s->reg[b + 4] &= ~mask;
            }
        }
        publish(s);
        if (s->reg[0x51]) { adc_next(s); }
    } else if (reg >= 0x70 && reg <= 0x76 && (reg != 0x70 || value <= 1)) {
        uint8_t previous = s->reg[reg];
        s->reg[reg] = value;
        if (!pwm_configure(s)) { s->reg[reg] = previous; pwm_configure(s); goto invalid; }
    } else { goto invalid; }
    s->reg[3] = 2;
    return 0;
invalid:
    s->reg[3] = 4;
    return 1;
}

static uint8_t extio_recv(I2CSlave *dev)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(dev);
    unsigned reg = s->address;
    if (reg >= 0x10 && reg < 0x46 && reg % 16 == 3) {
        uint32_t driven, levels;
        listenai_extio_outputs(DEVICE(s), reg / 16 - 1, &driven, &levels);
        return levels;
    }
    uint8_t value = s->reg[reg];
    if (reg == 0x52) { s->reg[reg] = 0; }
    return value;
}

static void pin_input(void *opaque, int pin, int level)
{
    ListenaiEXTIO *s = opaque;
    uint32_t mask = 1u << pin;
    if ((s->external_valid & mask) && !!(s->external & mask) == !!level) { return; }
    s->external_valid |= mask;
    s->external = level ? s->external | mask : s->external & ~mask;
    publish(s);
}

static void init(Object *obj)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(obj);
    s->adc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adc_step, s);
    s->pwm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pwm_step, s);
    qdev_init_gpio_in_named(DEVICE(s), pin_input, "pin-in", 32);
    qdev_init_gpio_out_named(DEVICE(s), s->outputs, "pin-out", 32);
}

static void finalize(Object *obj)
{
    ListenaiEXTIO *s = LISTENAI_EXTIO(obj);
    timer_free(s->adc_timer); timer_free(s->pwm_timer);
}

static void class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->event = event; ic->send = extio_send; ic->recv = extio_recv;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), reset);
}

static const TypeInfo info = {
    .name = TYPE_LISTENAI_EXTIO, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ListenaiEXTIO), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)
