/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_ICOUNT_CLOCK_H
#define QEMU_ICOUNT_CLOCK_H

/* A phase is a fraction of one CPU cycle, with denominator 1e9 * scale.
 * scale is a multiple of hz_den and must fit two full phases in 128 bits.
 * Frequencies are positive and at most one cycle per nanosecond. */
static inline uint64_t icount_phase_accumulate(uint64_t hz, uint32_t hz_den,
                                             __uint128_t scale, uint64_t delta,
                                             __uint128_t carry,
                                             __uint128_t *phase)
{
    __uint128_t raw = (__uint128_t)delta * hz;
    uint64_t divisor = UINT64_C(1000000000) * hz_den;
    __uint128_t tail = (raw % divisor) * (scale / hz_den) + carry;
    __uint128_t unit = UINT64_C(1000000000) * scale;
    *phase = tail % unit;
    return raw / divisor + tail / unit;
}

static inline uint64_t icount_phase_ns(uint64_t hz, uint32_t hz_den,
                                     __uint128_t scale, uint64_t instructions,
                                     __uint128_t phase)
{
    uint64_t divisor = UINT64_C(1000000000) * hz_den;
    __uint128_t needed = (__uint128_t)instructions * divisor -
                         phase / (scale / hz_den);
    /* Discarding the fractional part of phase/(scale/hz_den) cannot change
     * this ceiling. Rate <= 1 cycle/ns guarantees exactly the requested
     * number of cycles at this first integer nanosecond. */
    return (needed + hz - 1) / hz;
}
#endif
