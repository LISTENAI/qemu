/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "hw/irq.h"

static void fault(void *opaque, hwaddr addr, unsigned size, bool write, uint64_t value)
{
    venusa_fail(opaque, addr, size, write, value);
}
static void gpio_irq(void *opaque, unsigned first_pad, bool value)
{
    venusa_irq(opaque, 24 + first_pad / 32, value);
}
static void gpio_output(void *opaque, unsigned pad, bool value)
{
    VenusASoC *s = opaque;
    if (pad < 54) {
        venusa_i2s_pins_changed(s);
    }
    if (pad < 54) { qemu_set_irq(s->pad_output[pad], value); }
    if (pad < 54) {
        venusa_spi_pins_changed(s);
    }
    if (s->dvp.camera_clock_changed && pad < 38) {
        s->dvp.camera_clock_changed(s->dvp.camera_opaque);
    }
}
static void gpio_input(void *opaque, int pad, int value)
{
    VenusASoC *s = opaque;
    if (pad >= 54) { venusa_fail(s, 0x44100000 + pad * 4, 4, true, value); }
    s->board_inputs[pad / 32] = value ? s->board_inputs[pad / 32] | (1u << (pad % 32)) :
        s->board_inputs[pad / 32] & ~(1u << (pad % 32));
    listenai_gpio_input(&s->gpio[pad / 32], pad % 32, value);
    venusa_i2s_pad_input(s, pad, value);
    venusa_spi_pad_input(s, pad, value);
}
static void dma_irq(void *opaque, uint32_t pending)
{
    for (unsigned i = 0; i < 6; i++) { venusa_irq(opaque, 43 + i, !!(pending & (1u << i))); }
}
void venusa_dma_init(VenusASoC *s)
{
    s->dma.descriptor_writeback = true;
    listenai_dma_init(&s->dma, OBJECT(s), "venusa-dma", 0x40000000, 6,
                     dma_irq, fault, s);
}
static bool core_pad_connected(void *opaque, unsigned pad)
{
    VenusASoC *s = opaque;
    /* PB0..PB5 pass through the always-on mux before the core mux. */
    return pad < 54 && (pad < 32 || pad >= 38 ||
                        (s->aon_iomux[pad - 32] & 31) == 6);
}

void venusa_gpio_init(VenusASoC *s)
{
    qdev_init_gpio_in_named(DEVICE(s), gpio_input, "pad-in", 64);
    qdev_init_gpio_out_named(DEVICE(s), s->pad_output, "pad-out", 64);
    ListenaiGPIOCallbacks cb = { .opaque = s, .fail = fault,
        .irq = gpio_irq, .output = gpio_output };
    listenai_pinmux_init(&s->pinmux, OBJECT(s), "venusa-pinmux", 0x44100000, 54, cb);
    s->pinmux.function_mask = 31;
    s->pinmux.gpio_zero = (UINT64_C(1) << 38) - 1;
    s->pinmux.gpio_one = ((UINT64_C(1) << 54) - 1) & ~s->pinmux.gpio_zero;
    s->pinmux.pad_connected = core_pad_connected;
    s->pinmux.pad_opaque = s;
    for (unsigned i = 0; i < 2; i++) {
        char name[32];
        snprintf(name, sizeof(name), "venusa-gpio%u", i);
        listenai_gpio_init(&s->gpio[i], OBJECT(s), name, 0x43600000 + i * 0x100000,
                          32 * i, &s->pinmux, cb);
    }
}

static const uint64_t pwm_routes[8] = {
    UINT64_C(0x5001004004), UINT64_C(0xa002008008),
    UINT64_C(0x10004010010), UINT64_C(0x20008020020),
    UINT64_C(0x40110040040), UINT64_C(0x80220080080),
    UINT64_C(0x100440100001), UINT64_C(0x200880200002),
};

int venusa_pwm_channel(VenusASoC *s, unsigned pad)
{
    if (pad >= 54 || !listenai_pinmux_function(&s->pinmux, pad, 11)) { return -1; }
    for (unsigned i = 0; i < 8; i++) {
        if (pwm_routes[i] & (UINT64_C(1) << pad)) { return i; }
    }
    return -1;
}

void venusa_gpt_output(VenusASoC *s, unsigned output, bool level)
{
    assert(output < 8);
    for (unsigned pad = 0; pad < 54; pad++) {
        if (pwm_routes[output] & (UINT64_C(1) << pad)) {
            listenai_pinmux_output(&s->pinmux, pad, 11, level);
        }
    }
}
