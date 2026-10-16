/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARCS PSRAM controller wiring and in-package Xccela geometry. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs.h"

void arcs_xccela_init(ListenAIXccela *chip, uint64_t size)
{
    static const uint8_t modes[10] = { 0x20, 0x0d, 0xdd };
    assert(size == 8 * 1024 * 1024 || size == 16 * 1024 * 1024);
    listenai_xccela_init(chip, NULL, "arcs-xccela", size, modes);
}

static void psram_fault(void *opaque, hwaddr address, unsigned size,
                        bool write, uint64_t value)
{
    arcs_soc_fail(opaque, address, size, write, value);
}

void arcs_psram_init(ArcsSoC *soc)
{
    listenai_psram_init(&soc->psram, OBJECT(soc), "arcs-psram-ctrl",
                        0x47b00000, soc->psram.chip, 10, NULL, psram_fault, soc);
}

void arcs_psram_reset(ArcsSoC *soc)
{
    listenai_psram_reset(&soc->psram);
}
