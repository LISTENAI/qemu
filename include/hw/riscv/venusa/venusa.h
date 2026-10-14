/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RISCV_VENUSA_H
#define HW_RISCV_VENUSA_H
#include "hw/sysbus.h"
#include "hw/misc/listenai_luna.h"
#include "qemu/units.h"
#include "hw/riscv/venusa/venusa_audio.h"
#include "target/riscv/cpu.h"
#include "target/riscv/nuclei_n300.h"
#include "hw/char/listenai_uart.h"
#include "hw/ssi/listenai_spimem.h"
#include "hw/sd/listenai_sd.h"
#include "hw/mem/listenai_psram.h"
#include "hw/timer/nuclei_systimer.h"
#include "hw/gpio/listenai_gpio.h"
#include "hw/dma/listenai_dma.h"
#include "hw/timer/listenai_dual_timer.h"
#include "hw/i2c/listenai_i2c.h"
#include "hw/rtc/listenai_calendar.h"
#include "hw/riscv/venusa/venusa_gpt.h"
#include "hw/timer/listenai_aon.h"
#include "hw/watchdog/listenai_wdt.h"
#include "hw/adc/venusa_gpadc.h"
#include "hw/intc/nuclei_cidu.h"
#include "hw/riscv/venusa/venusa_spi.h"
#include "hw/riscv/venusa/venusa_dvp.h"

#define TYPE_VENUSA_SOC "venusa-soc"
OBJECT_DECLARE_SIMPLE_TYPE(VenusASoC, VENUSA_SOC)

typedef struct VenusAUART {
    VenusASoC *soc;
    ListenAIUART device;
    unsigned index;
} VenusAUART;

typedef struct VenusAPeripheral {
    VenusASoC *soc;
    unsigned index;
} VenusAPeripheral;

struct VenusASoC {
    SysBusDevice parent_obj;
    VenusAAudio audio;
    ListenAILUNA luna;
    uint32_t luna_mtq_control, luna_mtq_mask, luna_busrd;
    VenusGPADC gpadc;
    NucleiCIDU cidu;
    VenusASPI spi[2];
    VenusADVP dvp;
    bool core1_held;
    RISCVCPU cpu[2];
    MemoryRegion io, ram[6], rom, psram_alias;
    ListenAIXccela psram_chip;
    ListenAIPSRAM psram_ctrl;
    ListenAISD sd;
    ListenAINOR nor;
    ListenAISpiMem flash_ctrl;
    MemoryRegion flash_delay_io;
    uint32_t flash_delay_regs[2];
    NucleiSysTimer timer[2];
    VenusAUART uart[3];
    uint32_t syscfg[0xf0 / 4], buscfg[0x30 / 4];
    uint32_t aon[0x100 / 4], iomux[0x100 / 4], aon_iomux[0x100 / 4];
    ListenaiPinmux pinmux;
    ListenaiGPIO gpio[2];
    qemu_irq pad_output[64];
    ListenAIDMA dma;
    ListenAIDualTimer dual_timer[2];
    ListenaiI2C i2c[2];
    ListenAICalendar calendar;
    VenusAGPT gpt;
    ListenaiAONTimer aon_timer;
    ListenaiAONWDT aon_wdt;
    ListenaiWDT wdt[2];
    VenusAPeripheral wdt_context[2];
    VenusAPeripheral dual_context[2], i2c_context[2];
    uint32_t board_inputs[2];
    uint8_t otp[64];
    BlockBackend *flash;
    QEMUTimer *deadline;
    bool reported, follow_hclk;
    bool reset_preserve_aon;
    uint32_t reset_cause;
    uint64_t irq_levels[2];
    uint32_t hclk_n, hclk_m, pclk_n, pclk_m, pll_core_div, gpt_t0_div;
};

G_NORETURN void venusa_fail(VenusASoC *s, hwaddr address, unsigned size,
                           bool write, uint64_t value);
void venusa_irq(VenusASoC *s, unsigned irq, bool value);
bool venusa_sysctl_read(VenusASoC *s, hwaddr address, uint32_t *value);
bool venusa_sysctl_write(VenusASoC *s, hwaddr address, uint32_t value);
void venusa_sysctl_reset(VenusASoC *s);
void venusa_psram_init(VenusASoC *s);
void venusa_flashdl_init(VenusASoC *s);
void venusa_flashdl_reset(VenusASoC *s);
void venusa_storage_init(VenusASoC *s);
void venusa_storage_reset(VenusASoC *s);
void venusa_uart_init(VenusASoC *s);
void venusa_uart_update_dma(VenusASoC *s);
char *venusa_snapshot(VenusASoC *s, const char *status);
void venusa_runtime_init(VenusASoC *s);
void venusa_report(VenusASoC *s, const char *status);
void venusa_clock_update(VenusASoC *s);
void venusa_hclk_ratio(VenusASoC *s, uint64_t *n, uint64_t *d);
void venusa_peripherals_init(VenusASoC *s);
void venusa_peripherals_reset(VenusASoC *s);
void venusa_aon_init(VenusASoC *s);
void venusa_aon_reset(VenusASoC *s);
void venusa_reset_cpu(VenusASoC *s, unsigned index);
void venusa_gpio_init(VenusASoC *s);
void venusa_gpt_output(VenusASoC *s, unsigned output, bool level);
void venusa_dma_init(VenusASoC *s);
void venusa_uart_reset(VenusASoC *s, unsigned index);
uint32_t venusa_uart_read(VenusASoC *s, unsigned index, unsigned offset);
void venusa_uart_write(VenusASoC *s, unsigned index, unsigned offset, uint32_t value);
void venusa_luna_init(VenusASoC *s);
void venusa_luna_reset(VenusASoC *s);
#endif
