/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA GPADC: ideal, synchronous software-triggered voltage conversions. */
#include "qemu/osdep.h"
#include "hw/adc/venusa_gpadc.h"

#define CTRL0       0x00
#define CTRL1       0x04
#define CONFIG      0x08
#define CHSEL       0x0c
#define IMR0        0x60
#define RAW0        0x78
#define RAW1        0x7c
#define RAW2        0x80
#define ISR0        0x84
#define DMA_RDR     0x90
#define FIFO_DATA   0x94
#define FIFO_CTRL   0xd4
#define REG(s, off) ((s)->regs[(off) / 4])

static const uint32_t interrupt_bits[] = { 0x3f000f, 0x3f003f, 0x3f003f };

static void update_irq(VenusAGPADC *s)
{
    bool pending = false;
    for (unsigned n = 0; n < 3; n++) {
        pending |= (REG(s, RAW0 + n * 4) & ~REG(s, IMR0 + n * 4)) != 0;
    }
    s->callbacks.irq(s->callbacks.opaque, pending);
}

static void fail(VenusAGPADC *s, hwaddr off, unsigned size, bool write,
                 uint64_t value)
{
    s->callbacks.fail(s->callbacks.opaque, s->base + off, size, write, value);
}

static bool register_valid(hwaddr off, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4) || (off & (size - 1))) {
        return false;
    }
    off &= ~3u;
    return off <= CHSEL || off == 0x28 ||
           (off >= 0x40 && off <= 0x54) ||
           (off >= IMR0 && off <= 0x68) ||
           (off >= RAW0 && off <= 0xa8) ||
           (off >= FIFO_CTRL && off <= 0xe8);
}

static bool sample(VenusAGPADC *s, unsigned channel, uint16_t *value)
{
    unsigned selection = (REG(s, CONFIG) >> 4) & 3;
    uint32_t reference = s->callbacks.reference_uv(s->callbacks.opaque,
                                                  selection);
    if (!reference) {
        return false;
    }
    unsigned divisor = (REG(s, CONFIG) & 4) ? 3 : 1;
    *value = MIN(1023, (uint64_t)s->voltage_uv[channel] * 1024 /
                      ((uint64_t)reference * divisor));
    return true;
}

static bool convert(VenusAGPADC *s, uint32_t control)
{
    unsigned rounds = control >> 24;
    unsigned selected = REG(s, CHSEL) & 0x3f;
    uint16_t samples[VENUSA_GPADC_CHANNELS] = { 0 };

    /* Completion must be acknowledged before the next software trigger. */
    if (REG(s, RAW0) & 8) {
        return true;
    }
    /* An unbounded conversion requires timed sampling, not a finite FIFO fill. */
    if (!rounds || !s->clock_enabled) {
        return false;
    }
    for (unsigned channel = 0; channel < VENUSA_GPADC_CHANNELS; channel++) {
        if ((selected & (1u << channel)) && !sample(s, channel, &samples[channel])) {
            return false;
        }
    }
    if (!selected) {
        REG(s, RAW0) |= 1;
        update_irq(s);
        return true;
    }
    REG(s, CTRL1) |= 1;
    REG(s, RAW0) |= 2; /* The ideal converter has no analog setup delay. */
    for (unsigned channel = 0; channel < VENUSA_GPADC_CHANNELS; channel++) {
        if (!(selected & (1u << channel))) {
            continue;
        }
        unsigned threshold = REG(s, FIFO_CTRL + channel * 4) & 15;
        for (unsigned round = 0; round < rounds; round++) {
            if (s->count[channel] == VENUSA_GPADC_FIFO_DEPTH) {
                REG(s, RAW2) |= 1u << channel;
                continue;
            }
            unsigned tail = (s->head[channel] + s->count[channel]) %
                            VENUSA_GPADC_FIFO_DEPTH;
            s->fifo[channel][tail] = samples[channel];
            s->count[channel]++;
            if (s->count[channel] == VENUSA_GPADC_FIFO_DEPTH) {
                REG(s, RAW1) |= 1u << channel;
            }
            if (s->count[channel] >= threshold) {
                REG(s, RAW1) |= 1u << (channel + 16);
            }
        }
    }
    REG(s, RAW0) |= 8;
    update_irq(s);
    return true;
}

static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    VenusAGPADC *s = opaque;
    if (!register_valid(off, size)) {
        fail(s, off, size, false, 0);
        return 0;
    }
    unsigned shift = (off & 3) * 8;
    hwaddr aligned = off & ~3u;
    uint32_t value;
    if (aligned == DMA_RDR || aligned == 0x28) {
        fail(s, off, size, false, 0);
        return 0;
    } else if (aligned >= FIFO_DATA && aligned <= 0xa8) {
        unsigned channel = (aligned - FIFO_DATA) / 4;
        value = 0;
        if (s->count[channel]) {
            value = s->fifo[channel][s->head[channel]];
            s->head[channel] = (s->head[channel] + 1) % VENUSA_GPADC_FIFO_DEPTH;
            s->count[channel]--;
            if (!s->count[channel]) {
                REG(s, RAW0) |= 1u << (channel + 16);
            }
        } else {
            REG(s, RAW2) |= 1u << (channel + 16);
        }
        update_irq(s);
    } else if (aligned >= FIFO_CTRL) {
        unsigned channel = (aligned - FIFO_CTRL) / 4;
        value = REG(s, aligned) | (s->count[channel] << 8);
    } else if (aligned >= ISR0 && aligned <= 0x8c) {
        value = REG(s, aligned - 12) & ~REG(s, aligned - 36);
    } else {
        value = REG(s, aligned);
    }
    return value >> shift;
}

static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusAGPADC *s = opaque;
    if (!register_valid(off, size)) {
        fail(s, off, size, true, value);
        return;
    }
    unsigned shift = (off & 3) * 8;
    hwaddr aligned = off & ~3u;
    uint32_t mask = size == 4 ? UINT32_MAX : ((1u << (size * 8)) - 1) << shift;
    uint32_t written = value << shift;
    uint32_t merged = (REG(s, aligned) & ~mask) | (written & mask);

    if (aligned >= RAW0 && aligned <= RAW2) {
        REG(s, aligned) &= ~written;
    } else if (aligned >= IMR0 && aligned <= 0x68) {
        REG(s, aligned) = merged & interrupt_bits[(aligned - IMR0) / 4];
    } else if (aligned >= FIFO_CTRL) {
        unsigned channel = (aligned - FIFO_CTRL) / 4;
        REG(s, aligned) = merged & 15;
        if (written & 16) {
            s->head[channel] = s->count[channel] = 0;
            REG(s, RAW0) |= 1u << (channel + 16);
        }
    } else if (aligned == CTRL0) {
        if ((merged & 6) || ((written & 1) && !convert(s, merged))) {
            goto unsupported;
        }
        REG(s, CTRL0) = merged & ~0x8001u;
    } else if (aligned == CTRL1) {
        /* ADC_EN and HARD_TRIG are W1C, BUSY is read-only. */
        if (merged & (0x3f0000 | 0x70 | 8)) {
            goto unsupported;
        }
        REG(s, CTRL1) = (merged & 4) | (REG(s, CTRL1) & ~written & 3);
    } else if (aligned == CONFIG) {
        if (merged & (0x4000 | 0x380)) {
            goto unsupported;
        }
        REG(s, CONFIG) = merged & 0x3c7f;
    } else if (aligned == CHSEL) {
        if (merged & 0x3f0000) {
            goto unsupported;
        }
        REG(s, CHSEL) = merged & 0x3f;
    } else if (aligned >= 0x40 && aligned <= 0x54) {
        /* Nonzero calibration requires a separately verified transfer function. */
        if (merged) {
            goto unsupported;
        }
        REG(s, aligned) = 0;
    } else {
        goto unsupported;
    }
    update_irq(s);
    return;

unsupported:
    fail(s, off, size, true, value);
}

static const MemoryRegionOps ops = {
    .read = read_reg,
    .write = write_reg,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void venusa_gpadc_reset(VenusAGPADC *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->head, 0, sizeof(s->head));
    memset(s->count, 0, sizeof(s->count));
    REG(s, CTRL0) = 0x010a0820;
    REG(s, CONFIG) = 0x25;
    REG(s, RAW0) = 0x3f0000;
    for (unsigned n = 0; n < 3; n++) {
        REG(s, IMR0 + n * 4) = n ? 0x3f003f : 0x3f0002;
    }
    for (unsigned channel = 0; channel < VENUSA_GPADC_CHANNELS; channel++) {
        REG(s, FIFO_CTRL + channel * 4) = 8;
    }
    update_irq(s);
}

void venusa_gpadc_init(VenusAGPADC *s, Object *owner, const char *name,
                      hwaddr base, VenusAGPADCCallbacks callbacks)
{
    s->base = base;
    s->callbacks = callbacks;
    memory_region_init_io(&s->io, owner, &ops, s, name, 0x1000);
    venusa_gpadc_reset(s);
}

void venusa_gpadc_clock(VenusAGPADC *s, bool enabled)
{
    s->clock_enabled = enabled;
}

bool venusa_gpadc_set_voltage(VenusAGPADC *s, unsigned channel, uint32_t uv)
{
    if (channel >= VENUSA_GPADC_CHANNELS) {
        return false;
    }
    s->voltage_uv[channel] = uv;
    return true;
}
