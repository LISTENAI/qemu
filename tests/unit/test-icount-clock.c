/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/icount-clock.h"

static void check_scale(uint64_t hz, uint32_t den, __uint128_t factor)
{
    uint64_t small_scale = den;
    __uint128_t large_scale = small_scale * factor;
    __uint128_t small = 0, large = 0;
    /* The small representation permits a direct product oracle. The large
     * representation deliberately exceeds the range of that product. */
    for (unsigned i = 0; i < 500; i++) {
        uint64_t delta = i % 3 ? 1000000 : UINT64_C(1000000000000);
        __uint128_t unit = UINT64_C(1000000000) * small_scale;
        __uint128_t raw = (__uint128_t)delta * hz + small;
        uint64_t expected = raw / unit;
        uint64_t actual = icount_phase_accumulate(hz, den, large_scale,
                                                 delta, large, &large);
        small = raw % unit;
        g_assert_cmpuint(actual, ==, expected);
        g_assert_true(large == small * factor);
        uint64_t cycles = 999999 - i;
        uint64_t ns = icount_phase_ns(hz, den, large_scale, cycles, large);
        uint64_t expected_ns = (cycles * unit - small + hz - 1) / hz;
        g_assert_cmpuint(ns, ==, expected_ns);
        /* The completion boundary is minimal, and its remainder survives
         * subsequent elapsed-time accounting. */
        __uint128_t previous;
        actual = icount_phase_accumulate(hz, den, large_scale, ns - 1,
                                         large, &previous);
        g_assert_cmpuint(actual, ==, cycles - 1);
        actual = icount_phase_accumulate(hz, den, large_scale, ns,
                                         large, &large);
        g_assert_cmpuint(actual, ==, cycles);
        small = (small + (__uint128_t)ns * hz) % unit;
        g_assert_true(large == small * factor);
    }
}

static void phases(void)
{
    check_scale(37000000, 1, 1);
    check_scale(1000000000, 1, (__uint128_t)1 << 86);
    check_scale(24000000, 7, (__uint128_t)1 << 83);
    check_scale(UINT64_C(67108864015625), 1835008,
                (__uint128_t)1 << 65);
}

static void native_division(void)
{
    const uint64_t rates[] = { 1, 37000000, 300000000, 1000000000,
                              UINT64_C(67108864015625) };
    const uint32_t denominators[] = { 1, 7, 1835008 };
    const uint64_t deltas[] = { 0, 1, 999999999, UINT64_C(1000000000000),
                                UINT64_MAX };
    const uint64_t factors[] = { 1, 5, 1000, UINT64_C(10000000000) };
    for (size_t h = 0; h < G_N_ELEMENTS(rates); h++) {
        for (size_t d = 0; d < G_N_ELEMENTS(denominators); d++) {
            uint64_t hz = rates[h];
            uint32_t den = denominators[d];
            if (hz > UINT64_C(1000000000) * den) {
                continue;
            }
            for (size_t f = 0; f < G_N_ELEMENTS(factors); f++) {
                __uint128_t factor = factors[f];
                __uint128_t scale = den * factor;
                __uint128_t unit = UINT64_C(1000000000) * den;
                for (size_t i = 0; i < G_N_ELEMENTS(deltas); i++) {
                    for (unsigned edge = 0; edge < 3; edge++) {
                        __uint128_t carry = edge == 0 ? 0 :
                                            edge == 1 ? unit / 2 : unit - 1;
                        __uint128_t raw = (__uint128_t)deltas[i] * hz + carry;
                        __uint128_t phase;
                        uint64_t expected = raw / unit;
                        uint64_t actual = icount_phase_accumulate(hz, den,
                            scale, deltas[i], carry * factor, &phase);
                        g_assert_cmpuint(actual, ==, expected);
                        g_assert_true(phase == (raw % unit) * factor);
                        if (expected) {
                            __uint128_t needed = (__uint128_t)expected * unit -
                                                 carry;
                            uint64_t ns = icount_phase_ns(hz, den, scale,
                                                         expected, carry * factor);
                            g_assert_cmpuint(ns, ==, (needed + hz - 1) / hz);
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/icount/large-rational-phase", phases);
    g_test_add_func("/icount/native-division", native_division);
    return g_test_run();
}
