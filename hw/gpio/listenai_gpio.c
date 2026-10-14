/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional digital GPIO/PinMux. Analog pads and debounce are not modeled. */
#include "qemu/osdep.h"
#include "hw/gpio/listenai_gpio.h"
#include "hw/irq.h"
#include "system/address-spaces.h"

static bool gpio_register(hwaddr offset)
{
    switch (offset) {
    case 0x24: case 0x28: case 0x40: case 0x44: case 0x50:
    case 0x54: case 0x58: case 0x5c: case 0x60: case 0x70: case 0x74:
        return true;
    default: return false;
    }
}

bool listenai_pinmux_function(ListenaiPinmux *s, unsigned pin, unsigned function)
{
    assert(pin < 64);
    return (s->config[pin] & 0x1ff) == function;
}

static bool output_enabled(ListenaiPinmux *s, unsigned pin, bool direction)
{
    uint32_t config = s->config[pin];
    uint32_t function = 1u << (config & 31);
    bool peripheral = (s->peripheral_valid[pin] & function) && !(config & 0x1e0);
    /* Selector 1 is explicit GPIO; selector 0 retains the default GPIO path. */
    bool gpio = listenai_pinmux_function(s, pin, 0) || listenai_pinmux_function(s, pin, 1);
    return config & 0x400000 ? !(config & 0x200000) : peripheral || (direction && gpio);
}

static bool resolve(ListenaiPinmux *s, unsigned pin, bool direction,
                    bool output, bool undriven)
{
    uint32_t config = s->config[pin];
    uint32_t function = 1u << (config & 31);
    bool peripheral = (s->peripheral_valid[pin] & function) && !(config & 0x1e0);
    if (!output_enabled(s, pin, direction)) { return undriven; }
    if (peripheral) { output = !!(s->peripheral_level[pin] & function); }
    return config & 0x1000000 ? !!(config & 0x800000) : output;
}

void listenai_gpio_outputs_snapshot(ListenaiGPIO *s, uint32_t *driven, uint32_t *levels)
{
    *driven = *levels = 0;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t mask = 1u << i;
        bool direction = !!(s->regs[0x28 / 4] & mask);
        if (output_enabled(s->pinmux, s->first_pad + i, direction)) {
            *driven |= mask;
            if (resolve(s->pinmux, s->first_pad + i, direction,
                        !!(s->regs[0x24 / 4] & mask), false)) { *levels |= mask; }
        }
    }
}

static void gpio_outputs(ListenaiGPIO *s)
{
    for (unsigned i = 0; i < 32; i++) {
        uint32_t mask = 1u << i;
        s->callbacks.output(s->callbacks.opaque, s->first_pad + i,
                     resolve(s->pinmux, s->first_pad + i,
                             !!(s->regs[0x28 / 4] & mask),
                             !!(s->regs[0x24 / 4] & mask), !!(s->inputs & mask)));
    }
}

static unsigned mode(ListenaiGPIO *s, unsigned pin)
{
    return (s->regs[0x54 / 4 + pin / 8] >> (4 * (pin % 8))) & 7;
}

static void gpio_irq(ListenaiGPIO *s)
{
    for (unsigned i = 0; i < 32; i++) {
        unsigned trigger = mode(s, i);
        uint32_t mask = 1u << i;
        if (trigger == 2 || trigger == 3) {
            bool active = !!(s->inputs & mask) == (trigger == 2);
            s->pending = active ? s->pending | mask : s->pending & ~mask;
        }
    }
    s->callbacks.irq(s->callbacks.opaque, s->first_pad,
                 !!(s->pending & s->regs[0x50 / 4]));
}

void listenai_gpio_input(ListenaiGPIO *s, unsigned bit, bool value)
{
    assert(bit < 32);
    uint32_t mask = 1u << bit;
    bool previous = !!(s->inputs & mask);
    unsigned trigger = mode(s, bit);
    s->inputs = value ? s->inputs | mask : s->inputs & ~mask;
    if ((trigger == 5 && previous && !value) ||
        (trigger == 6 && !previous && value) ||
        (trigger == 7 && previous != !!value)) {
        s->pending |= mask;
    }
    gpio_irq(s);
    gpio_outputs(s);
}

static uint64_t gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    ListenaiGPIO *s = opaque;
    if (size != 4 || (offset & 3)) {
        s->callbacks.fail(s->callbacks.opaque, s->base + offset, size, false, 0);
        return 0;
    }
    switch (offset) {
    case 0x20: return (s->inputs & ~s->regs[0x28 / 4]) |
                      (s->regs[0x24 / 4] & s->regs[0x28 / 4]);
    case 0x64: return s->pending;
    case 0x2c: case 0x30: return 0;
    default:
        if (gpio_register(offset)) { return s->regs[offset / 4]; }
        s->callbacks.fail(s->callbacks.opaque, s->base + offset, size, false, 0);
        return 0;
    }
}

static void gpio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    ListenaiGPIO *s = opaque;
    if (size != 4 || (offset & 3)) {
        s->callbacks.fail(s->callbacks.opaque, s->base + offset, size, true, value);
        return;
    }
    switch (offset) {
    case 0x64: s->pending &= ~value; break;
    case 0x2c: s->regs[0x24 / 4] &= ~value; break;
    case 0x30: s->regs[0x24 / 4] |= value; break;
    default:
        if (!gpio_register(offset)) {
            s->callbacks.fail(s->callbacks.opaque, s->base + offset, size, true, value);
            return;
        }
        s->regs[offset / 4] = value;
        break;
    }
    gpio_irq(s);
    gpio_outputs(s);
}

static void pinmux_changed(ListenaiPinmux *s)
{
    for (unsigned i = 0; i < s->bank_count; i++) {
        gpio_outputs(s->banks[i]);
    }
}

void listenai_pinmux_output(ListenaiPinmux *s, unsigned pin, unsigned function, bool value)
{
    assert(pin < 64 && function < 32);
    if ((s->peripheral_valid[pin] & (1u << function)) &&
        !!(s->peripheral_level[pin] & (1u << function)) == value) { return; }
    s->peripheral_valid[pin] |= 1u << function;
    if (value) { s->peripheral_level[pin] |= 1u << function; }
    else { s->peripheral_level[pin] &= ~(1u << function); }
    pinmux_changed(s);
}

void listenai_pinmux_release_output(ListenaiPinmux *s, unsigned pin,
                                    unsigned function)
{
    uint32_t mask;

    assert(pin < 64 && function < 32);
    mask = 1u << function;
    if (!(s->peripheral_valid[pin] & mask)) {
        return;
    }
    s->peripheral_valid[pin] &= ~mask;
    pinmux_changed(s);
}

uint32_t listenai_pinmux_config(ListenaiPinmux *s, unsigned pin)
{
    assert(pin < s->count);
    return s->config[pin];
}

void listenai_pinmux_configure(ListenaiPinmux *s, unsigned pin, uint32_t value)
{
    assert(pin < s->count);
    s->config[pin] = value;
    pinmux_changed(s);
}

static uint64_t pinmux_read(void *opaque, hwaddr offset, unsigned size)
{
    ListenaiPinmux *s = opaque;
    if (size != 4 || (offset & 3) || offset >= s->count * sizeof(s->config[0])) {
        s->callbacks.fail(s->callbacks.opaque, s->base + offset, size, false, 0);
        return 0;
    }
    return listenai_pinmux_config(s, offset / 4);
}

static void pinmux_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    ListenaiPinmux *s = opaque;
    if (size != 4 || (offset & 3) || offset >= s->count * sizeof(s->config[0])) {
        s->callbacks.fail(s->callbacks.opaque, s->base + offset, size, true, value);
        return;
    }
    listenai_pinmux_configure(s, offset / 4, value);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read, .write = gpio_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};
static const MemoryRegionOps pinmux_ops = {
    .read = pinmux_read, .write = pinmux_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_pinmux_init_unmapped(ListenaiPinmux *s, unsigned count,
                                  ListenaiGPIOCallbacks callbacks)
{
    assert(count && count <= ARRAY_SIZE(s->config));
    assert(callbacks.fail);
    s->count = count;
    s->callbacks = callbacks;
}

void listenai_pinmux_init(ListenaiPinmux *s, Object *owner, const char *name,
                         hwaddr base, unsigned count,
                         ListenaiGPIOCallbacks callbacks)
{
    listenai_pinmux_init_unmapped(s, count, callbacks);
    s->base = base;
    memory_region_init_io(&s->io, owner, &pinmux_ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), base, &s->io);
}

void listenai_gpio_init(ListenaiGPIO *s, Object *owner, const char *name,
                       hwaddr base, unsigned first_pad, ListenaiPinmux *pinmux,
                       ListenaiGPIOCallbacks callbacks)
{
    assert(pinmux);
    assert(first_pad + 32 <= ARRAY_SIZE(pinmux->config));
    assert(pinmux->bank_count < ARRAY_SIZE(pinmux->banks));
    assert(callbacks.fail && callbacks.irq && callbacks.output);
    s->base = base;
    s->first_pad = first_pad;
    s->pinmux = pinmux;
    s->callbacks = callbacks;
    pinmux->banks[pinmux->bank_count++] = s;
    memory_region_init_io(&s->io, owner, &gpio_ops, s, name, 0x1000);
    memory_region_add_subregion(get_system_memory(), base, &s->io);
}

void listenai_pinmux_reset(ListenaiPinmux *s)
{
    memset(s->config, 0, sizeof(s->config));
}

void listenai_gpio_reset(ListenaiGPIO *s, uint32_t inputs)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->inputs = inputs;
    s->pending = 0;
    gpio_irq(s);
    gpio_outputs(s);
}
