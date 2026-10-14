/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded host control and observations, independent of board peripherals. */
#include "qemu/osdep.h"
#include "hw/riscv/venusa/venusa.h"
#include "exec/icount.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "system/system.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

char *venusa_snapshot(VenusASoC *s, const char *status)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"status\":\"%s\",\"virtual_ns\":%" PRId64
        ",\"seconds\":%.9f,\"aggregate_instructions\":%" PRId64 ",\"cores\":[",
        status, ns, ns / 1e9, icount_enabled() ? icount_get_raw() : 0);
    for (unsigned i = 0; i < 2; i++) {
        CPURISCVState *env = &s->cpu[i].env;
        NucleiN300State *n = env->nuclei_state;
        g_string_append_printf(out, "%s{\"hart\":%u,\"pc\":%u,\"mcause\":%u,"
            "\"mepc\":%u,\"mtval\":%u,\"instructions\":%" PRId64
            ",\"exceptions\":%" PRIu64 ",\"interrupts\":%" PRIu64
            ",\"clock_hz_numerator\":%" PRIu64 ",\"clock_hz_denominator\":%u}",
            i ? "," : "", i, (uint32_t)env->pc, (uint32_t)env->mcause,
            (uint32_t)env->mepc, (uint32_t)env->mtval,
            qatomic_read_i64(&CPU(&s->cpu[i])->icount_executed), n->exceptions, n->interrupts,
            CPU(&s->cpu[i])->icount_hz, CPU(&s->cpu[i])->icount_hz_den);
    }
    g_string_append_printf(out, "],\"uart_tx_bytes\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]}",
        s->uart[0].device.tx_bytes, s->uart[1].device.tx_bytes, s->uart[2].device.tx_bytes);
    return g_string_free(out, false);
}

void venusa_report(VenusASoC *s, const char *status)
{
    const char *path = getenv("LISEM_QEMU_REPORT");
    if (!path || s->reported) { return; }
    FILE *f = !strcmp(path, "-") ? stdout : fopen(path, "w");
    if (!f) { error_report("Could not open VenusA report"); exit(1); }
    s->reported = true;
    g_autofree char *json = venusa_snapshot(s, status);
    fprintf(f, "%s\n", json);
    if (f == stdout) { fflush(f); } else { fclose(f); }
}

static void finish(void *opaque)
{
    VenusASoC *s = opaque;
    if (!qtest_enabled()) {
        for (unsigned i = 0; i < 2; i++) { cpu_pause(CPU(&s->cpu[i])); }
        vm_stop(RUN_STATE_PAUSED);
    }
    venusa_report(s, "budget-complete");
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
}

void venusa_runtime_init(VenusASoC *s)
{
    const char *clock = getenv("LISEM_QEMU_SOC_CLOCK");
    if (clock && !qtest_enabled()) {
        char *end;
        uint64_t quantum = g_ascii_strtoull(clock, &end, 10);
        if (!*clock || *end || !quantum || quantum > 1000000) {
            error_report("Invalid LISEM_QEMU_SOC_CLOCK quantum"); exit(1);
        }
        s->follow_hclk = true;
        for (unsigned i = 0; i < 2; i++) { icount_clock_configure(CPU(&s->cpu[i]), 24000000, quantum); }
    }
    if (getenv("LISEM_QEMU_PACE") && !qtest_enabled()) { icount_clock_enable_pacing(); }
    const char *budget = getenv("LISEM_QEMU_BUDGET_NS");
    if (budget) {
        char *end;
        uint64_t ns = g_ascii_strtoull(budget, &end, 10);
        if (!*budget || *end || !ns || ns > INT64_MAX) {
            error_report("Invalid LISEM_QEMU_BUDGET_NS"); exit(1);
        }
        s->deadline = timer_new_ns(QEMU_CLOCK_VIRTUAL, finish, s);
        timer_mod(s->deadline, ns);
    }
}
