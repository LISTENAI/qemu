/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA clock tree, IRQ and common-DMA routing for the shared SPI engine. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/riscv/venusa/venusa.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "hw/gpio/listenai_gpio_wire.h"

static uint64_t spi_clock(void *opaque)
{
    VenusASPI *p = opaque;
    VenusASoC *s = p->soc;
    uint32_t cfg = s->syscfg[0x1c / 4 + p->index];
    uint64_t n = 24000000, d = 1;
    if (!(cfg & (1u << 23))) {
        return 0;
    }
    if (cfg & (1u << 22)) {
        if (!(s->buscfg[2] & 1) || !(s->buscfg[2] & (1u << 26))) {
            return 0;
        }
        /* Fractional PLL requires a wider rational rate representation. */
        if (s->buscfg[6] & (1u << 9)) {
            venusa_fail(s, 0x43000018, 4, false, s->buscfg[6]);
        }
        n *= s->buscfg[6] & 0x1ff;
        d = ((s->buscfg[3] >> 12) & 15) + 6;
        if (d > 16) {
            venusa_fail(s, 0x4300000c, 4, false, s->buscfg[3]);
        }
    }
    n *= p->n;
    d *= p->m;
    if (!n || !d || LISTENAI_SPI_CLOCK_SCALE % d) {
        venusa_fail(s, 0x4310001c + p->index * 4, 4, false, cfg);
    }
    uint64_t rate, high;
    mulu64(&rate, &high, n, LISTENAI_SPI_CLOCK_SCALE / d);
    if (high) {
        venusa_fail(s, 0x4310001c + p->index * 4, 4, false, cfg);
    }
    return rate;
}

static void fault(void *opaque, hwaddr address, unsigned size, bool write,
                  uint64_t value)
{
    venusa_fail(((VenusASPI *)opaque)->soc, address, size, write, value);
}

static void interrupt(void *opaque, int unused, int level)
{
    VenusASPI *p = opaque;
    venusa_irq(p->soc, 32 + p->index, level);
}

static const unsigned pins[2][4] = {
    {16, 19, 18, 17},
    {22, 23, 24, 21},
};

bool venusa_spi_pad_selected(void *opaque, unsigned signal, unsigned pad)
{
    VenusASPI *p = opaque;
    return signal < 4 && pad == pins[p->index][signal] &&
           listenai_pinmux_function(&p->soc->pinmux, pad, 5 + p->index);
}

static void pin_output(void *opaque, int pin, int level)
{
    VenusASPI *p = opaque;
    p->pin_level[pin] = level;
    if (p->pin_enabled[pin]) {
        listenai_pinmux_output(&p->soc->pinmux, pins[p->index][pin],
                               5 + p->index, level);
    } else {
        listenai_pinmux_release_output(&p->soc->pinmux, pins[p->index][pin],
                                       5 + p->index);
    }
}

static void pin_enable(void *opaque, int pin, int level)
{
    VenusASPI *p = opaque;
    p->pin_enabled[pin] = level;
    pin_output(p, pin, p->pin_level[pin]);
}

static bool pins_connected(void *opaque)
{
    VenusASPI *p = opaque;
    unsigned function = 5 + p->index;
    unsigned clock = pins[p->index][LISTENAI_SPI_CLOCK];
    unsigned mosi = pins[p->index][LISTENAI_SPI_MOSI];
    unsigned miso = pins[p->index][LISTENAI_SPI_MISO];
    if (listenai_pinmux_function(&p->soc->pinmux, clock, function) &&
        listenai_gpio_wire_connected(DEVICE(p->soc), clock)) {
        return true;
    }
    if (listenai_pinmux_function(&p->soc->pinmux, mosi, function) &&
        listenai_pinmux_function(&p->soc->pinmux, miso, function) &&
        listenai_gpio_wire_connects(DEVICE(p->soc), mosi, miso)) {
        return true;
    }
    return false;
}

static void chip_select(void *opaque, int unused, int level)
{
    VenusASPI *p = opaque;
    qemu_set_irq(p->cs, level);
    if (!(p->device.regs[0x10 / 4] & 4)) {
        pin_output(p, LISTENAI_SPI_CS, level);
    }
}

void venusa_spi_pins_changed(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        ListenaiSPI *p = &s->spi[i].device;
        if (!p->service || !p->active || !(p->regs[0x10 / 4] & 4)) {
            continue;
        }
        for (unsigned pin = 0; pin < 4; pin++) {
            if ((pin == LISTENAI_SPI_MOSI && p->mode == 1) ||
                pin == LISTENAI_SPI_MISO) {
                continue;
            }
            if (!listenai_pinmux_function(&s->pinmux, pins[i][pin], 5 + i)) {
                /*
                 * The asynchronous receiver's mid-word reconfiguration is
                 * not calibrated. Do not join bits across missing edges.
                 */
                venusa_fail(s, 0x44100000 + pins[i][pin] * 4, 4, true,
                            listenai_pinmux_config(&s->pinmux, pins[i][pin]));
            }
        }
    }
}

void venusa_spi_pad_input(VenusASoC *s, unsigned pad, bool level)
{
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned pin = 0; pin < 4; pin++) {
            if (pad == pins[i][pin] &&
                listenai_pinmux_function(&s->pinmux, pad, 5 + i)) {
                ListenaiSPI *p = &s->spi[i].device;
                if (pin == LISTENAI_SPI_CS && !level &&
                    (p->regs[0x10 / 4] & 4)) {
                    unsigned mode = (p->regs[0x20 / 4] >> 24) & 15;
                    for (unsigned required = 0; required < 4; required++) {
                        if ((required == LISTENAI_SPI_MOSI && mode == 1) ||
                            required == LISTENAI_SPI_MISO) {
                            continue;
                        }
                        if (!listenai_pinmux_function(
                                &s->pinmux, pins[i][required], 5 + i)) {
                            /* No input pad route at CS assertion. */
                            return;
                        }
                    }
                }
                listenai_spi_pin_input(p, pin, level);
            }
        }
    }
}

void venusa_spi_dma_update(VenusASoC *s)
{
    uint32_t mux = s->syscfg[0xa0 / 4];
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        unsigned rx = 6 + 2 * i, tx = rx + 1;
        if (!(mux & (1u << rx))) {
            listenai_dma_request(&s->dma, rx, p->rx_request);
        }
        if (!(mux & (1u << tx))) {
            listenai_dma_request(&s->dma, tx, p->tx_request);
        }
    }
}

static void requests(void *opaque, int direction, int level)
{
    VenusASPI *p = opaque;
    if (direction) {
        p->tx_request = level;
    } else {
        p->rx_request = level;
    }
    venusa_spi_dma_update(p->soc);
}

static bool dma_active(void *opaque)
{
    return ((VenusASPI *)opaque)->soc->dma.servicing;
}

void venusa_spi_clock(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        uint32_t cfg = s->syscfg[0x1c / 4 + i];
        if ((cfg & (1u << 31)) && !(p->clock_config & (1u << 31))) {
            p->n = (cfg >> 28) & 7;
            p->m = (cfg >> 24) & 15;
        }
        p->clock_config = cfg;
        if (p->device.service) {
            if (p->device.active && (p->device.regs[0x10 / 4] & 4) &&
                !spi_clock(p)) {
                venusa_fail(s, 0x4310001c + i * 4, 4, true, cfg);
            }
            listenai_spi_clock_changed(&p->device);
        }
    }
}

void venusa_spi_reset(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        p->n = p->m = 1;
        p->clock_config = 0;
        if (p->device.service) {
            listenai_spi_reset(&p->device);
        }
    }
}

void venusa_spi_init(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        VenusASPI *p = &s->spi[i];
        char name[32];
        p->soc = s;
        p->index = i;
        p->n = p->m = 1;
        snprintf(name, sizeof(name), "spi%u", i);
        object_initialize_child(OBJECT(s), name, &p->device, TYPE_LISTENAI_SPI);
        p->device.hardware = (ListenaiSPIHardware){
            .tx_depth = 8,
            .rx_depth = 8,
            .feature = 0x4322,
        };
        snprintf(name, sizeof(name), "venusa-spi%u", i);
        p->device.callbacks = (ListenaiSPICallbacks){
            .opaque = p,
            .fail = fault,
            .dma_active = dma_active,
            .clock_rate = spi_clock,
            .pins_connected = pins_connected,
        };
        p->device.base = 0x43e00000 + i * 0x100000;
        qdev_prop_set_string(DEVICE(&p->device), "bus-name", name);
        sysbus_realize(SYS_BUS_DEVICE(&p->device), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&p->device), 0, p->device.base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&p->device), 0,
                           qemu_allocate_irq(interrupt, p, 0));
        qdev_connect_gpio_out_named(DEVICE(&p->device), "cs", 0,
                                    qemu_allocate_irq(chip_select, p, 0));
        for (unsigned n = 0; n < 4; n++) {
            qdev_connect_gpio_out_named(DEVICE(&p->device), "pins-out", n,
                                        qemu_allocate_irq(pin_output, p, n));
            qdev_connect_gpio_out_named(DEVICE(&p->device), "pins-enable", n,
                                        qemu_allocate_irq(pin_enable, p, n));
        }
        for (unsigned n = 0; n < 2; n++) {
            qdev_connect_gpio_out_named(DEVICE(&p->device), "dma", n,
                                        qemu_allocate_irq(requests, p, n));
        }
        snprintf(name, sizeof(name), "spi%u-cs", i);
        qdev_init_gpio_out_named(DEVICE(s), &p->cs, name, 1);
    }
}
