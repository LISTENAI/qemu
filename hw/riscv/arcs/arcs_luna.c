/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"

static G_NORETURN void fatal(void *opaque, const char *message)
{
    ArcsSoC *soc = opaque;
    error_report("%s", message);
    soc->report(soc->report_opaque, "unsupported-luna");
    exit(1);
}
void arcs_luna_init(ArcsSoC *soc)
{
    bool safe = g_strcmp0(getenv("ARCS_QEMU_LUNA_SAFE_READS"), "1") == 0;
    listenai_luna_init(&soc->luna, OBJECT(soc), get_system_memory(),
                       0x49000000, UINT32_MAX, safe, soc, fatal);
}
void arcs_luna_reset(ArcsSoC *soc)
{
    listenai_luna_reset(&soc->luna);
}
