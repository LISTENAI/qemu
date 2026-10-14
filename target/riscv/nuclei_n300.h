/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_NUCLEI_N300_H
#define QEMU_NUCLEI_N300_H

/* Functional N300/ECLIC contract, shared by both ARCS harts. */
typedef struct NucleiN300State {
    uint32_t csrs[64];
    uint32_t mtvt;
    uint8_t threshold, level, config;
    int best, acknowledged;
    uint8_t irq[80][4];
    bool line[80];
    uint64_t exceptions, interrupts;
} NucleiN300State;

void nuclei_n300_init(RISCVCPU *cpu, bool dsp);
void nuclei_n300_reset(RISCVCPU *cpu);
bool nuclei_n300_interrupt(CPUState *cs);
void nuclei_n300_trap(CPURISCVState *env, bool interrupt);
void nuclei_n300_mret(CPURISCVState *env);
void nuclei_n300_irq(CPURISCVState *env, unsigned irq, bool value);
uint64_t nuclei_n300_eclic_read(CPURISCVState *env, hwaddr offset, unsigned size);
void nuclei_n300_eclic_write(CPURISCVState *env, hwaddr offset, uint64_t value,
                          unsigned size);
#endif
