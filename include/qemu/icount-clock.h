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
    if (scale <= UINT64_MAX / UINT64_C(2000000000)) {
        uint64_t whole = 0;
        /* Bound the product without losing elapsed whole cycles. Long-lived
         * counters need this split even at an integer oscillator rate. */
        if (raw >> 64) {
            whole = (delta / divisor) * hz;
            raw = (__uint128_t)(delta % divisor) * hz;
        }
        if (!(raw >> 64)) {
            uint64_t part = raw;
            uint64_t unit = UINT64_C(1000000000) * (uint64_t)scale;
            uint64_t tail = (part % divisor) * ((uint64_t)scale / hz_den) +
                            (uint64_t)carry;
            /* Both addends are below unit, so one subtraction suffices.
             * The scale bound ensures their sum cannot overflow. */
            bool overflow = tail >= unit;
            *phase = overflow ? tail - unit : tail;
            return whole + part / divisor + overflow;
        }
        /* The wide fallback consumes the original elapsed-time product. */
        raw = (__uint128_t)delta * hz;
    }
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
    if (scale <= UINT64_MAX / UINT64_C(1000000000)) {
        uint64_t fraction = (uint64_t)phase / ((uint64_t)scale / hz_den);
        __uint128_t needed = (__uint128_t)instructions * divisor - fraction;
        if (needed <= UINT64_MAX - (hz - 1)) {
            return ((uint64_t)needed + hz - 1) / hz;
        }
    }
    __uint128_t needed = (__uint128_t)instructions * divisor -
                         phase / (scale / hz_den);
    /* Discarding the fractional part of phase/(scale/hz_den) cannot change
     * this ceiling. Rate <= 1 cycle/ns guarantees exactly the requested
     * number of cycles at this first integer nanosecond. */
    return (needed + hz - 1) / hz;
}
#endif
