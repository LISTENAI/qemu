/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS I2C addresses, IRQ routing and peripheral clock divider. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"

static void fail(void *opaque, hwaddr address, unsigned size,
                 bool write, uint64_t value)
{
    ArcsI2C *s = opaque;
    arcs_soc_fail(s->soc, address, size, write, value);
}
static void irq(void *opaque, bool level)
{
    ArcsI2C *s = opaque;
    arcs_soc_irq(s->soc, 43 + s->index, level);
}
static void clock_ratio(void *opaque, uint64_t *numerator, uint64_t *denominator)
{
    ArcsI2C *s = opaque;
    uint32_t divider = s->soc->sysctl.pll_regs[1];
    *numerator = arcs_hclk_hz(s->soc) * ((divider >> 5) & 15);
    *denominator = divider & 31;
    if (!*numerator || !*denominator) {
        arcs_soc_fail(s->soc, 0x46001004, 4, false, divider);
    }
}
void arcs_i2c_reset(ArcsSoC *soc, unsigned index)
{
    listenai_i2c_reset(&soc->i2c[index].controller);
}
void arcs_i2c_init(ArcsSoC *soc)
{
    static const ListenaiI2CProfile profile = {
        .fifo_depth = 8, .configuration = 1,
    };
    for (unsigned i = 0; i < 2; i++) {
        ArcsI2C *s = &soc->i2c[i];
        s->soc = soc; s->index = i;
        ListenaiI2CCallbacks callbacks = {
            .opaque = s, .fail = fail, .irq = irq, .clock = clock_ratio,
        };
        listenai_i2c_init(&s->controller, DEVICE(soc),
                          i ? "arcs-i2c1" : "arcs-i2c0",
                          0x46d00000 + i * 0x100000, callbacks, &profile);
    }
}
