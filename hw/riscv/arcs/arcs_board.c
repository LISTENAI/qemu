/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Checked board assembly. This layer never programs chip registers. */
#include "qemu/osdep.h"
#include "hw/riscv/arcs/arcs_board.h"
#include "hw/irq.h"
#include "hw/i2c/listenai_extio.h"
#include "hw/core/split-irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qbool.h"
#include "qobject/qlist.h"
#include "qemu/error-report.h"

static G_NORETURN void invalid(const char *field)
{
    error_report("Invalid board assembly field: %s", field);
    exit(1);
}

static unsigned i2c_route(unsigned scl, unsigned sda)
{
    if ((scl == 39 && sda == 38) || (scl == 23 && sda == 22)) { return 0; }
    if (scl == 33 && sda == 32) { return 1; }
    invalid("I2C physical pad route");
}

void arcs_board_load(ListenaiBoard *b, bool evb)
{
    *b = (ListenaiBoard) {
        .panel = true, .camera = true, .sd = true, .amplifier = true,
        .sd_detect = evb, .touch = evb, .touch_scl = 23, .touch_sda = 22, .touch_irq = 24,
        .psram_bytes = 16 * 1024 * 1024,
        .lcd_sck = evb ? 35 : 25, .lcd_sdi = evb ? 33 : 24,
        .lcd_cs = evb ? 37 : 22, .lcd_dc = evb ? 32 : 23,
        .lcd_reset = evb ? 1 : 41, .lcd_te = evb ? 40 : 27,
        .lcd_width = 240, .lcd_height = evb ? 320 : 240,
        .lcd_rotation = evb ? 270 : 90, .lcd_inverted = true,
        .backlight_pad = evb ? 0 : 21, .backlight_active = !evb,
        .amplifier_pad = evb ? 27 : 1, .amplifier_active = true,
        .camera_pads = {evb ? 23 : 39, evb ? 22 : 38, 26, 12, 10, 11,
                         13, 14, 15, 16, 17, 18, 19, 20, UINT_MAX, UINT_MAX},
    };
    const char *path = getenv("ARCS_QEMU_BOARD");
    if (!path) { return; }
    g_autofree char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) { invalid("file"); }
    QObject *root = qobject_from_json(text, &error_fatal);
    QDict *dict = qobject_to(QDict, root);
    const char *chip = dict ? qdict_get_try_str(dict, "chip") : NULL;
    if (!chip || (strcmp(chip, "ls2663") && strcmp(chip, "ls2684"))) { invalid("chip"); }
    b->psram_bytes = !strcmp(chip, "ls2663") ? 8 * 1024 * 1024 : 16 * 1024 * 1024;
    listenai_board_load(b, path, chip, 28, 10);
    qobject_unref(root);
    if (b->camera) {
        b->camera_i2c = i2c_route(b->camera_pads[LISTENAI_CAM_SCL], b->camera_pads[LISTENAI_CAM_SDA]);
        static const unsigned dvp[] = {26, 12, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20};
        for (unsigned i = 0; i < ARRAY_SIZE(dvp); i++) {
            if (b->camera_pads[i + 2] != dvp[i]) { invalid("camera DVP pad route"); }
        }
    }
    if (b->touch) { b->touch_i2c = i2c_route(b->touch_scl, b->touch_sda); }
    if (b->expander) { b->extio_i2c = i2c_route(b->extio_scl, b->extio_sda); }
    if (b->typec) { b->typec_i2c = i2c_route(b->typec_scl, b->typec_sda); }
    unsigned scl[] = {b->camera_pads[LISTENAI_CAM_SCL], b->touch_scl, b->extio_scl, b->typec_scl};
    unsigned sda[] = {b->camera_pads[LISTENAI_CAM_SDA], b->touch_sda, b->extio_sda, b->typec_sda};
    unsigned bus[] = {b->camera_i2c, b->touch_i2c, b->extio_i2c, b->typec_i2c};
    bool enabled[] = {b->camera, b->touch, b->expander, b->typec};
    for (unsigned i = 0; i < ARRAY_SIZE(enabled); i++) {
        for (unsigned j = i + 1; j < ARRAY_SIZE(enabled); j++) {
            if (enabled[i] && enabled[j] && bus[i] == bus[j] && (scl[i] != scl[j] || sda[i] != sda[j])) {
                invalid("shared I2C devices must share physical nets");
            }
        }
    }

}

static bool spi_selected(void *opaque, unsigned signal, unsigned pad)
{
    return arcs_spi_pad_selected(opaque, signal, pad);
}

void arcs_board_attach_spi(ListenaiBoard *b, ArcsSoC *soc, DeviceState *panel)
{
    ListenaiSPI *controllers[3];
    void *opaque[3];
    for (unsigned i = 0; i < 3; i++) {
        controllers[i] = &soc->spi[i].device;
        opaque[i] = &soc->spi[i];
    }
    listenai_board_attach_spi(b, DEVICE(soc), panel, controllers, opaque, 3, spi_selected);
}

void arcs_board_connect_pad(ArcsSoC *soc, unsigned pad, qemu_irq input)
{
    listenai_board_connect_pad(DEVICE(soc), pad, input);
}


bool arcs_board_camera_route(ListenaiBoard *b, ArcsSoC *soc)
{
    return arcs_pinmux_function(&soc->pinmux[0], b->camera_pads[LISTENAI_CAM_SCL], 8) &&
           arcs_pinmux_function(&soc->pinmux[0], b->camera_pads[LISTENAI_CAM_SDA], 8);
}

double arcs_board_backlight(ListenaiBoard *b, ArcsSoC *soc)
{
    unsigned pin = b->backlight_pad;
    if (pin >= 64) {
        double duty = listenai_extio_duty(b->extio, pin - 64);
        return b->backlight_active ? duty : 1 - duty;
    }
    uint32_t driven, levels;
    arcs_gpio_outputs_snapshot(&soc->gpio[pin / 32], &driven, &levels);
    bool pwm = arcs_pinmux_function(&soc->pinmux[0], pin, 12);
    double duty;
    if (pwm) {
        unsigned channel;
        if (pin == 21) { channel = 1; }
        else if (pin == 0) { channel = 0; }
        else {
            arcs_soc_fail(soc, 0x47500000 + 4 * pin, 4, false,
                          listenai_pinmux_config(&soc->pinmux[0], pin));
            return 0;
        }
        duty = arcs_gpt_duty(&soc->gpt, channel);
    } else {
        if (!(driven & (1u << (pin % 32)))) { return 0; }
        duty = !!(levels & (1u << (pin % 32)));
    }
    return b->backlight_active ? duty : 1.0 - duty;
}

