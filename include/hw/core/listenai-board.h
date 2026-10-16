/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CORE_LISTENAI_BOARD_H
#define HW_CORE_LISTENAI_BOARD_H
#include "hw/ssi/listenai_spi.h"
#include "hw/i2c/i2c.h"
#include "qobject/qdict.h"

#define LISTENAI_BOARD_BUTTONS 16
typedef struct ListenaiBoardButton {
    char *id;
    unsigned pad;
    bool analog;
    unsigned resistance_ohms;
    bool pressed_level, released_level, pressed;
    void *owner;
} ListenaiBoardButton;

typedef struct ListenaiBoard {
    bool configured, panel, camera, sd, amplifier, touch, keypad;
    unsigned touch_scl, touch_sda, touch_irq, touch_i2c;
    unsigned keypad_channel, keypad_supply_uv, keypad_pull_up_ohms;
    bool sd_detect, sd_external_detect, sd_detect_active, expander;
    unsigned sd_detect_endpoint;
    DeviceState *extio;
    char *extio_id;
    unsigned extio_scl, extio_sda, extio_address, extio_i2c;
    bool typec, typec_host;
    unsigned typec_scl, typec_sda, typec_address, typec_i2c, typec_irq, typec_id_pin;
    DeviceState *typec_device;
    unsigned psram_bytes;
    uint64_t pull_mask, pull_levels;
    uint32_t extio_pull_mask, extio_pull_levels;
    unsigned lcd_sck, lcd_sdi, lcd_cs, lcd_dc, lcd_reset, lcd_te;
    unsigned lcd_width, lcd_height, lcd_x, lcd_y, lcd_rotation;
    bool lcd_inverted, backlight_active, amplifier_active;
    unsigned backlight_pad, amplifier_pad;
    unsigned camera_pads[16], camera_i2c;
    unsigned button_count;
    ListenaiBoardButton buttons[LISTENAI_BOARD_BUTTONS];
    SSIBus *panel_bus;
    DeviceState *panel_device;
    struct {
        struct ListenaiBoard *board;
        ListenaiSPI *spi;
        bool (*selected)(void *, unsigned, unsigned);
        void *opaque;
    } spi_routes[3];
} ListenaiBoard;

enum {
    LISTENAI_CAM_SCL, LISTENAI_CAM_SDA, LISTENAI_CAM_MCLK, LISTENAI_CAM_PCLK,
    LISTENAI_CAM_HSYNC, LISTENAI_CAM_VSYNC, LISTENAI_CAM_D0,
    LISTENAI_CAM_RESET = 14, LISTENAI_CAM_PWDN = 15,
};

void listenai_board_load(ListenaiBoard *b, const char *path, const char *chip,
                         unsigned pa_pads, unsigned pb_pads);
void listenai_board_connect_pad(DeviceState *soc, unsigned pad, qemu_irq input);
void listenai_board_attach_spi(ListenaiBoard *b, DeviceState *soc,
                              DeviceState *panel, ListenaiSPI **controllers,
                              void **opaque, unsigned count,
                              bool (*selected)(void *, unsigned, unsigned));
void listenai_board_camera_wire(ListenaiBoard *b, DeviceState *soc, DeviceState *sensor);
DeviceState *listenai_board_panel_new(ListenaiBoard *b, Object *owner);
void listenai_board_panel_wire(ListenaiBoard *b, DeviceState *soc, DeviceState *panel);
void listenai_board_sd_wire(ListenaiBoard *b, DeviceState *soc, DeviceState *sd_host);
void listenai_board_pulls_apply(ListenaiBoard *b, DeviceState *soc);
void listenai_board_attach_extio(ListenaiBoard *b, Object *owner, I2CBus *bus);
void listenai_board_attach_typec(ListenaiBoard *b, Object *owner, DeviceState *soc, I2CBus *bus);
void listenai_board_connect_gpio(ListenaiBoard *b, DeviceState *soc, unsigned endpoint, qemu_irq input);
void listenai_board_gpio_input(ListenaiBoard *b, DeviceState *soc, unsigned endpoint, bool level);
QDict *listenai_board_devices_snapshot(ListenaiBoard *b);
void listenai_board_cleanup(ListenaiBoard *b);
#endif
