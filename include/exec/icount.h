/*
 * icount - Instruction Counter API
 * CPU timers state API
 *
 * Copyright 2020 SUSE LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_ICOUNT_H
#define EXEC_ICOUNT_H

/**
 * ICountMode: icount enablement state:
 *
 * @ICOUNT_DISABLED: Disabled - Do not count executed instructions.
 * @ICOUNT_PRECISE: Enabled - Fixed conversion of insn to ns via "shift" option
 * @ICOUNT_ADAPTATIVE: Enabled - Runtime adaptive algorithm to compute shift
 */
typedef enum {
    ICOUNT_DISABLED = 0,
    ICOUNT_PRECISE,
    ICOUNT_ADAPTATIVE,
} ICountMode;

#ifdef CONFIG_TCG
extern ICountMode use_icount;
#define icount_enabled() (use_icount)
#else
#define icount_enabled() ICOUNT_DISABLED
#endif

/* Protect the CONFIG_USER_ONLY test vs poisoning. */
#if defined(COMPILING_PER_TARGET) || defined(COMPILING_SYSTEM_VS_USER)
# ifdef CONFIG_USER_ONLY
#  undef  icount_enabled
#  define icount_enabled() ICOUNT_DISABLED
# endif
#endif

/*
 * Update the icount with the executed instructions. Called by
 * cpus-tcg vCPU thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu);
void icount_clock_configure(CPUState *cpu, uint32_t hz, uint64_t quantum);
void icount_clock_set_hz(CPUState *cpu, uint32_t hz);
void icount_clock_enable_pacing(void);
void icount_clock_pace(void);
bool icount_clock_idle_advance(void);
int64_t icount_clock_audio_epoch(void);
void icount_clock_audio_cursor(const uint64_t *cursor);
int64_t icount_clock_budget(CPUState *cpu, int64_t deadline);
void icount_clock_finish(CPUState *cpu);
int64_t icount_get_cpu_raw(CPUState *cpu);
int64_t icount_get_cpu_cycles(CPUState *cpu);
void icount_clock_snapshot(CPUState *cpu, int64_t *frontier_ns,
                           uint64_t *phase);
void icount_clock_set_ratio(CPUState *cpu, uint64_t numerator,
                            uint32_t denominator);

/* get raw icount value */
int64_t icount_get_raw(void);

/* return the virtual CPU time in ns, based on the instruction counter. */
int64_t icount_get(void);
/*
 * convert an instruction counter value to ns, based on the icount shift.
 * This shift is set as a fixed value with the icount "shift" option
 * (precise mode), or it is constantly approximated and corrected at
 * runtime in adaptive mode.
 */
int64_t icount_to_ns(int64_t icount);

/**
 * icount_configure: configure the icount options, including "shift"
 * @opts: Options to parse
 * @errp: pointer to a NULL-initialized error object
 *
 * Return: true on success, else false setting @errp with error
 */
bool icount_configure(QemuOpts *opts, Error **errp);

/* used by tcg vcpu thread to calc icount budget */
int64_t icount_round(int64_t count);

/* if the CPUs are idle, start accounting real time to virtual clock. */
void icount_start_warp_timer(void);
void icount_account_warp_timer(void);
void icount_notify_exit(void);

#endif /* EXEC_ICOUNT_H */
