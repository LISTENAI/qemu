/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS video QSPI LCD clock, IRQ and GPDMA adapter. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

/* ARCS PinMux function 30, ordered as clock, IO0, IO1, IO2, IO3.
 * Pad alternatives are chip capabilities, independent of the attached device. */
static const uint64_t pin_routes[] = {
    UINT64_C(0x800420810), UINT64_C(0x8200808088), UINT64_C(0x4101104044),
    UINT64_C(0x1002041201), UINT64_C(0x404010100),
};

bool arcs_qspi_pad_valid(unsigned signal, unsigned pad)
{
    return signal < ARRAY_SIZE(pin_routes) && pad < 64 &&
           (pin_routes[signal] & (UINT64_C(1) << pad));
}

bool arcs_qspi_pad_selected(ArcsSoC *soc, unsigned signal, unsigned pad)
{
    return arcs_qspi_pad_valid(signal, pad) &&
           arcs_pinmux_function(&soc->pinmux[0], pad, 30);
}

static void fault(void *opaque, hwaddr address, unsigned size, bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}

static uint64_t clock_rate(void *opaque)
{
    ArcsSoC *soc = opaque;
    uint32_t cfg = soc->sysctl.ap_regs[3];
    uint32_t *pll = soc->sysctl.pll_regs;
    if (!(soc->sysctl.ap_regs[2] & 0x8000) || !(cfg & 0x200000)) { return 0; }
    uint64_t hz = 24000000;
    if (cfg & 0x100000) {
        if (!(pll[2] & 1)) { return 0; }
        if (pll[6] & 0x100) { arcs_soc_fail(soc, 0x46100018, 4, false, pll[6]); }
        hz = hz * (pll[6] & 255) / 12;
    }
    unsigned n = (cfg >> 26) & 7, m = (cfg >> 22) & 15;
    if (!n || !m) { arcs_soc_fail(soc, 0x4580000c, 4, false, cfg); }
    return hz * n * (LISTENAI_SPI_CLOCK_SCALE / m);
}

static void irq(void *opaque, int pin, int level)
{
    arcs_soc_irq(opaque, 74, level);
}

static void dma(void *opaque, int pin, int level)
{
    ArcsSoC *soc = opaque;
    /* DMA_SEL.QSPI_OUT selects the video output request for handshake 0. */
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(soc), "gpdma-request", 0),
                 level && (soc->sysctl.ap_regs[0x1c / 4] & 1));
}

void arcs_qspi_clock_changed(ArcsSoC *soc)
{
    ListenaiSPI *s = &soc->qspi_lcd;
    if (!s->service) { return; }
    listenai_spi_clock_changed(s);
    dma(soc, 0, (s->regs[0x30 / 4] & 16) &&
        s->count < s->hardware.tx_depth && s->count <= ((s->regs[0x30 / 4] >> 16) & 31));
}

void arcs_qspi_init(ArcsSoC *soc)
{
    ListenaiSPI *s = &soc->qspi_lcd;
    object_initialize_child(OBJECT(soc), "qspi-lcd", s, TYPE_LISTENAI_QSPI_LCD);
    s->base = 0x4500a800;
    s->hardware = (ListenaiSPIHardware) { .tx_depth = 16, .rx_depth = 16, .feature = 0x233 };
    s->callbacks = (ListenaiSPICallbacks) { .opaque = soc, .fail = fault, .clock_rate = clock_rate };
    qdev_prop_set_string(DEVICE(s), "bus-name", "qspi-lcd");
    sysbus_realize(SYS_BUS_DEVICE(s), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, s->base);
    sysbus_connect_irq(SYS_BUS_DEVICE(s), 0, qemu_allocate_irq(irq, soc, 0));
    qdev_connect_gpio_out_named(DEVICE(s), "dma", 1, qemu_allocate_irq(dma, soc, 0));
}
