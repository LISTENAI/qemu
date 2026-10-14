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
    if (pad < 54) { qemu_set_irq(s->pad_output[pad], value); }
}
static void gpio_input(void *opaque, int pad, int value)
{
    VenusASoC *s = opaque;
    if (pad >= 54) { venusa_fail(s, 0x44100000 + pad * 4, 4, true, value); }
    s->board_inputs[pad / 32] = value ? s->board_inputs[pad / 32] | (1u << (pad % 32)) :
        s->board_inputs[pad / 32] & ~(1u << (pad % 32));
    listenai_gpio_input(&s->gpio[pad / 32], pad % 32, value);
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
void venusa_gpio_init(VenusASoC *s)
{
    qdev_init_gpio_in_named(DEVICE(s), gpio_input, "pad-in", 64);
    qdev_init_gpio_out_named(DEVICE(s), s->pad_output, "pad-out", 64);
    ListenaiGPIOCallbacks cb = { .opaque = s, .fail = fault,
        .irq = gpio_irq, .output = gpio_output };
    listenai_pinmux_init(&s->pinmux, OBJECT(s), "venusa-pinmux", 0x44100000, 54, cb);
    for (unsigned i = 0; i < 2; i++) {
        char name[32];
        snprintf(name, sizeof(name), "venusa-gpio%u", i);
        listenai_gpio_init(&s->gpio[i], OBJECT(s), name, 0x43600000 + i * 0x100000,
                          32 * i, &s->pinmux, cb);
    }
}

void venusa_gpt_output(VenusASoC *s, unsigned output, bool level)
{
    /* Verified SoC mux route: PWM0 to PA14, alternate function 11. */
    if (output == 0) {
        listenai_pinmux_output(&s->pinmux, 14, 11, level);
    }
}
