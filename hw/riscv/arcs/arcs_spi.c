/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS SPI clock/IRQ/DMA routing. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "hw/irq.h"
#include "hw/gpio/listenai_gpio_wire.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "system/address-spaces.h"

static uint64_t clock_rate(ArcsSPI *s)
{
    uint32_t cfg = s->soc->sysctl.common_regs[0x14 / 4 + s->index];
    uint32_t *pll = s->soc->sysctl.pll_regs;
    uint64_t num = 24000000, den = 1;
    if (!(cfg & (1u << 23))) {
        return 0;
    }
    if (cfg & (1u << 22)) {
        if (!(pll[2] & 1)) {
            return 0;
        }
        if (pll[6] & 0x100) {
            arcs_soc_fail(s->soc, 0x46100018, 4, false, pll[6]);
        }
        num *= pll[6] & 255;
        /* LS2684 SPI uses SYSPLL /12 for all reserved PERI_DIV_SEL
         * values. Verified with long TX transfers and PLL relock. */
        den = 12;
    }
    num *= (cfg >> 28) & 7;
    den *= (cfg >> 24) & 15;
    if (!num || !den) {
        arcs_soc_fail(s->soc, 0x46000014 + 4 * s->index, 4, false, cfg);
    }
    assert(LISTENAI_SPI_CLOCK_SCALE % den == 0);
    return num * (LISTENAI_SPI_CLOCK_SCALE / den);
}

static void spi_fail(void *opaque, hwaddr address, unsigned size, bool write,
                     uint64_t value)
{
    ArcsSPI *s = opaque;
    arcs_soc_fail(s->soc, address, size, write, value);
}

static void spi_irq(void *opaque, int unused, int level)
{
    ArcsSPI *s = opaque;
    arcs_soc_irq(s->soc, 46 + s->index, level);
}

static const uint64_t pin_routes[3][4] = {
    {UINT64_C(0x28202208208), UINT64_C(0x14101104104), UINT64_C(0x2000882082),
     UINT64_C(0x1000441041)},
    {UINT64_C(0x802020820), UINT64_C(0x28201208208), UINT64_C(0x14100904104),
     UINT64_C(0x2000482082)},
    {UINT64_C(0x14102104104), UINT64_C(0x2004082082), UINT64_C(0x1000841041),
     UINT64_C(0x28200608208)},
};

static bool pin_selected(ArcsSPI *s, unsigned pin, unsigned pad)
{
    return (pin_routes[s->index][pin] & (UINT64_C(1) << pad)) &&
           arcs_pinmux_function(&s->soc->pinmux[0], pad, 5 + s->index);
}

static void pin_output(void *opaque, int pin, int level)
{
    ArcsSPI *s = opaque;
    s->pin_level[pin] = level;
    for (unsigned pad = 0; pad < 64; pad++) {
        if (!(pin_routes[s->index][pin] & (UINT64_C(1) << pad))) {
            continue;
        }
        if (s->pin_enabled[pin]) {
            arcs_pinmux_output(&s->soc->pinmux[0], pad, 5 + s->index, level);
        } else {
            listenai_pinmux_release_output(&s->soc->pinmux[0], pad,
                                           5 + s->index);
        }
    }
}

static void pin_enable(void *opaque, int pin, int level)
{
    ArcsSPI *s = opaque;
    s->pin_enabled[pin] = level;
    pin_output(s, pin, s->pin_level[pin]);
}

/* Multiple simultaneous input selections have no calibrated arbitration. */
static int input_pad(ArcsSPI *s, unsigned pin)
{
    int found = -1;
    for (unsigned pad = 0; pad < 64; pad++) {
        if (!pin_selected(s, pin, pad)) {
            continue;
        }
        if (found >= 0) {
            arcs_soc_fail(s->soc, 0x47500000 + 4 * pad, 4, false,
                          listenai_pinmux_config(&s->soc->pinmux[0], pad));
        }
        found = pad;
    }
    return found;
}

static bool digital_route(ArcsSPI *s)
{
    if (((s->device.regs[0x20 / 4] >> 24) & 15) != 1) {
        s->selected_pad[LISTENAI_SPI_MISO] = input_pad(s, LISTENAI_SPI_MISO);
    }
    return true;
}

static bool pins_connected(void *opaque)
{
    ArcsSPI *s = opaque;
    for (unsigned pad = 0; pad < 64; pad++) {
        if (pin_selected(s, LISTENAI_SPI_CLOCK, pad) &&
            listenai_gpio_wire_connected(DEVICE(s->soc), pad)) {
            return digital_route(s);
        }
        if (!pin_selected(s, LISTENAI_SPI_MOSI, pad)) {
            continue;
        }
        for (unsigned input = 0; input < 64; input++) {
            if (pin_selected(s, LISTENAI_SPI_MISO, input) &&
                listenai_gpio_wire_connects(DEVICE(s->soc), pad, input)) {
                return digital_route(s);
            }
        }
    }
    return false;
}

static bool required_pin(unsigned pin, unsigned mode)
{
    return pin == LISTENAI_SPI_CLOCK || pin == LISTENAI_SPI_CS ||
           (pin == LISTENAI_SPI_MOSI && mode != 1);
}

static bool capture_route(ArcsSPI *s)
{
    unsigned mode = (s->device.regs[0x20 / 4] >> 24) & 15;
    for (unsigned pin = 0; pin < 4; pin++) {
        s->selected_pad[pin] = -1;
        if (!required_pin(pin, mode)) {
            continue;
        }
        s->selected_pad[pin] = input_pad(s, pin);
        if (s->selected_pad[pin] < 0) {
            return false;
        }
    }
    return true;
}

static void spi_cs(void *opaque, int unused, int level)
{
    ArcsSPI *s = opaque;
    qemu_set_irq(s->soc->spi_cs[s->index], level);
    if (!(s->device.regs[0x10 / 4] & 4)) {
        pin_output(s, LISTENAI_SPI_CS, level);
    }
}

void arcs_spi_pins_changed(ArcsSoC *soc)
{
    for (unsigned i = 0; i < 3; i++) {
        ArcsSPI *s = &soc->spi[i];
        ListenaiSPI *p = &s->device;
        if (p->service) {
            unsigned input =
                (p->regs[0x10 / 4] & 4) ? LISTENAI_SPI_MOSI : LISTENAI_SPI_MISO;
            for (unsigned pad = 0; pad < 64; pad++) {
                if (pin_selected(s, input, pad)) {
                    listenai_spi_pin_input(
                        p, input,
                        (soc->gpio[pad / 32].inputs >> (pad % 32)) & 1);
                    break;
                }
            }
        }
        if (!p->service || !p->active || !p->serial ||
            !(p->regs[0x10 / 4] & 4)) {
            continue;
        }
        for (unsigned pin = 0; pin < 4; pin++) {
            int pad = s->selected_pad[pin];
            if (required_pin(pin, p->mode) &&
                (pad < 0 || !pin_selected(s, pin, pad))) {
                arcs_soc_fail(
                    soc, 0x47500000 + 4 * MAX(pad, 0), 4, true,
                    pad < 0 ? 0 : listenai_pinmux_config(&soc->pinmux[0], pad));
            }
        }
    }
}

void arcs_spi_pad_input(ArcsSoC *soc, unsigned pad, bool level)
{
    for (unsigned i = 0; i < 3; i++) {
        ArcsSPI *s = &soc->spi[i];
        for (unsigned pin = 0; pin < 4; pin++) {
            if (!pin_selected(s, pin, pad)) {
                continue;
            }
            if (pin == LISTENAI_SPI_CS && !level &&
                (s->device.regs[0x10 / 4] & 4) && !capture_route(s)) {
                return;
            }
            if (pin == LISTENAI_SPI_CS && !level &&
                (s->device.regs[0x10 / 4] & 4)) {
                int mosi = s->selected_pad[LISTENAI_SPI_MOSI];
                if (mosi >= 0) {
                    /* PinMux can select an already-high pad without an edge. */
                    listenai_spi_pin_input(
                        &s->device, LISTENAI_SPI_MOSI,
                        (soc->gpio[mosi / 32].inputs >> (mosi % 32)) & 1);
                }
            }
            qemu_set_irq(
                qdev_get_gpio_in_named(DEVICE(&s->device), "pins-in", pin),
                level);
        }
    }
}

static const unsigned dma_requests[3][2] = { {10, 11}, {6, 7}, {4, 5} };

void arcs_spi_update_dma(ArcsSoC *soc)
{
    uint32_t mux = soc->sysctl.common_regs[0x94 / 4];
    for (unsigned i = 0; i < 3; i++) {
        ArcsSPI *s = &soc->spi[i];
        for (unsigned direction = 0; direction < 2; direction++) {
            unsigned request = dma_requests[i][direction];
            /* These request slots select SPI at mux value zero. */
            listenai_dma_request(&soc->dma, request,
                                 ((mux >> (2 * request)) & 3) == 0 &&
                                     s->dma_request[direction]);
        }
    }
}

static void spi_dma(void *opaque, int direction, int level)
{
    ArcsSPI *s = opaque;
    s->dma_request[direction] = level;
    arcs_spi_update_dma(s->soc);
}

static bool spi_dma_active(void *opaque)
{
    return ((ArcsSPI *)opaque)->soc->dma.servicing;
}

static uint64_t spi_clock(void *opaque)
{
    return clock_rate(opaque);
}

void arcs_spi_clock_changed(ArcsSoC *soc, unsigned index)
{
    ListenaiSPI *s = &soc->spi[index].device;
    if (s->active && (s->regs[0x10 / 4] & 4) && !clock_rate(&soc->spi[index])) {
        arcs_soc_fail(soc, 0x46000014 + index * 4, 4, true,
                      soc->sysctl.common_regs[0x14 / 4 + index]);
    }
    listenai_spi_clock_changed(s);
}

void arcs_spi_init(ArcsSoC *soc)
{
    qdev_init_gpio_out_named(DEVICE(soc), soc->spi_cs, "spi-cs", 3);
    for (unsigned i = 0; i < 3; i++) {
        ArcsSPI *s = &soc->spi[i];
        char name[16];
        s->soc = soc;
        s->index = i;
        snprintf(name, sizeof(name), "spi%u", i);
        object_initialize_child(OBJECT(soc), name, &s->device,
                                TYPE_LISTENAI_SPI);
        s->device.safe_reads = soc->safe_mmio_reads;
        s->device.hardware = (ListenaiSPIHardware){
            .tx_depth = 16,
            .rx_depth = 16,
            .feature = 0x4b33,
        };
        s->device.callbacks = (ListenaiSPICallbacks){
            .opaque = s,
            .fail = spi_fail,
            .dma_active = spi_dma_active,
            .clock_rate = spi_clock,
            .pins_connected = pins_connected,
        };
        s->device.base = 0x47000000 + i * 0x100000;
        snprintf(name, sizeof(name), "arcs-spi%u", i);
        qdev_prop_set_string(DEVICE(&s->device), "bus-name", name);
        sysbus_realize(SYS_BUS_DEVICE(&s->device), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->device), 0, s->device.base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->device), 0,
                           qemu_allocate_irq(spi_irq, s, 0));
        qdev_connect_gpio_out_named(DEVICE(&s->device), "cs", 0,
                                    qemu_allocate_irq(spi_cs, s, 0));
        for (unsigned n = 0; n < 4; n++) {
            s->selected_pad[n] = -1;
            qdev_connect_gpio_out_named(DEVICE(&s->device), "pins-out", n,
                                        qemu_allocate_irq(pin_output, s, n));
            qdev_connect_gpio_out_named(DEVICE(&s->device), "pins-enable", n,
                                        qemu_allocate_irq(pin_enable, s, n));
        }
        for (unsigned n = 0; n < 2; n++) {
            qdev_connect_gpio_out_named(DEVICE(&s->device), "dma", n,
                                        qemu_allocate_irq(spi_dma, s, n));
        }
    }
}

void arcs_spi_reset(ArcsSoC *soc, unsigned i)
{
    for (unsigned pin = 0; pin < 4; pin++) {
        soc->spi[i].selected_pad[pin] = -1;
    }
    listenai_spi_reset(&soc->spi[i].device);
}
