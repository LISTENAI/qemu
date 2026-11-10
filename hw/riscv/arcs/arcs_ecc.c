/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS CLUE public-key engine, fixed ROM entries used by the SDK. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "system/address-spaces.h"

#define BASE 0x44010000
#define IRQ 22
#define CNTL 0x00
#define NEXT_FETCH 0x04
#define RET_CODE 0x08
#define STACK 0x10
#define CONFIG 0x1c
#define STAT 0x20
#define FLAGS 0x24
#define IE 0x40
#define GO (UINT32_C(1) << 31)
#define BUSY GO
#define IRQ_PENDING (UINT32_C(1) << 30)
#define ZERO (UINT32_C(1) << 28)
#define RADIX_MASK 0x7ff
#define A 0x0400
#define B 0x0800
#define C 0x0c00
#define D 0x1000

enum {
    MMULT = 0x0a, MADD, MSUB, MDIV, MINV, MOD, MP, RI, R2, PMULT, PDBL,
    PADD = 0x16, PVER = 0x18,
};

static void update_irq(ArcsECC *s)
{
    arcs_soc_irq(s->soc, IRQ, s->pending && (s->ie & IRQ_PENDING));
}

static ArcsECCInt operand(const ArcsECC *s, unsigned bank, unsigned slot)
{
    ArcsECCInt value = { 0 };
    memcpy(value.w, &s->ram[(bank - A + slot * s->stride) / 4],
           s->field.words * sizeof(value.w[0]));
    return value;
}

static void store_operand(ArcsECC *s, unsigned bank, unsigned slot,
                          ArcsECCInt value)
{
    memcpy(&s->ram[(bank - A + slot * s->stride) / 4], value.w,
           s->field.words * sizeof(value.w[0]));
}

static ArcsECCPoint point(const ArcsECC *s, unsigned slot)
{
    return (ArcsECCPoint){ .x = operand(s, A, slot), .y = operand(s, B, slot) };
}

static bool point_operation(ArcsECC *s)
{
    const ArcsECCField *f = &s->field;
    ArcsECCInt a = arcs_ecc_reduce(f, operand(s, A, 6));
    ArcsECCInt b = arcs_ecc_reduce(f, operand(s, A, 7));
    ArcsECCPoint p = point(s, 2), r;

    s->zero = arcs_ecc_point_valid(f, a, b, p);
    if (s->entry == PVER) {
        ArcsECCInt radix = arcs_ecc_power_of_two(f, f->words * 32);

        /* The ROM leaves both coordinates in Montgomery form. */
        store_operand(s, A, 2,
                      arcs_ecc_mul(f, arcs_ecc_reduce(f, p.x), radix));
        store_operand(s, B, 2,
                      arcs_ecc_mul(f, arcs_ecc_reduce(f, p.y), radix));
        return true;
    }
    if (!s->zero) {
        return false;
    }
    s->zero = false;
    if (s->entry == PMULT) {
        ArcsECCInt scalar = operand(s, D, 7);
        const ArcsECCInt small = { 0 };

        /*
         * LS2684 ROM can stop on small scalars (observed for 2 and P-256).
         * The stop conditions are not characterized for this range.
         */
        if ((scalar.w[0] <= 2 &&
             !memcmp(scalar.w + 1, small.w + 1,
                     sizeof(scalar.w) - sizeof(scalar.w[0]))) ||
            !arcs_ecc_point_mul(f, a, p, scalar, &r)) {
            return false;
        }
    } else {
        ArcsECCPoint q = s->entry == PDBL ? p : point(s, 3);
        if (!arcs_ecc_point_valid(f, a, b, q) ||
            !arcs_ecc_point_add(f, a, p, q, &r)) {
            return false;
        }
    }
    /* The SDK's affine return slots do not describe a point at infinity. */
    if (r.infinity) {
        return false;
    }
    store_operand(s, A, 2, r.x);
    store_operand(s, B, 2, r.y);
    return true;
}

static bool execute(ArcsECC *s)
{
    const ArcsECCField *f = &s->field;
    ArcsECCInt x = arcs_ecc_reduce(f, operand(s, A, 0));
    ArcsECCInt r;

    if (s->entry >= PMULT) {
        return point_operation(s);
    }
    switch (s->entry) {
    case RI:
        r = arcs_ecc_power_of_two(f, f->words * 32);
        if (!arcs_ecc_inv(f, r, &r)) {
            return false;
        }
        store_operand(s, D, 2, r);
        s->zero = true;
        return true;
    case MP:
        store_operand(s, D, 1, arcs_ecc_montgomery_parameter(f));
        s->zero = true;
        return true;
    case R2: {
        ArcsECCInt radix = arcs_ecc_power_of_two(f, f->words * 32);
        ArcsECCInt inverse = arcs_ecc_reduce(f, operand(s, D, 2));
        const ArcsECCInt one = { .w = { 1 } };

        r = arcs_ecc_mul(f, radix, inverse);
        if (memcmp(r.w, one.w, sizeof(r.w))) {
            return false;
        }
        r = arcs_ecc_mul(f, radix, radix);
        store_operand(s, D, 2, r);
        store_operand(s, D, 3, r);
        s->zero = true;
        return true;
    }
    case MMULT:
        r = arcs_ecc_mul(f, x, arcs_ecc_reduce(f, operand(s, B, 0)));
        break;
    case MADD:
        r = arcs_ecc_add(f, x, arcs_ecc_reduce(f, operand(s, B, 0)));
        break;
    case MSUB:
        r = arcs_ecc_sub(f, x, arcs_ecc_reduce(f, operand(s, B, 0)));
        break;
    case MDIV:
    case MINV:
        if (!arcs_ecc_inv(f, x, &r)) {
            return false;
        }
        if (s->entry == MDIV) {
            r = arcs_ecc_mul(f, arcs_ecc_reduce(f, operand(s, C, 0)), r);
        }
        store_operand(s, C, 0, r);
        return true;
    case MOD:
        r = arcs_ecc_reduce(f, operand(s, C, 0));
        break;
    default:
        g_assert_not_reached();
    }
    store_operand(s, A, 0, r);
    return true;
}

static void complete(void *opaque)
{
    ArcsECC *s = opaque;
    assert(s->busy && s->clock);
    if (!execute(s)) {
        arcs_soc_fail(s->soc, BASE + CNTL, 4, true, s->control | GO);
    }
    s->busy = false;
    s->pending = true;
    s->remaining = 0;
    update_irq(s);
}

static int64_t completion_ns(const ArcsECC *s)
{
    /* Nominal LS2684 P-256 measurements at 300 MHz, rounded to a us. */
    if (s->field.words == 8) {
        switch (s->entry) {
        case RI:
            return 169000;
        case MP:
            return 93000;
        case R2:
            return 134000;
        case PVER:
            return 7000;
        }
    }
    return s->entry == PMULT ? 5161000 : 1000;
}

static bool start(ArcsECC *s, uint32_t value)
{
    unsigned radix = (value >> 8) & 7;
    unsigned words = value & 0xff;
    if (s->busy || s->pending || (value & ~(GO | RADIX_MASK)) ||
        (radix != 2 && radix != 3)) {
        return false;
    }
    switch (s->entry) {
    case MMULT: case MADD: case MSUB: case MDIV: case MINV: case MOD:
    case RI: case MP: case R2: case PMULT: case PDBL: case PADD: case PVER:
        break;
    default:
        return false;
    }
    unsigned full = radix == 2 ? 8 : 16;
    if (words > full || (radix == 3 && words && words <= 8)) {
        return false;
    }
    s->stride = full * 4;
    s->field.words = words ? words : full;
    s->field.modulus = operand(s, D, 0);
    if (!arcs_ecc_field_valid(&s->field)) {
        return false;
    }
    s->control = value & RADIX_MASK;
    s->zero = false;
    s->busy = true;
    /*
     * Functional virtual delay, independent of host runtime and guest polls.
     * These delays do not represent PKA cycle counts.
     */
    s->remaining = completion_ns(s);
    if (s->clock) {
        timer_mod(s->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->remaining);
    }
    return true;
}

static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    ArcsECC *s = opaque;
    if (size == 4 && !(off & 3)) {
        if (off >= A) {
            return s->ram[(off - A) / 4];
        }
        switch (off) {
        case CNTL:
            return s->control;
        case NEXT_FETCH:
            return s->entry;
        case RET_CODE:
            return (s->busy ? BUSY : 0) | (s->pending ? IRQ_PENDING : 0) |
                   (s->zero ? ZERO : 0);
        case STACK:
        case CONFIG:
            return 0;
        case FLAGS:
            return s->zero;
        case STAT:
            return s->pending ? IRQ_PENDING : 0;
        case IE:
            return s->ie;
        }
    }
    arcs_soc_fail(s->soc, BASE + off, size, false, 0);
    return 0;
}

static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    ArcsECC *s = opaque;
    if (size != 4 || (off & 3)) {
        goto unsupported;
    }
    if (off == STAT && !(value & ~IRQ_PENDING)) {
        if (value & IRQ_PENDING) {
            s->pending = false;
            update_irq(s);
        }
        return;
    }
    if (off == IE && !(value & ~IRQ_PENDING)) {
        s->ie = value;
        update_irq(s);
        return;
    }
    if (s->busy) {
        goto unsupported;
    }
    if (off >= A) {
        s->ram[(off - A) / 4] = value;
        return;
    }
    switch (off) {
    case CNTL:
        if (value & GO) {
            if (start(s, value)) {
                return;
            }
        } else if (!(value & ~RADIX_MASK)) {
            s->control = value;
            return;
        }
        break;
    case NEXT_FETCH:
        if (value <= UINT8_MAX) {
            s->entry = value;
            return;
        }
        break;
    case FLAGS:
        if (!value) {
            s->zero = false;
            return;
        }
        break;
    case CONFIG: case STACK:
        if (!value) {
            return;
        }
        break;
    }
unsupported:
    arcs_soc_fail(s->soc, BASE + off, size, true, value);
}

static const MemoryRegionOps ops = {
    .read = read_reg,
    .write = write_reg,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void arcs_ecc_clock(ArcsSoC *soc, bool enabled)
{
    ArcsECC *s = &soc->ecc;
    if (s->clock == enabled) {
        return;
    }
    if (s->busy) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (enabled) {
            timer_mod(s->timer, now + s->remaining);
        } else {
            s->remaining = MAX(1, timer_expire_time_ns(s->timer) - now);
            timer_del(s->timer);
        }
    }
    s->clock = enabled;
}

void arcs_ecc_reset(ArcsSoC *soc)
{
    ArcsECC *s = &soc->ecc;
    timer_del(s->timer);
    memset(s->ram, 0, sizeof(s->ram));
    s->control = s->entry = s->ie = 0;
    s->field = (ArcsECCField){ 0 };
    s->remaining = 0;
    s->busy = s->pending = s->zero = false;
    update_irq(s);
}

void arcs_ecc_init(ArcsSoC *soc)
{
    ArcsECC *s = &soc->ecc;
    s->soc = soc;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, complete, s);
    memory_region_init_io(&s->mmio, OBJECT(soc), &ops, s, "arcs-ecc",
                          A + sizeof(s->ram));
    memory_region_add_subregion(get_system_memory(), BASE, &s->mmio);
}

void arcs_ecc_finalize(ArcsSoC *soc)
{
    if (soc->ecc.timer) {
        timer_free(soc->ecc.timer);
    }
}
