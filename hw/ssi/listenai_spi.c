/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Timed ListenAI SPI FIFO engine; board devices connect through SSI. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/ssi/listenai_spi.h"
#include "system/address-spaces.h"

static uint32_t spi_pending(ListenaiSPI *s)
{
    return s->pending | (s->count <= ((s->regs[0x30 / 4] >> 16) & 31) ? 8 : 0);
}

static void signals(ListenaiSPI *s)
{
    uint32_t control = s->regs[0x30 / 4];
    s->callbacks.cs(s->callbacks.opaque,
                    control & (1u << 21) ? !!(control & (1u << 22)) : !s->active);
    s->callbacks.dma(s->callbacks.opaque, false, (control & 16) && s->count < 16);
    s->callbacks.irq(s->callbacks.opaque,
                    !!(spi_pending(s) & s->regs[0x38 / 4]));
}

/* LCM(1..15) times the SPI SYSPLL /12 branch divider. Work is
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

static void schedule(ListenaiSPI *s)
{
    if (!s->active || !s->count) { return; }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->phase) {
        unsigned frames = s->regs[0x10 / 4] & 0x80 ? MIN(s->remaining, 4) : 1;
        unsigned divider = s->regs[0x40 / 4] & 255;
        unsigned cycles = s->bits * frames * (divider == 255 ? 1 : 2 * (divider + 1));
        s->work = int128_add(s->work,
            multiply(cycles, SPI_CLOCK_SCALE * 1000000000));
        s->phase = true;
        s->updated = now;
        s->rate = clock_rate(s);
    }
    if (s->rate && !timer_pending(s->service)) {
        Int128 rounded = int128_add(s->work, int128_make64(s->rate - 1));
        uint64_t delay = int128_le(s->work, int128_zero()) ? 0 :
            int128_get64(int128_divu(rounded, int128_make64(s->rate)));
        timer_mod(s->service, now + delay);
    }
}

void listenai_spi_clock_changed(ListenaiSPI *s)
{
    if (!s->phase) { return; }
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

static void drain(void *opaque)
{
    ListenaiSPI *s = opaque;
    account(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    s->phase = false;
    if (!s->active || !s->count) { s->work = int128_zero(); return; }
    bool merge = s->regs[0x10 / 4] & 0x80;
    bool lsb = s->regs[0x10 / 4] & 8;
    if (merge && s->bits != 8) { fail(s, 0x10, s->regs[0x10 / 4]); }
    if (s->route_valid && !s->route_valid(s->route_opaque)) {
        fail(s, 0x2c, s->fifo[s->head]);
    }
    uint32_t word = s->fifo[s->head];
    s->head = (s->head + 1) % 16; s->count--;
    for (unsigned i = 0; i < (merge ? 4 : 1) && s->remaining; i++) {
        uint32_t frame = merge ? (word >> (8 * i)) & 255 : word;
        for (unsigned b = 0; b < s->bits / 8; b++) {
            uint8_t data = frame >> (lsb ? 8 * b : s->bits - 8 - 8 * b);
            ssi_transfer(s->bus, lsb ? reverse(data) : data);
        }
        s->remaining--; s->frames++;
    }
    if (!s->remaining) { s->active = false; s->pending |= 16; }
    /* Retain sub-nanosecond credit only across contiguous FIFO words. */
    if (!s->active) { s->work = int128_zero(); }
    signals(s);  /* May synchronously refill from the DMA request endpoint. */
    if (!s->count) { s->work = int128_zero(); }
    schedule(s);
}

static uint64_t spi_read(void *opaque, hwaddr off, unsigned size)
{
    ListenaiSPI *s = opaque;
    if ((off & 3) || off > 0x7c || off == 0x2c) {
        s->callbacks.fail(s->callbacks.opaque, s->base + off, size, false, 0);
        return 0;
    }
    switch (off) {
    case 0: return 0x02002044;
    case 0x34: return s->active | (s->count << 16) | (!s->count ? 1u << 22 : 0) |
                      (s->count == 16 ? 1u << 23 : 0) | (1u << 14);
    case 0x3c: return spi_pending(s);
    case 0x7c: return 0x33;
    default: return s->regs[off / 4];
    }
}

static bool spi_icount_read_safe(void *opaque, hwaddr off)
{
    ListenaiSPI *s = opaque;
    return s->safe_reads && off <= 0x7c && off != 0x2c && !(off & 3);
}

static void spi_write_impl(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenaiSPI *s = opaque;
    if ((off & 3) || off > 0x7c) { fail(s, off, value); }
    switch (off) {
    case 0x2c:
        if (s->count == 16) { fail(s, off, value); }
        s->fifo[(s->head + s->count++) % 16] = value;
        break;
    case 0x30:
        if (value & 8) { fail(s, off, value); }
        if (value & 1) {
            s->active = s->phase = false; s->work = int128_zero();
            timer_del(s->service); s->pending = 0;
        }
        if (value & 5) { s->head = s->count = 0; }
        s->regs[off / 4] = value & ~7u;
        break;
    case 0x40:
        if (s->active && s->regs[off / 4] != value) { fail(s, off, value); }
        s->regs[off / 4] = value;
        break;
    case 0x3c: s->pending &= ~value; break;
    case 0x24: {
        uint32_t format = s->regs[0x10 / 4];
        s->bits = ((format >> 8) & 31) + 1;
        if (s->active || (format & 0x14) ||
            (s->bits != 8 && s->bits != 16 && s->bits != 32) ||
            (s->regs[0x20 / 4] & 0x6fc00000) != 0x01000000) { fail(s, off, value); }
        s->remaining = (uint64_t)s->regs[0x18 / 4] + 1;
        s->active = true; s->pending &= ~16u; s->regs[off / 4] = value;
        break;
    }
    case 0: case 0x34: case 0x7c: return;
    default: s->regs[off / 4] = value; break;
    }
    signals(s); schedule(s);
}

static void spi_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ListenaiSPI *s = opaque;
    bool nested = s->io_busy;
    /* A DMA request may synchronously refill this FIFO during a control
     * write. Other recursive controller writes cannot mutate outer state. */
    if (nested && (off != 0x2c || (!s->callbacks.dma_active || !s->callbacks.dma_active(s->callbacks.opaque)))) { fail(s, off, value); }
    s->io_busy = true;
    spi_write_impl(opaque, off, value, size);
    s->io_busy = nested;
}

#define OPS(read_fn, write_fn, safe_fn) { \
    .read = read_fn, .write = write_fn, .icount_read_safe = safe_fn, \
    .endianness = DEVICE_LITTLE_ENDIAN, \
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true }, \
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true }, \
}
static const MemoryRegionOps spi_ops = OPS(spi_read, spi_write, spi_icount_read_safe);
void listenai_spi_init(ListenaiSPI *s, DeviceState *owner, const char *name,
                       hwaddr base, ListenaiSPICallbacks callbacks)
{
    assert(callbacks.fail && callbacks.irq && callbacks.dma &&
           callbacks.cs && callbacks.clock_rate);
    s->base = base; s->callbacks = callbacks;
    s->bus = ssi_create_bus(owner, name);
    s->service = timer_new_ns(QEMU_CLOCK_VIRTUAL, drain, s);
    memory_region_init_io(&s->io, OBJECT(owner), &spi_ops, s, name, 0x1000);
    s->io.disable_reentrancy_guard = true;
    memory_region_add_subregion(get_system_memory(), base, &s->io);
}
void listenai_spi_reset(ListenaiSPI *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->head = s->count = s->pending = 0;
    s->remaining = s->frames = 0;
    s->active = s->phase = false; s->work = int128_zero();
    timer_del(s->service); signals(s);
}
