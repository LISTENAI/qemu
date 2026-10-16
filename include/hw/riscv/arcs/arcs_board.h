/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_ARCS_BOARD_H
#define HW_RISCV_ARCS_BOARD_H
#include "hw/riscv/arcs/arcs.h"
#include "qobject/qdict.h"

#include "hw/core/listenai-board.h"
void arcs_board_load(ListenaiBoard *b, bool evb);
void arcs_board_attach_spi(ListenaiBoard *b, ArcsSoC *soc, DeviceState *panel);
bool arcs_board_camera_route(ListenaiBoard *b, ArcsSoC *soc);
double arcs_board_backlight(ListenaiBoard *b, ArcsSoC *soc);
void arcs_board_connect_pad(ArcsSoC *soc, unsigned pad, qemu_irq input);
#endif
