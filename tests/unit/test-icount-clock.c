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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/icount/large-rational-phase", phases);
    return g_test_run();
}
