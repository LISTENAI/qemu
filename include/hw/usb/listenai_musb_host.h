/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_USB_LISTENAI_MUSB_HOST_H
#define HW_USB_LISTENAI_MUSB_HOST_H
#include "hw/usb/listenai_musb.h"
#include "chardev/char-fe.h"
#define TRACE_CAPACITY 128
#define DESCRIPTOR_CAPACITY 4096
typedef enum HostPhase { HOST_IDLE, HOST_RESET, HOST_REQUEST, HOST_TRANSFER,
                         HOST_NEXT } HostPhase;
typedef struct USBTrace {
    uint64_t sequence;
    int64_t virtual_ns;
    const char *kind;
    uint8_t data[64];
    unsigned length;
} USBTrace;
typedef struct USBCDC {
    CharBackend chr;
    QEMUTimer *timer;
    char *listen;
    const char *error;
    bool connected, ready, opening;
    unsigned control, in, out, notification, in_max, out_max, notification_max;
    uint8_t incoming[65536], outgoing[65536];
    unsigned incoming_count, outgoing_count;
    uint64_t bytes_in, bytes_out, notifications;
    int64_t deadline, control_at;
} USBCDC;
struct ListenAIMUSBHost {
    ListenAIMUSB *controller;
    QEMUTimer *timer, *sof;
    uint16_t frame;
    bool connected, enumerating, high_speed;
    HostPhase phase;
    const char *status, *error;
    int64_t deadline;
    unsigned step, expected, received, packet_size, configuration;
    uint8_t setup[8], response[DESCRIPTOR_CAPACITY];
    uint8_t device[18], config[DESCRIPTOR_CAPACITY], strings[3][255];
    unsigned device_length, config_length, string_length[3], language;
    USBTrace trace[TRACE_CAPACITY];
    uint64_t sequence, epoch;
    unsigned trace_count;
    CharBackend adb;
    QEMUTimer *bulk;
    bool adb_connected, adb_header;
    char *listen;
    const char *adb_error;
    unsigned adb_in, adb_out, adb_in_max, adb_out_max;
    unsigned adb_remaining, adb_payload;
    uint8_t incoming[65536], outgoing[65536];
    unsigned incoming_count, outgoing_count;
    uint64_t bytes_in, bytes_out;
    USBCDC cdc;
    void (*control_done)(ListenAIMUSBHost *h);
};

void listenai_musb_irq(ListenAIMUSB *s);
void listenai_musb_downstream_kick(ListenAIMUSB *s);
void listenai_musb_downstream_reset(ListenAIMUSB *s);
void listenai_musb_downstream_finalize(ListenAIMUSB *s);
uint8_t listenai_musb_downstream_devctl(ListenAIMUSB *s);
uint8_t listenai_musb_downstream_target(ListenAIMUSB *s, unsigned off);
bool listenai_musb_downstream_write(ListenAIMUSB *s, unsigned ep, unsigned off, uint8_t value);
void listenai_musb_fifo_clear(ListenAIMUSB *s);
void listenai_musb_cancel_dma(ListenAIMUSB *s);
void listenai_musb_descriptors_clear(ListenAIMUSBHost *h);
void listenai_musb_sof_update(ListenAIMUSBHost *h);
void listenai_musb_bus_reset(ListenAIMUSBHost *h, bool enumerate);
void listenai_musb_dma_service(ListenAIMUSB *s);
void listenai_musb_adb_close(ListenAIMUSBHost *h, const char *reason);
void listenai_musb_adb_init(ListenAIMUSB *s);
void listenai_musb_adb_finalize(ListenAIMUSBHost *h);
void listenai_musb_cdc_init(ListenAIMUSB *s);
void listenai_musb_cdc_finalize(ListenAIMUSBHost *h);
void listenai_musb_cdc_close(ListenAIMUSBHost *h, const char *reason);
void listenai_musb_control(ListenAIMUSBHost *h, uint8_t type, uint8_t request,
                           uint16_t value, uint16_t index,
                           void (*done)(ListenAIMUSBHost *h));
#endif
