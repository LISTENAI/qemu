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

/* Core IOMUX functions 5/6: CLK, MOSI, MISO and CS. */
static const uint64_t pin_routes[2][4] = {
    {UINT64_C(0x50208410408), UINT64_C(0x81002082040),
     UINT64_C(0x106881041022), UINT64_C(0x820440820811)},
    {UINT64_C(0x44828420804), UINT64_C(0xa0200808020),
     UINT64_C(0x110101104010), UINT64_C(0x80a08028240a)},
};

bool venusa_spi_pad_selected(void *opaque, unsigned signal, unsigned pad)
{
    VenusASPI *p = opaque;
    return signal < 4 && pad < 54 &&
           (pin_routes[p->index][signal] & (UINT64_C(1) << pad)) &&
           listenai_pinmux_function(&p->soc->pinmux, pad, 5 + p->index);
}

static int input_pad(VenusASPI *p, unsigned signal)
{
    int found = -1;
    for (unsigned pad = 0; pad < 54; pad++) {
        if (!venusa_spi_pad_selected(p, signal, pad)) { continue; }
        if (found >= 0) {
            venusa_fail(p->soc, 0x44100000 + pad * 4, 4, false,
                        listenai_pinmux_config(&p->soc->pinmux, pad));
        }
        found = pad;
    }
    return found;
}

static void pin_output(void *opaque, int pin, int level)
{
    VenusASPI *p = opaque;
    p->pin_level[pin] = level;
    uint64_t routes = pin_routes[p->index][pin];
    while (routes) {
        unsigned pad = ctz64(routes);
        routes &= routes - 1;
        if (p->pin_enabled[pin]) {
            listenai_pinmux_output(&p->soc->pinmux, pad, 5 + p->index, level);
        } else {
            listenai_pinmux_release_output(&p->soc->pinmux, pad, 5 + p->index);
        }
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
    for (unsigned clock = 0; clock < 54; clock++) {
        if (venusa_spi_pad_selected(p, LISTENAI_SPI_CLOCK, clock) &&
            listenai_gpio_wire_connected(DEVICE(p->soc), clock)) {
            input_pad(p, LISTENAI_SPI_MISO);
            return true;
        }
    }
    int miso = input_pad(p, LISTENAI_SPI_MISO);
    for (unsigned mosi = 0; miso >= 0 && mosi < 54; mosi++) {
        if (venusa_spi_pad_selected(p, LISTENAI_SPI_MOSI, mosi) &&
            listenai_gpio_wire_connects(DEVICE(p->soc), mosi, miso)) {
            return true;
        }
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

static bool required_pin(ListenaiSPI *p, unsigned pin)
{
    unsigned mode = (p->regs[0x20 / 4] >> 24) & 15;
    return pin == LISTENAI_SPI_CLOCK ||
           (pin == LISTENAI_SPI_CS && !(p->regs[0x30 / 4] & (1u << 21))) ||
           (pin == LISTENAI_SPI_MOSI && mode != 1);
}

static bool capture_route(void *opaque)
{
    VenusASPI *p = opaque;
    for (unsigned pin = 0; pin < 4; pin++) {
        p->selected_pad[pin] = required_pin(&p->device, pin) ? input_pad(p, pin) : -1;
        if (required_pin(&p->device, pin) && p->selected_pad[pin] < 0) {
            return false;
        }
    }
    return true;
}

void venusa_spi_pins_changed(VenusASoC *s)
{
    for (unsigned i = 0; i < 2; i++) {
        ListenaiSPI *p = &s->spi[i].device;
        if (!p->service || !p->active || !(p->regs[0x10 / 4] & 4)) {
            continue;
        }
        for (unsigned pin = 0; pin < 4; pin++) {
            if (!required_pin(p, pin)) {
                continue;
            }
            int pad = s->spi[i].selected_pad[pin];
            if (pad < 0 || input_pad(&s->spi[i], pin) != pad) {
                /*
                 * The asynchronous receiver's mid-word reconfiguration is
                 * not calibrated. Do not join bits across missing edges.
                 */
                venusa_fail(s, 0x44100000 + MAX(pad, 0) * 4, 4, true,
                            pad < 0 ? 0 : listenai_pinmux_config(&s->pinmux, pad));
            }
        }
    }
}

void venusa_spi_pad_input(VenusASoC *s, unsigned pad, bool level)
{
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned pin = 0; pin < 4; pin++) {
            if (venusa_spi_pad_selected(&s->spi[i], pin, pad)) {
                ListenaiSPI *p = &s->spi[i].device;
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
            listenai_dma_burst_request(&s->dma, rx, p->rx_request);
        }
        if (!(mux & (1u << tx))) {
            listenai_dma_burst_request(&s->dma, tx, p->tx_request);
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
            .slave_route = capture_route,
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
