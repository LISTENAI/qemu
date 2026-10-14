/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_BLOCK_LISTENAI_NOR_H
#define HW_BLOCK_LISTENAI_NOR_H

#include "system/memory.h"

#define LISTENAI_NOR_SIZE 0x1000000

/* External flash state belongs to the board and survives controller reset. */
typedef struct ListenAINOR {
    MemoryRegion rom;
    hwaddr address;
    FILE *backing;
    uint8_t status[3];
    bool wel, asleep, reset_enabled, four_byte;
} ListenAINOR;

void listenai_nor_init(ListenAINOR *chip, Object *owner, const char *name,
                       hwaddr address, const char *image, bool persist);
/* A NULL chip models an unselected bus, returning erased bytes on MISO.
 * False means an unsupported command or protected programming operation;
 * the controller must report it rather than fabricate completion. */
bool listenai_nor_command(ListenAINOR *chip, uint8_t op, uint32_t address,
                          const uint8_t *tx, unsigned count, uint8_t *rx,
                          unsigned length, unsigned *received);
#endif
