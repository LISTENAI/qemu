/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Timed ListenAI SPI FIFO engine; board devices connect through SSI. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/ssi/listenai_spi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "migration/vmstate.h"

/* TX/RX threshold and END enables also gate and clear their event latches. */
#define SPI_ENABLE_LATCHES 0x1cu

static uint32_t spi_pending(ListenaiSPI *s)
{
    uint32_t thresholds =
        (s->count <= ((s->regs[0x30 / 4] >> 16) & 31) ? 8 : 0);
    if (s->rx_count >= ((s->regs[0x30 / 4] >> 8) & 31)) {
        thresholds |= 4;
    }
    s->pending |= thresholds & s->regs[0x38 / 4];
    return s->pending & s->regs[0x38 / 4];
}

static void latch(ListenaiSPI *s, uint32_t events)
{
    s->pending |= events & (s->regs[0x38 / 4] | ~SPI_ENABLE_LATCHES);
}

static void signals(ListenaiSPI *s)
{
    uint32_t control = s->regs[0x30 / 4];
    qemu_set_irq(s->cs,
                 control & (1u << 21) ? !!(control & (1u << 22)) : !s->active);
    for (unsigned pin = 0; pin < 4; pin++) {
        bool slave = s->regs[0x10 / 4] & 4;
        bool output = slave ? pin == LISTENAI_SPI_MISO && s->selected :
                              pin != LISTENAI_SPI_MISO;
        qemu_set_irq(s->pin_enable[pin], output);
    }
    qemu_set_irq(s->dma[0], (control & 8) && s->rx_count);
    qemu_set_irq(s->dma[1], (control & 16) && s->count < s->hardware.tx_depth);
    qemu_set_irq(s->irq, !!(spi_pending(s) & s->regs[0x38 / 4]));
}

/* Exact common scale for integer PLL and cascaded SPI dividers. Work is
 * measured in source-clock cycles, scaled by 1e9 for nanosecond accounting. */
#define SPI_CLOCK_SCALE LISTENAI_SPI_CLOCK_SCALE

static Int128 multiply(uint64_t a, uint64_t b)
{
    uint64_t lo, hi;
    mulu64(&lo, &hi, a, b);
    return int128_make128(lo, hi);
}

static uint64_t clock_rate(ListenaiSPI *s)
{
    return s->callbacks.clock_rate(s->callbacks.opaque);
}

static void account(ListenaiSPI *s, int64_t now)
{
    if (s->phase && s->rate) {
        s->work = int128_sub(s->work, multiply(now - s->updated, s->rate));
    }
    s->updated = now;
}

static bool ready(ListenaiSPI *s)
{
    if (s->serial && s->inflight) {
        return s->active;
    }
    return s->active &&
           (s->mode == 2 || s->count || (s->serial && s->tx_left)) &&
           (s->mode == 1 || s->rx_count < s->hardware.rx_depth);
}

static void schedule(ListenaiSPI *s)
{
    if (s->regs[0x10 / 4] & 4) {
        return;
    }
    if (!ready(s)) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->phase) {
        unsigned frames = s->regs[0x10 / 4] & 0x80 ? MIN(s->remaining, 4) : 1;
        unsigned divider = s->regs[0x40 / 4] & 255;
        unsigned cycles = (s->serial ? 1 : s->bits * frames) *
                          (divider == 255 ? 1 : 2 * (divider + 1));
        s->work =
            int128_add(s->work, multiply(cycles, SPI_CLOCK_SCALE * 1000000000 /
                                                     (s->serial ? 2 : 1)));
        s->phase = true;
        s->updated = now;
        s->rate = clock_rate(s);
    }
    if (s->rate && !timer_pending(s->service)) {
        Int128 rounded = int128_add(s->work, int128_make64(s->rate - 1));
        uint64_t delay =
            int128_le(s->work, int128_zero())
                ? 0
                : int128_get64(int128_divu(rounded, int128_make64(s->rate)));
        timer_mod(s->service, now + delay);
    }
}

void listenai_spi_clock_changed(ListenaiSPI *s)
{
    if (!s->phase) {
        return;
    }
    account(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->rate = clock_rate(s);
    timer_del(s->service);
    schedule(s);
}

static void fail(ListenaiSPI *s, hwaddr off, uint32_t value)
{
    s->callbacks.fail(s->callbacks.opaque, s->base + off, 4, true, value);
}

static uint8_t reverse(uint8_t byte)
{
    byte = (byte >> 4) | (byte << 4);
    byte = ((byte & 0xcc) >> 2) | ((byte & 0x33) << 2);
    return ((byte & 0xaa) >> 1) | ((byte & 0x55) << 1);
}

static void pin_drive(ListenaiSPI *s, unsigned pin, bool level)
{
    qemu_set_irq(s->pins[pin], level);
}

static bool is_slave(ListenaiSPI *s)
{
    return s->regs[0x10 / 4] & 4;
}

static void serial_cancel(ListenaiSPI *s)
{
    s->inflight = s->consumed = false;
    s->bit_index = s->half_edges = s->tx_left = s->rx_packed = 0;
    s->serial_rx = s->packed_rx = 0;
}

static void serial_format(ListenaiSPI *s)
{
    uint32_t format = s->regs[0x10 / 4];
    uint32_t transfer = s->regs[0x20 / 4] & 0x6fc00000;
    s->bits = ((format >> 8) & 31) + 1;
    s->mode = (transfer >> 24) & 15;
    if ((format & 16) || (s->bits != 8 && s->bits != 16 && s->bits != 32) ||
        ((format & 128) && s->bits != 8) || s->mode > 2 ||
        (transfer & ~0x03000000u)) {
        fail(s, 0x10, format);
    }
}

static void serial_prepare(ListenaiSPI *s)
{
    bool merge = s->regs[0x10 / 4] & 128;
    unsigned shift = s->tx_left ? (4 - s->tx_left) * 8 : 0;
    uint32_t word = s->tx_left ? s->packed_tx : s->count ? s->fifo[s->head] : 0;
    s->serial_tx = s->mode == 2 ? 0 : merge ? (word >> shift) & 255 : word;
    s->serial_rx = 0;
    s->bit_index = s->half_edges = 0;
    s->inflight = true;
    s->consumed = false;
}

static bool serial_bit(ListenaiSPI *s)
{
    unsigned bit =
        s->regs[0x10 / 4] & 8 ? s->bit_index : s->bits - 1 - s->bit_index;
    return (s->serial_tx >> bit) & 1;
}

static void serial_consume(ListenaiSPI *s)
{
    if (s->consumed) {
        return;
    }
    s->consumed = true;
    if (s->mode != 2 && !s->tx_left) {
        if (s->count) {
            s->packed_tx = s->fifo[s->head];
            s->head = (s->head + 1) % s->hardware.tx_depth;
            s->count--;
        } else {
            s->packed_tx = 0;
            latch(s, 2);
            s->regs[0x60 / 4] |= 1u << 18;
        }
        s->tx_left = s->regs[0x10 / 4] & 128 ? 4 : 1;
    }
    signals(s);
}

static void serial_flush_rx(ListenaiSPI *s)
{
    if (!s->rx_packed) {
        return;
    }
    if (s->rx_count < s->hardware.rx_depth) {
        s->rx_fifo[(s->rx_head + s->rx_count++) % s->hardware.rx_depth] =
            s->packed_rx;
    } else {
        latch(s, 1);
        s->regs[0x60 / 4] |= 1u << 17;
    }
    s->rx_packed = s->packed_rx = 0;
}

static void serial_complete(ListenaiSPI *s)
{
    bool slave = is_slave(s), merge = s->regs[0x10 / 4] & 128;
    if (s->mode != 1) {
        s->packed_rx |=
            merge ? s->serial_rx << (8 * s->rx_packed) : s->serial_rx;
        s->rx_packed++;
        if (!merge || s->rx_packed == 4 || (!slave && s->remaining == 1)) {
            serial_flush_rx(s);
        }
        if (slave) {
            s->regs[0x64 / 4]++;
        }
    }
    if (s->mode != 2) {
        if (s->tx_left) {
            s->tx_left--;
        }
        if (slave) {
            s->regs[0x68 / 4]++;
        }
    }
    s->frames++;
    s->inflight = false;
    if (!slave && !--s->remaining) {
        s->active = false;
        latch(s, 16);
    }
    signals(s);
}

static void serial_sample(ListenaiSPI *s)
{
    unsigned bit =
        s->regs[0x10 / 4] & 8 ? s->bit_index : s->bits - 1 - s->bit_index;
    s->serial_rx |= (uint32_t)s->pin_data << bit;
    s->bit_index++;
}

static void serial_master_tick(ListenaiSPI *s)
{
    bool cpha = s->regs[0x10 / 4] & 1;
    bool cpol = s->regs[0x10 / 4] & 2;
    if (!s->inflight) {
        serial_prepare(s);
        if (!cpha) {
            pin_drive(s, LISTENAI_SPI_MOSI, serial_bit(s));
        }
    }
    serial_consume(s);
    bool leading = !(s->half_edges & 1);
    if ((cpha && leading) || (!cpha && !leading && s->bit_index < s->bits)) {
        pin_drive(s, LISTENAI_SPI_MOSI, serial_bit(s));
    }
    pin_drive(s, LISTENAI_SPI_CLOCK, leading ? !cpol : cpol);
    if (leading != cpha) {
        serial_sample(s);
    }
    s->half_edges++;
    if (s->half_edges == 2 * s->bits) {
        serial_complete(s);
        if (s->active && ready(s)) {
            serial_prepare(s);
            if (!cpha) {
                pin_drive(s, LISTENAI_SPI_MOSI, serial_bit(s));
            }
        }
    }
}

void listenai_spi_pin_input(ListenaiSPI *s, unsigned pin, bool level)
{
    if (pin == LISTENAI_SPI_MISO && !is_slave(s)) {
        s->pin_data = level;
        return;
    }
    if (!is_slave(s)) {
        return;
    }
    if (pin == LISTENAI_SPI_MOSI) {
        s->pin_data = level;
        return;
    }
    if (pin == LISTENAI_SPI_CS) {
        if (s->selected == !level) {
            return;
        }
        s->selected = !level;
        if (!clock_rate(s)) {
            return;
        }
        if (level) {
            if (s->active) {
                serial_flush_rx(s);
                s->active = false;
                latch(s, 16);
                serial_cancel(s);
                signals(s);
            }
        } else {
            serial_format(s);
            serial_cancel(s);
            s->regs[0x64 / 4] = s->regs[0x68 / 4] = 0;
            s->active = s->serial = true;
            s->pin_clock = !!(s->regs[0x10 / 4] & 2);
            serial_prepare(s);
            if (!(s->regs[0x10 / 4] & 1)) {
                pin_drive(s, LISTENAI_SPI_MISO, serial_bit(s));
            }
            signals(s);
        }
        return;
    }
    if (pin != LISTENAI_SPI_CLOCK || s->pin_clock == level) {
        return;
    }
    s->pin_clock = level;
    if (!s->selected || !s->active || !clock_rate(s)) {
        return;
    }
    bool leading = level != !!(s->regs[0x10 / 4] & 2);
    bool cpha = s->regs[0x10 / 4] & 1;
    if (leading && !s->inflight) {
        serial_prepare(s);
    }
    if (leading) {
        serial_consume(s);
    }
    if (leading != cpha) {
        serial_sample(s);
        if (s->bit_index == s->bits) {
            serial_complete(s);
        }
    } else {
        if (!s->inflight) {
            serial_prepare(s);
        }
        pin_drive(s, LISTENAI_SPI_MISO, serial_bit(s));
    }
}

static void drain(void *opaque)
{
    ListenaiSPI *s = opaque;
    account(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->phase = false;
    if (!ready(s)) {
        s->work = int128_zero();
        return;
    }
    if (s->serial) {
        serial_master_tick(s);
        if (!s->active || !ready(s)) {
            s->work = int128_zero();
        }
        schedule(s);
        return;
    }
    bool merge = s->regs[0x10 / 4] & 0x80;
    bool lsb = s->regs[0x10 / 4] & 8;
    if (merge && s->bits != 8) {
        fail(s, 0x10, s->regs[0x10 / 4]);
    }
    if (s->route_valid && !s->route_valid(s->route_opaque)) {
        fail(s, 0x2c, s->fifo[s->head]);
    }
    uint32_t word = s->mode == 2 ? 0 : s->fifo[s->head];
    if (s->mode != 2) {
        s->head = (s->head + 1) % s->hardware.tx_depth;
        s->count--;
    }
    uint32_t received = 0;
    for (unsigned i = 0; i < (merge ? 4 : 1) && s->remaining; i++) {
        uint32_t frame = merge ? (word >> (8 * i)) & 255 : word;
        uint32_t result = 0;
        for (unsigned b = 0; b < s->bits / 8; b++) {
            unsigned shift = lsb ? 8 * b : s->bits - 8 - 8 * b;
            uint8_t data = frame >> shift;
            SSIBus *bus = s->board_bus ? s->board_bus(s->board_opaque) : s->bus;
            uint8_t input = ssi_transfer(bus, lsb ? reverse(data) : data);
            result |= (uint32_t)(lsb ? reverse(input) : input) << shift;
        }
        received |= merge ? result << (8 * i) : result;
        s->remaining--;
        s->frames++;
    }
    if (s->mode != 1) {
        s->rx_fifo[(s->rx_head + s->rx_count++) % s->hardware.rx_depth] =
            received;
    }
    if (!s->remaining) {
        s->active = false;
        latch(s, 16);
    }
    /* Retain sub-nanosecond credit only across contiguous FIFO words. */
    if (!s->active) {
        s->work = int128_zero();
    }
    signals(s); /* May synchronously refill from the DMA request endpoint. */
    if (!ready(s)) {
        s->work = int128_zero();
    }
    schedule(s);
}

static uint64_t spi_read(void *opaque, hwaddr off, unsigned size)
{
    ListenaiSPI *s = opaque;
    if ((off & 3) || off > 0x7c) {
        s->callbacks.fail(s->callbacks.opaque, s->base + off, size, false, 0);
        return 0;
    }
    switch (off) {
    case 0x2c: {
        if (!s->rx_count) {
            s->callbacks.fail(s->callbacks.opaque, s->base + off, size, false,
                              0);
            return 0;
        }
        uint32_t value = s->rx_fifo[s->rx_head];
        s->rx_head = (s->rx_head + 1) % s->hardware.rx_depth;
        s->rx_count--;
        signals(s);
        schedule(s);
        return value;
    }
    case 0:
        return 0x02002044;
    case 0x34:
        return s->active | (s->count << 16) | (!s->count ? 1u << 22 : 0) |
               (s->count == s->hardware.tx_depth ? 1u << 23 : 0) |
               (s->rx_count << 8) | (!s->rx_count ? 1u << 14 : 0) |
               (s->hardware.rx_depth && s->rx_count == s->hardware.rx_depth
                    ? 1u << 15
                    : 0);
    case 0x3c:
        return spi_pending(s);
    case 0x7c:
        return s->hardware.feature;
    default:
        return s->regs[off / 4];
    }
}

static bool spi_icount_read_safe(void *opaque, hwaddr off)
{
    ListenaiSPI *s = opaque;
    return s->safe_reads && off <= 0x7c && off != 0x2c && !(off & 3);
}

static void spi_write_impl(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    ListenaiSPI *s = opaque;
    if ((off & 3) || off > 0x7c) {
        fail(s, off, value);
    }
    switch (off) {
    case 0x10:
        if (s->serial && s->active && value != s->regs[off / 4]) {
            fail(s, off, value);
        }
        /* Slave data transfer is driven by external CS and SCLK. */
        s->regs[off / 4] = value;
        if (!(value & 4)) {
            pin_drive(s, LISTENAI_SPI_CLOCK, !!(value & 2));
        }
        break;
    case 0x60:
        s->regs[off / 4] =
            (value & 0x1ffff) | (s->regs[off / 4] & ~value & 0x60000);
        break;
    case 0x2c:
        if (s->count == s->hardware.tx_depth) {
            fail(s, off, value);
        }
        s->fifo[(s->head + s->count++) % s->hardware.tx_depth] = value;
        if (s->serial && s->active && s->inflight && !s->consumed) {
            serial_prepare(s);
            if (!(s->regs[0x10 / 4] & 1)) {
                pin_drive(s,
                          is_slave(s) ? LISTENAI_SPI_MISO : LISTENAI_SPI_MOSI,
                          serial_bit(s));
            }
        }
        break;
    case 0x20:
        if (s->serial && s->active && value != s->regs[off / 4]) {
            fail(s, off, value);
        }
        s->regs[off / 4] = value;
        break;
    case 0x30:
        if (s->serial && s->active && (value & 6) && !(value & 1)) {
            fail(s, off, value);
        }
        if (value & 1) {
            s->active = s->phase = false;
            s->work = int128_zero();
            timer_del(s->service);
            s->pending = 0;
            serial_cancel(s);
        }
        if (value & 5) {
            s->head = s->count = 0;
        }
        if (value & 3) {
            s->rx_head = s->rx_count = 0;
        }
        s->regs[off / 4] = value & ~7u;
        break;
    case 0x40:
        if (s->active && s->regs[off / 4] != value) {
            fail(s, off, value);
        }
        s->regs[off / 4] = value;
        break;
    case 0x38:
        s->pending &= value | ~SPI_ENABLE_LATCHES;
        s->regs[off / 4] = value;
        break;
    case 0x3c:
        s->pending &= ~value;
        break;
    case 0x24: {
        uint32_t format = s->regs[0x10 / 4];
        s->bits = ((format >> 8) & 31) + 1;
        uint32_t transfer = s->regs[0x20 / 4] & 0x6fc00000;
        s->mode = (transfer >> 24) & 15;
        if (s->active || (format & 0x14) ||
            (s->bits != 8 && s->bits != 16 && s->bits != 32) ||
            (transfer & ~0x03000000u) || s->mode > 2 ||
            (s->mode == 0 && s->regs[0x18 / 4] != s->regs[0x1c / 4])) {
            fail(s, off, value);
        }
        s->remaining = (uint64_t)s->regs[(s->mode == 2 ? 0x1c : 0x18) / 4] + 1;
        s->serial = s->callbacks.pins_connected &&
                    s->callbacks.pins_connected(s->callbacks.opaque);
        if (s->serial && !QTAILQ_EMPTY(&BUS(s->bus)->children)) {
            fail(s, off,
                 value); /* Digital peers and SSI are distinct routes. */
        }
        serial_cancel(s);
        s->active = true;
        s->pending &= ~16u;
        s->regs[off / 4] = value;
        if (s->serial) {
            serial_format(s);
            pin_drive(s, LISTENAI_SPI_CLOCK, !!(format & 2));
            if (ready(s)) {
                serial_prepare(s);
                if (!(format & 1)) {
                    pin_drive(s, LISTENAI_SPI_MOSI, serial_bit(s));
                }
            }
        }
        break;
    }
    case 0:
    case 0x34:
    case 0x64:
    case 0x68:
    case 0x7c:
        return;
    default:
        s->regs[off / 4] = value;
        break;
    }
    signals(s);
    schedule(s);
}

static void spi_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenaiSPI *s = opaque;
    bool nested = s->io_busy;
    /* A DMA request may synchronously refill this FIFO during a control
     * write. Other recursive controller writes cannot mutate outer state. */
    if (nested &&
        (off != 0x2c || (!s->callbacks.dma_active ||
                         !s->callbacks.dma_active(s->callbacks.opaque)))) {
        fail(s, off, value);
    }
    s->io_busy = true;
    spi_write_impl(opaque, off, value, size);
    s->io_busy = nested;
}

#define OPS(read_fn, write_fn, safe_fn)                                        \
    {                                                                          \
        .read = read_fn,                                                       \
        .write = write_fn,                                                     \
        .icount_read_safe = safe_fn,                                           \
        .endianness = DEVICE_LITTLE_ENDIAN,                                    \
        .valid = {.min_access_size = 1,                                        \
                  .max_access_size = 4,                                        \
                  .unaligned = true},                                          \
        .impl = {.min_access_size = 1,                                         \
                 .max_access_size = 4,                                         \
                 .unaligned = true},                                           \
    }

static const MemoryRegionOps spi_ops =
    OPS(spi_read, spi_write, spi_icount_read_safe);
static void spi_reset_state(ListenaiSPI *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->head = s->count = s->rx_head = s->rx_count = s->pending = 0;
    s->mode = 1;
    s->remaining = s->frames = 0;
    s->active = s->phase = s->serial = s->selected = false;
    s->pin_clock = s->pin_data = false;
    s->work = int128_zero();
    serial_cancel(s);
    timer_del(s->service);
}

static void spi_reset_outputs(ListenaiSPI *s)
{
    signals(s);
    pin_drive(s, LISTENAI_SPI_CLOCK, false);
    pin_drive(s, LISTENAI_SPI_MOSI, false);
    pin_drive(s, LISTENAI_SPI_MISO, false);
}

static void spi_reset_enter(Object *obj, ResetType type)
{
    spi_reset_state(LISTENAI_SPI(obj));
}

static void spi_reset_hold(Object *obj, ResetType type)
{
    spi_reset_outputs(LISTENAI_SPI(obj));
}

void listenai_spi_reset(ListenaiSPI *s)
{
    if (!resettable_is_in_reset(OBJECT(s))) {
        /* A controller reset does not assert any peripheral reset pin. */
        spi_reset_state(s);
        spi_reset_outputs(s);
    }
}

static void spi_pin_input(void *opaque, int pin, int level)
{
    listenai_spi_pin_input(opaque, pin, level);
}

static void spi_realize(DeviceState *dev, Error **errp)
{
    ListenaiSPI *s = LISTENAI_SPI(dev);
    if (!s->callbacks.fail || !s->callbacks.clock_rate ||
        !s->hardware.tx_depth || s->hardware.tx_depth > G_N_ELEMENTS(s->fifo) ||
        !s->hardware.rx_depth ||
        s->hardware.rx_depth > G_N_ELEMENTS(s->rx_fifo)) {
        error_setg(errp,
                   "ListenAI SPI requires a valid IP profile and SoC clock");
        return;
    }
    s->bus = ssi_create_bus(dev, s->bus_name);
}

static void spi_init(Object *obj)
{
    ListenaiSPI *s = LISTENAI_SPI(obj);
    SysBusDevice *busdev = SYS_BUS_DEVICE(obj);
    s->service = timer_new_ns(QEMU_CLOCK_VIRTUAL, drain, s);
    memory_region_init_io(&s->io, obj, &spi_ops, s, TYPE_LISTENAI_SPI, 0x1000);
    s->io.disable_reentrancy_guard = true;
    sysbus_init_mmio(busdev, &s->io);
    sysbus_init_irq(busdev, &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cs, "cs", 1);
    qdev_init_gpio_out_named(DEVICE(obj), s->dma, "dma", 2);
    qdev_init_gpio_out_named(DEVICE(obj), s->pins, "pins-out", 4);
    qdev_init_gpio_out_named(DEVICE(obj), s->pin_enable, "pins-enable", 4);
    qdev_init_gpio_in_named(DEVICE(obj), spi_pin_input, "pins-in", 4);
}

static void spi_finalize(Object *obj)
{
    timer_free(LISTENAI_SPI(obj)->service);
}

static const Property spi_properties[] = {
    DEFINE_PROP_STRING("bus-name", ListenaiSPI, bus_name),
};

static const VMStateDescription spi_vmstate = {
    .name = TYPE_LISTENAI_SPI,
    .unmigratable = true,
};

static void spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    dc->realize = spi_realize;
    dc->vmsd = &spi_vmstate;
    dc->user_creatable = false;
    device_class_set_props(dc, spi_properties);
    rc->phases.enter = spi_reset_enter;
    rc->phases.hold = spi_reset_hold;
}

static const TypeInfo spi_type = {
    .name = TYPE_LISTENAI_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ListenaiSPI),
    .instance_init = spi_init,
    .instance_finalize = spi_finalize,
    .class_init = spi_class_init,
};

static void spi_register_types(void)
{
    type_register_static(&spi_type);
}
type_init(spi_register_types)
