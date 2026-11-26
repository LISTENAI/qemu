/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS JPEG address, interrupt and DMA request wiring. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "qapi/error.h"
#include "hw/irq.h"

static void fault(void *opaque, hwaddr addr, unsigned size, bool write, uint64_t value)
{ arcs_soc_fail(opaque, addr, size, write, value); }
static void fatal(void *opaque, const char *message)
{ arcs_soc_fail_report(opaque, "peripheral-error"); }
static void irq(void *opaque, int n, int level)
{ arcs_soc_irq(opaque, 72, level); }
static void dma_request(void *opaque, int n, int level)
{
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(opaque), "gpdma-request", n), level);
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(opaque), "dma2d-request", n), level);
}

bool arcs_jpeg_dma_output_info(ArcsSoC *s, unsigned request,
                              uint32_t *remaining, uint32_t *input, bool *complete)
{ return request == 6 && listenai_jpeg_output_info(&s->jpeg, remaining, input, complete); }
void arcs_jpeg_clock(ArcsSoC *s, bool enabled) { listenai_jpeg_clock(&s->jpeg, enabled); }
void arcs_jpeg_reset(ArcsSoC *s) { listenai_jpeg_reset(&s->jpeg); }
void arcs_jpeg_init(ArcsSoC *s)
{
    object_initialize_child(OBJECT(s), "jpeg", &s->jpeg, TYPE_LISTENAI_JPEG);
    s->jpeg.base = 0x45001800;
    s->jpeg.opaque = s; s->jpeg.fail = fault; s->jpeg.fatal = fatal;
    SysBusDevice *bus = SYS_BUS_DEVICE(&s->jpeg);
    sysbus_realize(bus, &error_fatal);
    for (unsigned i = 0; i < 9; i++) {
        sysbus_mmio_map(bus, i, s->jpeg.base + s->jpeg.windows[i].offset);
    }
    sysbus_connect_irq(bus, 0, qemu_allocate_irq(irq, s, 0));
    for (unsigned i = 0; i < 2; i++) {
        sysbus_connect_irq(bus, 1 + i, qemu_allocate_irq(dma_request, s, 6 + i));
    }
}
