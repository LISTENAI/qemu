/* SPDX-License-Identifier: GPL-2.0-or-later */
/* BL6138 host interface. Register layout and flash command framing follow
 * the BL6xxx host driver. Flash starts erased; firmware supplies its image.
 * Touch scanning is functional, without executing the touch MCU instruction set.
 */
#include "qemu/osdep.h"
#include "hw/i2c/bl6138.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define FLASH_SIZE 32768
#define WRITE_SIZE 128
#define REPORT_SIZE 6

/* Estimated completion times for the BL6xxx digital programmer. The host
 * driver's 120/50/5 ms reset, loader/erase and page-write sleeps are polling
 * margins, not minimum device busy times. Tick-based waits can resume in the
 * preceding millisecond. Flash writes remain pending and reset-cancellable. */
#define RESET_RECOVERY_NS 60000000
#define LOADER_ENTRY_NS   25000000
#define FLASH_ERASE_NS    25000000
#define FLASH_WRITE_NS     2000000

OBJECT_DECLARE_SIMPLE_TYPE(BL6138State, BL6138)
struct BL6138State {
    I2CSlave parent_obj;
    char *filename;
    uint8_t flash[FLASH_SIZE], command[6 + WRITE_SIZE], payload[WRITE_SIZE];
    unsigned sent, read_size, pos, start, pending_address, pending_size;
    uint8_t op, reply[REPORT_SIZE], point_event;
    unsigned x, y;
    bool pressed, reset_low, int_high, sleeping, updating, sending;
    bool pending_erase, checksum_started;
    uint64_t generation, read_generation;
    int64_t ready_ns, checksum_ns;
    qemu_irq irq;
    QEMUTimer *flash_timer, *irq_timer;
};

static bool application_valid(BL6138State *s)
{
    return s->flash[0x5fc] == 0xaa && s->flash[0x5fd] == 0x55;
}

static void flash_complete(void *opaque)
{
    BL6138State *s = opaque;
    if (s->pending_erase) {
        memset(s->flash, 0xff, sizeof(s->flash));
    } else {
        for (unsigned i = 0; i < s->pending_size; i++) {
            s->flash[s->pending_address + i] &= s->payload[i];
        }
    }
    s->pending_size = 0;
    s->pending_erase = false;
    if (s->filename) {
        GError *error = NULL;
        if (!g_file_set_contents(s->filename, (char *)s->flash, FLASH_SIZE, &error)) {
            error_report("BL6138 flash write failed: %s", error->message);
            g_error_free(error);
            exit(1);
        }
    }
}

static void irq_end(void *opaque)
{
    BL6138State *s = opaque;
    qemu_set_irq(s->irq, 1);
}

void bl6138_redrive(DeviceState *dev)
{
    BL6138State *s = BL6138(dev);
    qemu_set_irq(s->irq, !timer_pending(s->irq_timer));
}

static void reset(DeviceState *dev)
{
    BL6138State *s = BL6138(dev);
    timer_del(s->flash_timer);
    timer_del(s->irq_timer);
    s->pending_size = s->read_size = s->sent = 0;
    s->pending_erase = s->checksum_started = s->sending = false;
    s->sleeping = s->updating = s->pressed = false;
    s->point_event = 3;
    s->generation++;
    s->ready_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    qemu_set_irq(s->irq, 1);
}

static void reset_pin(void *opaque, int pin, int level)
{
    BL6138State *s = opaque;
    if (s->reset_low == !level) {
        return;
    }
    s->reset_low = !level;
    if (s->reset_low) {
        reset(DEVICE(s));
    } else {
        s->ready_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RESET_RECOVERY_NS;
    }
}

static void int_pin(void *opaque, int pin, int level)
{
    BL6138State *s = opaque;
    s->int_high = !!level;
}

static unsigned command_length(BL6138State *s)
{
    const uint8_t *c = s->command;
    switch (c[0]) {
    case 0x01: case 0xf9: return !s->updating && application_valid(s);
    case 0xee: case 0xb6: case 0x3f: return !s->updating;
    case 0xa5: case 0x8a: return s->updating ? 0 : 2;
    case 0x55: return 3;
    case 0x0a: return 4; /* Three bytes select ID; four enter the loader. */
    case 0x09: return s->updating ? 2 : 0;
    case 0x0d: return s->updating ? 6 : 0;
    case 0x0f:
        if (!s->updating) { return 0; }
        if (s->sent < 6) { return 6; }
        unsigned start = lduw_be_p(c + 2), end = lduw_be_p(c + 4);
        return end >= start && end < FLASH_SIZE && end - start < WRITE_SIZE ?
               7 + end - start : 0;
    default: return 0;
    }
}

static int touch_send(I2CSlave *dev, uint8_t value)
{
    BL6138State *s = BL6138(dev);
    if (!s->sending || s->sent >= sizeof(s->command)) {
        s->sending = false;
        return 1;
    }
    s->command[s->sent++] = value;
    unsigned n = s->sent;
    uint8_t op = s->command[0];
    bool valid = n <= command_length(s);
    if (n == 2) {
        switch (op) {
        case 0xa5: valid &= value == 3; break;
        case 0x8a: valid &= value == 0xaa; break;
        case 0x55: valid &= value == 0x46; break;
        case 0x0a: valid &= value == 0xf5; break;
        case 0x09: case 0x0d: case 0x0f: valid &= value == (uint8_t)~op; break;
        }
    }
    if (op == 0x55 && n == 3) { valid &= value == 0x4f; }
    if (op == 0x0a && n == 3) { valid &= value == 0xe7; }
    if (op == 0x0a && n == 4) { valid &= value == 0xe8 && !s->int_high; }
    if (op == 0x0d && n == 6) {
        unsigned start = lduw_be_p(s->command + 2), end = lduw_be_p(s->command + 4);
        valid &= end >= start && end < FLASH_SIZE;
    }
    if (!valid) {
        s->sending = false;
        return 1;
    }
    return 0;
}

static void finish_command(BL6138State *s)
{
    uint8_t *c = s->command;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->sending = false;
    if (!s->sent) { return; }
    s->op = c[0];
    s->pos = 0;
    if (s->op == 0x0a && s->sent == 3 && s->updating) {
        s->reply[0] = 0x23; /* Supported BL6xxx flash ID profile. */
        s->read_size = 1;
        return;
    }
    if (s->sent != command_length(s)) { return; }
    switch (s->op) {
    case 0x0a:
        s->updating = true;
        s->ready_ns = now + LOADER_ENTRY_NS;
        break;
    case 0x55: s->updating = false; s->sleeping = false; break;
    case 0x09:
        s->pending_erase = true;
        timer_mod(s->flash_timer, now + FLASH_ERASE_NS);
        break;
    case 0x0f:
        s->pending_address = lduw_be_p(c + 2);
        s->pending_size = s->sent - 6;
        memcpy(s->payload, c + 6, s->pending_size);
        timer_mod(s->flash_timer, now + FLASH_WRITE_NS);
        break;
    case 0x0d:
        s->start = lduw_be_p(c + 2);
        s->read_size = lduw_be_p(c + 4) - s->start + 1;
        break;
    case 0x8a:
        s->checksum_started = true;
        s->checksum_ns = now + 250000000;
        break;
    case 0x3f: {
        unsigned sum = 0;
        bool ready = s->checksum_started && now >= s->checksum_ns;
        if (ready) {
            for (unsigned i = 0; i < FLASH_SIZE; i++) { sum += s->flash[i]; }
        }
        s->reply[0] = ready;
        stw_be_p(s->reply + 1, sum);
        s->read_size = 3;
        break;
    }
    case 0xb6: memcpy(s->reply, s->flash + 0x22a, 3); s->read_size = 3; break;
    case 0xee: s->reply[0] = 0; s->read_size = 1; break; /* unprotected */
    case 0xa5: s->sleeping = true; break;
    case 0xf9:
        /* Free-running scan heartbeat; repeated reads do not advance it. */
        stl_le_p(s->reply, (now - s->ready_ns) / 10000000);
        s->read_size = 4;
        break;
    case 0x01:
        s->reply[0] = 0;
        s->reply[1] = s->pressed;
        s->reply[2] = (s->point_event << 6) | (s->x >> 8);
        s->reply[3] = s->x;
        s->reply[4] = s->y >> 8;
        s->reply[5] = s->y;
        s->read_size = REPORT_SIZE;
        s->read_generation = s->generation;
        break;
    }
}

static int event(I2CSlave *dev, enum i2c_event event)
{
    BL6138State *s = BL6138(dev);
    if ((event == I2C_START_SEND || event == I2C_START_RECV ||
         event == I2C_FINISH) && s->sending) {
        finish_command(s);
    }
    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        if (s->reset_low || s->sleeping || s->pending_erase || s->pending_size ||
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->ready_ns) {
            return 1;
        }
        if (event == I2C_START_SEND) {
            s->sending = true;
            s->read_size = s->sent = 0;
        } else if (s->pos >= s->read_size) {
            return 1;
        }
    }
    return 0;
}

static uint8_t touch_recv(I2CSlave *dev)
{
    BL6138State *s = BL6138(dev);
    if (s->pos >= s->read_size) { return 0xff; }
    unsigned pos = s->pos++;
    if (s->op == 0x01 && s->pos == REPORT_SIZE &&
        s->read_generation == s->generation) {
        s->point_event = s->pressed ? 2 : 3;
    }
    return s->op == 0x0d ? s->flash[s->start + pos] : s->reply[pos];
}

static void contact_get(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    BL6138State *s = opaque;
    g_autofree char *value = s->pressed ? g_strdup_printf("%u,%u,press", s->x, s->y) :
                                         g_strdup("release");
    visit_type_str(v, name, &value, errp);
}

static void contact_set(Object *obj, Visitor *v, const char *name, void *opaque,
                        Error **errp)
{
    BL6138State *s = opaque;
    g_autofree char *value = NULL;
    unsigned x = s->x, y = s->y;
    char extra;
    if (!visit_type_str(v, name, &value, errp)) { return; }
    bool press = strcmp(value, "release") != 0;
    int consumed = 0;
    if (press && (sscanf(value, "%u,%u,press%n%c", &x, &y, &consumed, &extra) != 2 ||
                  !consumed || x >= 240 || y >= 320)) {
        error_setg(errp, "Touch input must be x,y,press within 240x320, or release");
        return;
    }
    if (press == s->pressed && x == s->x && y == s->y) { return; }
    s->point_event = press ? (s->pressed ? 2 : 0) : 1;
    s->pressed = press;
    s->x = x;
    s->y = y;
    s->generation++;
    if (application_valid(s) && !s->reset_low && !s->sleeping && !s->updating &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->ready_ns) {
        qemu_set_irq(s->irq, 0);
        timer_mod(s->irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

void bl6138_add_input(Object *owner, DeviceState *dev, const char *name)
{
    object_property_add(owner, name, "str", contact_get, contact_set, NULL, dev);
}

static void realize(DeviceState *dev, Error **errp)
{
    BL6138State *s = BL6138(dev);
    memset(s->flash, 0xff, sizeof(s->flash));
    if (s->filename) {
        FILE *f = fopen(s->filename, "rb");
        if (f) {
            bool valid = fread(s->flash, 1, FLASH_SIZE, f) == FLASH_SIZE &&
                         fgetc(f) == EOF && !ferror(f);
            fclose(f);
            if (!valid) {
                error_setg(errp, "BL6138 flash image must be exactly 32768 bytes");
                return;
            }
        } else if (errno != ENOENT) {
            error_setg_errno(errp, errno, "Opening BL6138 flash");
            return;
        }
    }
    s->flash_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, flash_complete, s);
    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, irq_end, s);
}

static void unrealize(DeviceState *dev)
{
    BL6138State *s = BL6138(dev);
    timer_free(s->flash_timer);
    timer_free(s->irq_timer);
}

static void instance_init(Object *obj)
{
    BL6138State *s = BL6138(obj);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "int", 1);
    qdev_init_gpio_in_named(DEVICE(obj), reset_pin, "reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), int_pin, "int-level", 1);
}

static const Property properties[] = {
    DEFINE_PROP_STRING("flash-file", BL6138State, filename),
};

static void class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);
    ic->send = touch_send;
    ic->recv = touch_recv;
    ic->event = event;
    dc->realize = realize;
    dc->unrealize = unrealize;
    device_class_set_legacy_reset(dc, reset);
    device_class_set_props(dc, properties);
}

static const TypeInfo info = {
    .name = TYPE_BL6138, .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(BL6138State), .instance_init = instance_init,
    .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&info);
}
type_init(register_types)
