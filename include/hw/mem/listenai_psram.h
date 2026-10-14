/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MEM_LISTENAI_PSRAM_H
#define HW_MEM_LISTENAI_PSRAM_H
#include "system/memory.h"

/* Volatile array data survives controller reset; identity is board-selected. */
typedef struct ListenAIXccela {
    MemoryRegion ram;
    uint8_t modes[10], reset_modes[10];
    bool mode_read_pairs;
} ListenAIXccela;

typedef bool (*ListenAIPSRAMExtraValid)(void *opaque, hwaddr offset);
typedef void (*ListenAIPSRAMFault)(void *opaque, hwaddr address, unsigned size,
                                  bool write, uint64_t value);
typedef struct ListenAIPSRAM {
    MemoryRegion io;
    ListenAIXccela *chip;
    uint32_t regs[0xc04 / 4];
    hwaddr address;
    unsigned sequence_count;
    ListenAIPSRAMExtraValid extra_valid;
    ListenAIPSRAMFault fault;
    void *opaque;
} ListenAIPSRAM;

void listenai_xccela_init(ListenAIXccela *chip, Object *owner,
                          const char *name, uint64_t size,
                          const uint8_t reset_modes[10]);
void listenai_psram_init(ListenAIPSRAM *s, Object *owner, const char *name,
                         hwaddr address, ListenAIXccela *chip, unsigned sequence_count,
                         ListenAIPSRAMExtraValid extra_valid,
                         ListenAIPSRAMFault fault, void *opaque);
void listenai_psram_reset(ListenAIPSRAM *s);
#endif
