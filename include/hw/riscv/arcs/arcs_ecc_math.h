/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_ECC_MATH_H
#define HW_RISCV_ARCS_ECC_MATH_H

#define ARCS_ECC_WORDS 16

/* Guest operand RAM uses least-significant words first. */
typedef struct ArcsECCInt {
    uint32_t w[ARCS_ECC_WORDS];
} ArcsECCInt;

typedef struct ArcsECCPoint {
    ArcsECCInt x, y;
    bool infinity;
} ArcsECCPoint;

typedef struct ArcsECCField {
    ArcsECCInt modulus;
    unsigned words;
} ArcsECCField;

bool arcs_ecc_field_valid(const ArcsECCField *f);
ArcsECCInt arcs_ecc_reduce(const ArcsECCField *f, ArcsECCInt a);
ArcsECCInt arcs_ecc_add(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b);
ArcsECCInt arcs_ecc_sub(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b);
ArcsECCInt arcs_ecc_mul(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b);
bool arcs_ecc_inv(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt *out);
ArcsECCInt arcs_ecc_power_of_two(const ArcsECCField *f, unsigned exponent);
ArcsECCInt arcs_ecc_montgomery_parameter(const ArcsECCField *f);
bool arcs_ecc_point_valid(const ArcsECCField *f, ArcsECCInt a, ArcsECCInt b,
                          ArcsECCPoint p);
bool arcs_ecc_point_add(const ArcsECCField *f, ArcsECCInt a, ArcsECCPoint p,
                        ArcsECCPoint q, ArcsECCPoint *out);
bool arcs_ecc_point_mul(const ArcsECCField *f, ArcsECCInt a, ArcsECCPoint p,
                        ArcsECCInt scalar, ArcsECCPoint *out);
#endif
