/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_CORE_LISTENAI_CAMERA_SCENE_H
#define HW_CORE_LISTENAI_CAMERA_SCENE_H
#include "hw/qdev-core.h"
#include "qemu/lisa-camera-input.h"
#include "qemu/timer.h"

typedef struct ListenaiCameraScene {
    DeviceState *camera;
    void *opaque;
    void (*changed)(void *opaque);
    bool (*enabled)(void *opaque);
    LisaMapping camera_mapping;
    QEMUTimer *camera_poll;
    uint64_t camera_generation, camera_sequence, camera_repeated;
    uint64_t camera_frame_index, camera_latched_frame;
    uint64_t camera_host_ns, camera_received_ns, camera_skipped;
    unsigned camera_source_state;
} ListenaiCameraScene;

void listenai_camera_scene_init(ListenaiCameraScene *s, Object *owner,
                               DeviceState *sensor, const char *shared,
                               void (*changed)(void *), bool (*enabled)(void *), void *opaque);
int listenai_camera_scene_receive(ListenaiCameraScene *s, Error **errp);
void listenai_camera_scene_latch(ListenaiCameraScene *s);
char *listenai_camera_scene_status(ListenaiCameraScene *s);
void listenai_camera_scene_destroy(ListenaiCameraScene *s);
#endif
