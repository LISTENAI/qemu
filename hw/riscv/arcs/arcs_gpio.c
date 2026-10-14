/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS GPIO address, interrupt and pad routing. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/irq.h"

static void gpio_fail(void *opaque, hwaddr address, unsigned size,
                      bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}
static void gpio_irq(void *opaque, unsigned first_pad, bool level)
{
    arcs_soc_irq(opaque, first_pad ? 38 : 37, level);
}
static void gpio_output(void *opaque, unsigned pad, bool level)
{
    ArcsSoC *soc = opaque;
    if (pad >= 12 && pad <= 15) {
        arcs_i2s_pins_changed(soc);
    }
    arcs_spi_pins_changed(soc);
    qemu_set_irq(soc->pad_out[pad], level);
}
static void input(void *opaque, int pin, int value)
{
    ArcsSoC *soc = opaque;
    listenai_gpio_input(&soc->gpio[pin / 32], pin % 32, value);
    arcs_i2s_pad_input(soc, pin, value);
    arcs_spi_pad_input(soc, pin, value);
}
void arcs_gpio_outputs_snapshot(ArcsGPIO *s, uint32_t *driven, uint32_t *levels)
{
    listenai_gpio_outputs_snapshot(s, driven, levels);
}
void arcs_pinmux_output(ArcsPinmux *s, unsigned pin, unsigned function, bool value)
{
    listenai_pinmux_output(s, pin, function, value);
}
bool arcs_pinmux_function(ArcsPinmux *s, unsigned pin, unsigned function)
{
    return listenai_pinmux_function(s, pin, function);
}
void arcs_gpio_init(ArcsSoC *soc)
{
    ListenaiGPIOCallbacks callbacks = {
        .opaque = soc, .fail = gpio_fail, .irq = gpio_irq,
        .output = gpio_output,
    };
    qdev_init_gpio_in_named(DEVICE(soc), input, "pad-in", 64);
    qdev_init_gpio_out_named(DEVICE(soc), soc->pad_out, "pad-out", 64);
    for (unsigned i = 0; i < 2; i++) {
        listenai_pinmux_init(&soc->pinmux[i], OBJECT(soc),
                            i ? "arcs-aon-pinmux" : "arcs-pinmux",
                            i ? 0x48100000 : 0x47500000, 64, callbacks);
        listenai_gpio_init(&soc->gpio[i], OBJECT(soc),
                          i ? "arcs-gpio-b" : "arcs-gpio-a",
                          0x46700000 + i * 0x100000, i * 32,
                          &soc->pinmux[0], callbacks);
    }
}
void arcs_gpio_reset(ArcsSoC *soc)
{
    for (unsigned i = 0; i < 2; i++) {
        listenai_pinmux_reset(&soc->pinmux[i]);
        listenai_gpio_reset(&soc->gpio[i], UINT32_MAX);
    }
}
