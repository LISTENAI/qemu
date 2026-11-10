/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CORE_LISTENAI_BOARD_H
#define HW_CORE_LISTENAI_BOARD_H
#include "hw/ssi/listenai_spi.h"
#include "hw/core/listenai-signal-trace.h"
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
    ListenaiSignalTrace signal_trace;
    bool configured, panel, epaper, camera, sd, amplifier, touch, keypad;
    unsigned touch_scl, touch_sda, touch_irq, touch_i2c;
    bool touch_sc919;
    unsigned touch_reset;
    unsigned keypad_channel, keypad_supply_uv, keypad_pull_up_ohms;
    bool sd_detect, sd_external_detect, sd_detect_active, expander;
    unsigned sd_detect_endpoint;
    DeviceState *extio;
    char *extio_id;
    unsigned extio_scl, extio_sda, extio_address, extio_i2c;
    bool typec, typec_host;
    unsigned typec_scl, typec_sda, typec_address, typec_i2c, typec_irq, typec_id_pin;
    DeviceState *typec_device;
    bool ls8500;
    char *ls8500_id;
    unsigned ls8500_scl, ls8500_sda, ls8500_irq, ls8500_i2c;
    DeviceState *ls8500_device;
    bool fs1815;
    char *fs1815_id;
    unsigned fs1815_scl, fs1815_sda, fs1815_i2c;
    DeviceState *fs1815_device;
    unsigned psram_bytes;
    uint32_t nor_bytes[2];
    bool nor_cs1;
    unsigned nor_cs1_pad;
    uint64_t pull_mask, pull_levels;
    uint32_t extio_pull_mask, extio_pull_levels;
    unsigned lcd_sck, lcd_sdi, lcd_cs, lcd_dc, lcd_reset, lcd_te;
    bool lcd_qspi, lcd_axs;
    unsigned lcd_data[4];
    void *lcd_route_opaque;
    unsigned lcd_width, lcd_height, lcd_x, lcd_y, lcd_rotation;
    bool lcd_inverted, backlight_active, amplifier_active;
    unsigned backlight_pad, amplifier_pad;
    unsigned camera_pads[16], camera_i2c;
    unsigned button_count;
    ListenaiBoardButton buttons[LISTENAI_BOARD_BUTTONS];
    unsigned pwm_led_count;
    struct { char *id; unsigned pad; bool active; } pwm_leds[8];
    double (*output_duty)(void *, unsigned);
    void *output_opaque;
    unsigned rotary_count;
    struct {
        char *id;
        unsigned a, b, edges_per_detent;
        DeviceState *device;
    } rotaries[4];
    SSIBus *panel_bus;
    DeviceState *panel_device;
    char *panel_id, *panel_controller_id, *backlight_id, *panel_otp_luts;
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
void listenai_board_panel_failure_callback(ListenaiBoard *b,
    void (*fn)(void *, const char *), void *opaque);
void listenai_board_panel_report(ListenaiBoard *b, FILE *stream);
void listenai_board_panel_save(ListenaiBoard *b, const char *path);
void listenai_board_sd_wire(ListenaiBoard *b, DeviceState *soc, DeviceState *sd_host);
void listenai_board_pulls_apply(ListenaiBoard *b, DeviceState *soc);
void listenai_board_attach_extio(ListenaiBoard *b, Object *owner, I2CBus *bus);
void listenai_board_attach_typec(ListenaiBoard *b, Object *owner, DeviceState *soc, I2CBus *bus);
void listenai_board_connect_gpio(ListenaiBoard *b, DeviceState *soc, unsigned endpoint, qemu_irq input);
void listenai_board_gpio_input(ListenaiBoard *b, DeviceState *soc, unsigned endpoint, bool level);
QDict *listenai_board_devices_snapshot(ListenaiBoard *b);
QDict *listenai_board_display_snapshot(ListenaiBoard *b);
void listenai_board_display_observer_init(ListenaiBoard *b, Object *owner);
void listenai_board_cleanup(ListenaiBoard *b);
void listenai_board_attach_ls8500(ListenaiBoard *b, Object *owner, DeviceState *soc, I2CBus *bus);
void listenai_board_attach_fs1815(ListenaiBoard *b, Object *owner, I2CBus *bus);
void listenai_board_rotaries_init(ListenaiBoard *b, Object *owner, DeviceState *soc);
void listenai_board_rotaries_drive(ListenaiBoard *b);
DeviceState *listenai_board_touch_new(ListenaiBoard *b, Object *owner,
                                     DeviceState *soc, I2CBus *bus);
void listenai_board_touch_input(ListenaiBoard *b, Object *owner, DeviceState *touch);
#endif
