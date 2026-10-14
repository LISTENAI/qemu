/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS PSRAM controller wiring and its external 128-Mbit Xccela chip. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"

void arcs_xccela_init(ArcsXccela128 *chip)
{
    static const uint8_t modes[10] = { 0x20, 0x0d, 0xdd };
    listenai_xccela_init(chip, NULL, "xccela-128m", 16 * 1024 * 1024, modes);
}

static void psram_fault(void *opaque, hwaddr address, unsigned size,
                        bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}

void arcs_psram_init(ArcsSoC *soc)
{
    listenai_psram_init(&soc->psram, OBJECT(soc), "arcs-psram-ctrl",
                        0x47b00000, soc->psram.chip, NULL, psram_fault, soc);
}

void arcs_psram_reset(ArcsSoC *soc)
{
    listenai_psram_reset(&soc->psram);
}
