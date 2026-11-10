/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs_ecc_math.h"

static const ArcsECCInt one = { .w = { 1 } };

static int compare(ArcsECCInt a, ArcsECCInt b, unsigned n)
{
    for (unsigned i = n; i-- > 0;) {
        if (a.w[i] != b.w[i]) {
            return a.w[i] > b.w[i] ? 1 : -1;
        }
    }
    return 0;
}

static bool is_zero(ArcsECCInt a, unsigned n)
{
    return compare(a, (ArcsECCInt){ 0 }, n) == 0;
}

static bool bit(ArcsECCInt a, unsigned i)
{
    return (a.w[i / 32] >> (i % 32)) & 1;
}

static ArcsECCInt add(ArcsECCInt a, ArcsECCInt b, unsigned n, uint32_t *carry)
{
    ArcsECCInt sum = { 0 };
    uint64_t acc = 0;
    for (unsigned i = 0; i < n; i++) {
        acc = (uint64_t)a.w[i] + b.w[i] + (acc >> 32);
        sum.w[i] = acc;
    }
    *carry = acc >> 32;
    return sum;
}

static ArcsECCInt sub(ArcsECCInt a, ArcsECCInt b, unsigned n)
{
    ArcsECCInt diff = { 0 };
    uint64_t borrow = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t rhs = (uint64_t)b.w[i] + borrow;
        diff.w[i] = a.w[i] - rhs;
        borrow = a.w[i] < rhs;
    }
    return diff;
}

static ArcsECCInt half(ArcsECCInt a, unsigned n, uint32_t carry)
{
    for (unsigned i = n; i-- > 0;) {
        uint32_t low = a.w[i] & 1;
        a.w[i] = (a.w[i] >> 1) | (carry << 31);
        carry = low;
    }
    return a;
}

bool arcs_ecc_field_valid(const ArcsECCField *f)
{
    return f->words && f->words <= ARCS_ECC_WORDS &&
           (f->modulus.w[0] & 1) && compare(f->modulus, one, f->words) > 0;
}

/* Inputs are residues in [0, modulus). */
ArcsECCInt arcs_ecc_add(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b)
{
    uint32_t carry;
    ArcsECCInt sum = add(a, b, f->words, &carry);
    if (carry || compare(sum, f->modulus, f->words) >= 0) {
        sum = sub(sum, f->modulus, f->words);
    }
    return sum;
}

ArcsECCInt arcs_ecc_sub(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b)
{
    if (compare(a, b, f->words) < 0) {
        return sub(f->modulus, sub(b, a, f->words), f->words);
    }
    return sub(a, b, f->words);
}

ArcsECCInt arcs_ecc_reduce(const ArcsECCField *f, ArcsECCInt a)
{
    ArcsECCInt r = { 0 };
    if (compare(a, f->modulus, f->words) < 0) {
        return a;
    }
    for (unsigned i = f->words * 32; i-- > 0;) {
        r = arcs_ecc_add(f, r, r);
        if (bit(a, i)) {
            r = arcs_ecc_add(f, r, one);
        }
    }
    return r;
}

ArcsECCInt arcs_ecc_mul(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b)
{
    ArcsECCInt r = { 0 };
    for (unsigned i = 0; i < f->words * 32; i++) {
        if (bit(b, i)) {
            r = arcs_ecc_add(f, r, a);
        }
        a = arcs_ecc_add(f, a, a);
    }
    return r;
}

static ArcsECCInt half_residue(const ArcsECCField *f, ArcsECCInt a)
{
    uint32_t carry = 0;
    if (a.w[0] & 1) {
        a = add(a, f->modulus, f->words, &carry);
    }
    return half(a, f->words, carry);
}

/* Binary extended GCD: u = a*x and v = a*y (mod modulus). */
bool arcs_ecc_inv(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt *out)
{
    ArcsECCInt u = a, v = f->modulus, x = one, y = { 0 };
    unsigned n = f->words;

    for (unsigned i = 0; i < n * 64; i++) {
        if (compare(u, one, n) == 0) {
            *out = x;
            return true;
        }
        if (compare(v, one, n) == 0) {
            *out = y;
            return true;
        }
        if (is_zero(u, n) || is_zero(v, n)) {
            break;
        }
        if (!(u.w[0] & 1)) {
            u = half(u, n, 0);
            x = half_residue(f, x);
        } else if (!(v.w[0] & 1)) {
            v = half(v, n, 0);
            y = half_residue(f, y);
        } else if (compare(u, v, n) >= 0) {
            u = half(sub(u, v, n), n, 0);
            x = half_residue(f, arcs_ecc_sub(f, x, y));
        } else {
            v = half(sub(v, u, n), n, 0);
            y = half_residue(f, arcs_ecc_sub(f, y, x));
        }
    }
    return false;
}

ArcsECCInt arcs_ecc_power_of_two(const ArcsECCField *f, unsigned exponent)
{
    ArcsECCInt r = one;
    for (unsigned i = 0; i < exponent; i++) {
        r = arcs_ecc_add(f, r, r);
    }
    return r;
}

/*
 * D1 holds -modulus^-1 modulo the selected power-of-two radix. Each
 * additional bit is chosen to make the truncated product all ones.
 */
ArcsECCInt arcs_ecc_montgomery_parameter(const ArcsECCField *f)
{
    ArcsECCInt r = { 0 }, product = { 0 }, shifted = f->modulus;
    for (unsigned i = 0; i < f->words * 32; i++) {
        uint32_t discarded;
        if (!bit(product, i)) {
            r.w[i / 32] |= UINT32_C(1) << (i % 32);
            product = add(product, shifted, f->words, &discarded);
        }
        shifted = add(shifted, shifted, f->words, &discarded);
    }
    return r;
}

bool arcs_ecc_point_valid(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b,
                          ArcsECCPoint p)
{
    if (p.infinity || compare(p.x, f->modulus, f->words) >= 0 ||
        compare(p.y, f->modulus, f->words) >= 0) {
        return false;
    }
    ArcsECCInt left = arcs_ecc_mul(f, p.y, p.y);
    ArcsECCInt right = arcs_ecc_mul(f, p.x, p.x);
    right = arcs_ecc_mul(f, arcs_ecc_add(f, right, a), p.x);
    right = arcs_ecc_add(f, right, b);
    return compare(left, right, f->words) == 0;
}

bool arcs_ecc_point_add(const ArcsECCField *f, ArcsECCInt a, ArcsECCPoint p,
                        ArcsECCPoint q, ArcsECCPoint *out)
{
    ArcsECCInt dx, dy, inv, slope;
    if (p.infinity || q.infinity) {
        *out = p.infinity ? q : p;
        return true;
    }
    if (compare(p.x, q.x, f->words) == 0) {
        if (is_zero(arcs_ecc_add(f, p.y, q.y), f->words)) {
            *out = (ArcsECCPoint){ .infinity = true };
            return true;
        }
        if (compare(p.y, q.y, f->words) != 0) {
            return false;
        }
        ArcsECCInt xx = arcs_ecc_mul(f, p.x, p.x);
        dy = arcs_ecc_add(f, arcs_ecc_add(f, xx, xx), arcs_ecc_add(f, xx, a));
        dx = arcs_ecc_add(f, p.y, p.y);
    } else {
        dy = arcs_ecc_sub(f, q.y, p.y);
        dx = arcs_ecc_sub(f, q.x, p.x);
    }
    if (!arcs_ecc_inv(f, dx, &inv)) {
        return false;
    }
    slope = arcs_ecc_mul(f, dy, inv);
    out->x = arcs_ecc_mul(f, slope, slope);
    out->x = arcs_ecc_sub(f, arcs_ecc_sub(f, out->x, p.x), q.x);
    out->y = arcs_ecc_mul(f, slope, arcs_ecc_sub(f, p.x, out->x));
    out->y = arcs_ecc_sub(f, out->y, p.y);
    out->infinity = false;
    return true;
}

bool arcs_ecc_point_mul(const ArcsECCField *f, ArcsECCInt a, ArcsECCPoint p,
                        ArcsECCInt scalar, ArcsECCPoint *out)
{
    ArcsECCPoint result = { .infinity = true };
    for (unsigned i = f->words * 32; i-- > 0;) {
        if (!arcs_ecc_point_add(f, a, result, result, &result) ||
            (bit(scalar, i) && !arcs_ecc_point_add(f, a, result, p, &result))) {
            return false;
        }
    }
    *out = result;
    return true;
}
