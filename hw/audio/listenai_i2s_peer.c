/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit external Philips-I2S clock/data peer with bounded PCM fixtures. */
#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "hw/audio/listenai_i2s_shift.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/system.h"
#include "qom/object.h"

#define TYPE_LISTENAI_I2S_PEER "listenai-i2s-peer"
OBJECT_DECLARE_SIMPLE_TYPE(ListenaiI2SPeer, LISTENAI_I2S_PEER)
struct ListenaiI2SPeer {
    DeviceState parent_obj;
    DeviceState *gpio;
    bool master, clock, ws, data, finished, capture_left_valid;
    bool armed, edge_started;
    uint32_t bclk_pin, ws_pin, input_pin, output_pin;
    uint32_t rate, width, frames;
    uint64_t start_ns, edges, transmitted, captured;
    char *input_path, *output_path;
    uint8_t *input;
    size_t input_length, position;
    int32_t frame[2], capture_frame[2];
    GByteArray *capture;
    qemu_irq pins[3];
    QEMUTimer *timer;
    ListenaiI2SShift tx, rx;
    Notifier exit;
};

static uint32_t source(void *opaque, unsigned channel)
{
    ListenaiI2SPeer *s = opaque;
    if (!channel) {
        if (s->input) {
            if (s->position + 8 > s->input_length) {
                error_report(
                    "I2S peer input exhausted before requested frames");
                exit(1);
            }
            s->frame[0] = ldl_le_p(s->input + s->position);
            s->frame[1] = ldl_le_p(s->input + s->position + 4);
            s->position += 8;
        }
    }
    return s->frame[channel];
}
static void source_done(void *opaque, unsigned channel, uint32_t word)
{
    ListenaiI2SPeer *s = opaque;
    if (channel) {
        s->transmitted++;
    }
}
static void sink(void *opaque, unsigned channel, uint32_t word)
{
    ListenaiI2SPeer *s = opaque;
    s->capture_frame[channel] =
        (int32_t)(word << (32 - s->width)) >> (32 - s->width);
    if (!channel) {
        s->capture_left_valid = true;
    } else if (s->capture_left_valid) {
        s->capture_left_valid = false;
        s->captured++;
        if (s->capture) {
            if (s->capture->len >= 8 * s->frames) {
                return;
            }
            uint8_t bytes[8];
            stl_le_p(bytes, s->capture_frame[0]);
            stl_le_p(bytes + 4, s->capture_frame[1]);
            g_byte_array_append(s->capture, bytes, sizeof(bytes));
        }
    }
}
static void sample_clock(ListenaiI2SPeer *s, bool level)
{
    if (!s->armed) {
        s->clock = level;
        return;
    }
    if (level == s->clock || s->finished) {
        return;
    }
    s->clock = level;
    if (!level) {
        s->edge_started = true;
        bool bit = listenai_i2s_shift(&s->tx, s->ws, s->width, 1, false, false,
                                      source, source_done, s);
        if (s->input) {
            qemu_set_irq(s->pins[2], bit);
        }
    } else if (s->edge_started) {
        listenai_i2s_shift(&s->rx, s->ws, s->width, 1, false, s->data, NULL,
                           sink, s);
        if (s->captured >= s->frames) {
            s->finished = true;
        }
    }
}
static void input(void *opaque, int pin, int level)
{
    ListenaiI2SPeer *s = opaque;
    if (pin == 2) {
        s->data = level;
    } else if (!s->master && pin == 1) {
        s->ws = level;
    } else if (!s->master) {
        sample_clock(s, level);
    }
}
static void tick(void *opaque)
{
    ListenaiI2SPeer *s = opaque;
    if (!s->armed) {
        s->armed = true;
        if (!s->master) {
            if (s->start_ns) {
                /* A late attachment waits for the next WS boundary. */
                s->tx.known = s->rx.known = true;
                s->tx.ws = s->rx.ws = s->ws;
            }
            return;
        }
    }
    bool rising = s->edges & 1;
    if (!rising) {
        unsigned bit = (s->edges / 2) % (2 * s->width);
        s->ws = bit >= s->width;
        qemu_set_irq(s->pins[1], s->ws);
        /* Drive data before the falling clock edge reaches the receiver. */
        sample_clock(s, false);
        qemu_set_irq(s->pins[0], false);
    } else {
        qemu_set_irq(s->pins[0], true);
        sample_clock(s, true);
    }
    s->edges++;
    if (!s->finished) {
        uint64_t hz = (uint64_t)s->rate * s->width * 4;
        timer_mod(s->timer,
                  s->start_ns +
                      (s->edges * UINT64_C(1000000000) + hz - 1) / hz);
    }
}
static void finish(Notifier *notifier, void *unused)
{
    ListenaiI2SPeer *s = container_of(notifier, ListenaiI2SPeer, exit);
    if (s->capture) {
        GError *err = NULL;
        if (!g_file_set_contents(s->output_path, (char *)s->capture->data,
                                 s->capture->len, &err)) {
            error_report("I2S peer capture: %s", err->message);
            g_error_free(err);
        }
    }
}
static void realize(DeviceState *dev, Error **errp)
{
    ListenaiI2SPeer *s = LISTENAI_I2S_PEER(dev);
    if (!s->gpio || s->width < 16 || s->width > 32 ||
        (s->width != 16 && s->width != 20 && s->width != 24 &&
         s->width != 32) ||
        s->rate < 8000 || s->rate > 192000 || !s->frames ||
        s->frames > 1000000 || s->start_ns > UINT64_C(600000000000)) {
        error_setg(errp, "I2S peer requires GPIO, 16/20/24/32-bit words, "
                         "8..192 kHz and 1..1000000 frames");
        return;
    }
    unsigned pins[] = {s->bclk_pin, s->ws_pin, s->input_pin, s->output_pin};
    NamedGPIOList *list;
    unsigned ins = 0, outs = 0;
    QLIST_FOREACH(list, &s->gpio->gpios, node)
    {
        if (list->name && !strcmp(list->name, "pad-in")) {
            ins = list->num_in;
        }
        if (list->name && !strcmp(list->name, "pad-out")) {
            outs = list->num_out;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        if (pins[i] >= ins || pins[i] >= outs) {
            error_setg(errp,
                       "I2S peer pad index is outside the GPIO interface");
            return;
        }
        for (unsigned j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                error_setg(errp, "I2S peer requires four distinct pads");
                return;
            }
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        unsigned pad = i < 2 ? pins[i] : s->output_pin;
        if ((i == 2 || !s->master) &&
            qdev_get_gpio_out_connector(s->gpio, "pad-out", pad)) {
            error_setg(errp,
                       "I2S peer pad output already has a connected resource");
            return;
        }
    }
    if (s->input_path) {
        GError *err = NULL;
        struct stat st;
        if (stat(s->input_path, &st) || !S_ISREG(st.st_mode) ||
            st.st_size != (off_t)s->frames * 8) {
            error_setg(
                errp,
                "I2S peer input must be a regular frames * 8 byte PCM file");
            return;
        }
        if (!g_file_get_contents(s->input_path, (char **)&s->input,
                                 &s->input_length, &err)) {
            error_setg(errp, "I2S peer input: %s", err->message);
            g_error_free(err);
            return;
        }
        if (s->input_length != (size_t)s->frames * 8) {
            error_setg(errp, "I2S peer input must contain frames * 8 bytes of "
                             "stereo signed-32 little-endian PCM");
            return;
        }
    }
    if (s->output_path) {
        s->capture = g_byte_array_new();
    }
    for (unsigned i = 0; i < 3; i++) {
        s->pins[i] = qdev_get_gpio_in_named(s->gpio, "pad-in", pins[i]);
    }
    if (!s->master) {
        qdev_connect_gpio_out_named(s->gpio, "pad-out", s->bclk_pin,
                                    qdev_get_gpio_in(dev, 0));
        qdev_connect_gpio_out_named(s->gpio, "pad-out", s->ws_pin,
                                    qdev_get_gpio_in(dev, 1));
    }
    qdev_connect_gpio_out_named(s->gpio, "pad-out", s->output_pin,
                                qdev_get_gpio_in(dev, 2));
    if (s->input) {
        qemu_set_irq(s->pins[2], 0);
    }
    s->clock = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, s);
    if (s->master) {
        qemu_set_irq(s->pins[0], true);
        qemu_set_irq(s->pins[1], false);
    }
    /* Arm after initial GPIO propagation, even when start-ns is zero. */
    timer_mod(s->timer, s->start_ns);
    s->exit.notify = finish;
    qemu_add_exit_notifier(&s->exit);
}
static void unrealize(DeviceState *dev)
{
    ListenaiI2SPeer *s = LISTENAI_I2S_PEER(dev);
    qemu_remove_exit_notifier(&s->exit);
    timer_free(s->timer);
    g_clear_pointer(&s->input, g_free);
    if (s->capture) {
        g_byte_array_unref(s->capture);
        s->capture = NULL;
    }
}
static const Property properties[] = {
    DEFINE_PROP_LINK("gpio", ListenaiI2SPeer, gpio, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_BOOL("master", ListenaiI2SPeer, master, true),
    DEFINE_PROP_UINT32("bclk-pin", ListenaiI2SPeer, bclk_pin, 4),
    DEFINE_PROP_UINT32("ws-pin", ListenaiI2SPeer, ws_pin, 5),
    DEFINE_PROP_UINT32("input-pin", ListenaiI2SPeer, input_pin, 6),
    DEFINE_PROP_UINT32("output-pin", ListenaiI2SPeer, output_pin, 7),
    DEFINE_PROP_UINT32("rate", ListenaiI2SPeer, rate, 16000),
    DEFINE_PROP_UINT32("width", ListenaiI2SPeer, width, 16),
    DEFINE_PROP_UINT32("frames", ListenaiI2SPeer, frames, 16000),
    DEFINE_PROP_UINT64("start-ns", ListenaiI2SPeer, start_ns, 0),
    DEFINE_PROP_STRING("input-s32le", ListenaiI2SPeer, input_path),
    DEFINE_PROP_STRING("output-s32le", ListenaiI2SPeer, output_path),
};
static void instance_init(Object *obj)
{
    qdev_init_gpio_in(DEVICE(obj), input, 3);
}
static void class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = realize;
    dc->unrealize = unrealize;
    dc->hotpluggable = false;
    dc->desc =
        "External Philips I2S clock/data peer with explicit PCM fixtures";
    device_class_set_props(dc, properties);
}
static const TypeInfo type_info = {
    .name = TYPE_LISTENAI_I2S_PEER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ListenaiI2SPeer),
    .instance_init = instance_init,
    .class_init = class_init,
};
static void register_types(void)
{
    type_register_static(&type_info);
}
type_init(register_types)
