/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CORE_LISTENAI_BOARD_INPUTS_H
#define HW_CORE_LISTENAI_BOARD_INPUTS_H
#include "hw/core/listenai-board.h"
typedef struct ListenaiBoardInputs {
    bool initialized;
    uint64_t pad_mask, pad_levels;
    uint32_t extio_mask, extio_levels;
    uint16_t adc_mask;
    uint32_t adc_uv[16];
    uint8_t extio_adc_mask;
    uint32_t extio_adc_uv[8];
} ListenaiBoardInputs;
bool listenai_board_inputs_parse(ListenaiBoard *b, ListenaiBoardInputs *state,
                                const char *value, unsigned pa, unsigned pb,
                                unsigned adc_first, unsigned adc_end, Error **errp);
void listenai_board_inputs_apply(ListenaiBoard *b, ListenaiBoardInputs *state, DeviceState *soc);
#endif
