/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Digital RGB scanout. A panel is a separate board-level device. */
#include "qemu/osdep.h"
#include "hw/display/venusa_rgb.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"

#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080

static void fail(VenusRGB *s, hwaddr off, bool write, uint32_t value)
{
    s->cb.fail(s->cb.opaque, s->base + off, 4, write, value);
}
static uint32_t raw(VenusRGB *s)
{
    return s->regs[9];
}
static void update(VenusRGB *s)
{
    s->regs[9] |= (!s->count ? 0x14 : 0) |
                  (s->count == s->fifo_capacity ? 0x28 : 0);
    s->cb.irq(s->cb.opaque, (raw(s) & ~s->regs[6] & 63) != 0);
    s->cb.request_changed(s->cb.opaque);
}
static unsigned bpp(VenusRGB *s)
{
    unsigned format = (s->regs[1] >> 4) & 3;
    return format == 0 ? 3 : format == 1 ? 4 : 2;
}
static void sync_pixels(VenusRGB *s);
static void expire(void *opaque);

static unsigned watermark(VenusRGB *s)
{
    static const unsigned burst[3][4] = {{2, 3, 6, 11}, {1, 2, 4, 8}, {2, 4, 8, 16}};
    unsigned fmt = (s->regs[1] >> 4) & 3, sel = s->regs[2] >> 29;
    return fmt < 3 && sel < 4 ? burst[fmt][sel] * bpp(s) : UINT_MAX;
}
bool venusa_rgb_request(VenusRGB *s)
{
    sync_pixels(s);
    return s->clock_enabled && (s->regs[0] & 1) &&
           s->fifo_capacity - s->count >= watermark(s);
}
static Int128 multiply(uint64_t a, uint64_t b)
{
    uint64_t lo, hi;
    mulu64(&lo, &hi, a, b);
    return int128_make128(lo, hi);
}
static void sync_clock(VenusRGB *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->active && s->clock_enabled && s->clock_n) {
        s->clock_work = int128_sub(s->clock_work,
                                  multiply(now - s->event_epoch, s->clock_n));
    }
    s->event_epoch = now;
}
static void arm(VenusRGB *s)
{
    if (!s->clock_enabled || !s->clock_n || !s->active) { return; }
    Int128 rounded = int128_add(s->clock_work, int128_make64(s->clock_n - 1));
    Int128 wait = int128_divs(rounded, int128_make64(s->clock_n));
    if (!int128_le(wait, int128_make64(INT64_MAX - s->event_epoch))) {
        fail(s, 0, true, s->regs[0]); return;
    }
    timer_mod(s->event, s->event_epoch + MAX(INT64_C(1), int128_get64(wait)));
}
static void schedule(VenusRGB *s, uint64_t cycles)
{
    s->clock_work = int128_add(s->clock_work,
                              multiply(MAX(UINT64_C(1), cycles) * 1000000000,
                                       s->clock_d));
    arm(s);
}
static unsigned hblank(VenusRGB *s)
{
    return (s->regs[2] & 255) + ((s->regs[2] >> 16) & 15) + (s->regs[3] & 255);
}
static void capture(VenusRGB *s)
{
    if (!s->capture_path) { return; }
    g_autofree char *header = g_strdup_printf("P6\n%u %u\n255\n", s->width, s->height);
    size_t head_len = strlen(header), pixels = (size_t)s->width * s->height * 3;
    g_autofree uint8_t *output = g_malloc(head_len + pixels);
    memcpy(output, header, head_len);
    memcpy(output + head_len, s->pixels, pixels);
    GError *error = NULL;
    if (!g_file_set_contents(s->capture_path, (const char *)output, head_len + pixels, &error)) {
        error_report("RGB capture failed: %s", error->message);
        g_error_free(error);
        fail(s, 0xf4, true, 0);
    }
}
static uint8_t take(VenusRGB *s)
{
    uint8_t value = s->fifo[s->head];
    s->head = (s->head + 1) % s->fifo_capacity;
    s->count--;
    return value;
}
static void pixel(VenusRGB *s)
{
    unsigned bytes = bpp(s);
    if (s->count < bytes) {
        s->underflow_pixels++;
        s->valid_frame = false;
        s->invalid_frames++;
        s->starved = true;
        s->active = false;
        s->clock_work = int128_zero();
        timer_del(s->event);
        return;
    }
    uint8_t r, g, b;
    if (bytes == 2) {
        uint16_t v = take(s); v |= (uint16_t)take(s) << 8;
        r = ((v >> 11) & 31) * 255 / 31;
        g = ((v >> 5) & 63) * 255 / 63;
        b = (v & 31) * 255 / 31;
    } else {
        b = take(s); g = take(s); r = take(s);
        if (bytes == 4) { take(s); }
    }
    if (s->regs[2] & (1u << 28)) { uint8_t swap = r; r = b; b = swap; }
    unsigned fmt = (s->regs[1] >> 6) & 3;
    if (fmt == 1) { r = (r >> 2) * 255 / 63; g = (g >> 2) * 255 / 63; b = (b >> 2) * 255 / 63; }
    if (fmt == 2) { r = (r >> 3) * 255 / 31; g = (g >> 2) * 255 / 63; b = (b >> 3) * 255 / 31; }
    if (s->pixels) {
        uint8_t *p = s->pixels + ((size_t)s->y * s->width + s->x) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
}
static void sync_pixels(VenusRGB *s)
{
    if (s->syncing || !s->active || !s->clock_enabled) { return; }
    s->syncing = true;
    sync_clock(s);
    if (s->phase == 1) {
        Int128 tick = multiply(UINT64_C(1000000000), s->clock_d);
        Int128 left = int128_divs(int128_add(s->clock_work,
                                 int128_sub(tick, int128_one())), tick);
        unsigned remaining = int128_le(left, int128_zero()) ? 0 : int128_get64(left);
        unsigned due = s->batch - MIN(s->batch, remaining);
        bool changed = due != 0;
        while (due-- && s->active) { pixel(s); s->x++; s->batch--; }
        if (s->active && !s->batch && int128_le(s->clock_work, int128_zero())) {
            expire(s);
        } else if (changed) {
            update(s);
        }
    }
    s->syncing = false;
}
static void begin_pixels(VenusRGB *s)
{
    s->phase = 1;
    /* Stop at the next observable FIFO waterline, empty state or line end.
     * MMIO and DMA request queries materialize pixels at their actual ticks. */
    s->batch = MIN(32u, MIN(s->width - s->x, MAX(1u, s->count / bpp(s))));
    unsigned vacancy = s->fifo_capacity - s->count, mark = watermark(s);
    if (vacancy < mark && mark <= s->fifo_capacity) {
        s->batch = MIN(s->batch, (mark - vacancy + bpp(s) - 1) / bpp(s));
    }
    schedule(s, s->batch);
}
static void expire(void *opaque)
{
    VenusRGB *s = opaque;
    if (!s->active || !s->clock_enabled) { return; }
    sync_clock(s);
    if (s->phase == 0) {
        s->regs[9] = (s->regs[9] & ~1u) | 2;
        s->valid_frame = true;
        s->x = s->y = 0;
        s->phase = 2;
        unsigned vpre = ((s->regs[2] >> 8) & 255) + ((s->regs[2] >> 20) & 15);
        unsigned hpre = (s->regs[2] & 255) + ((s->regs[2] >> 16) & 15);
        schedule(s, (uint64_t)vpre * (s->width + hblank(s)) + hpre);
    } else if (s->phase == 2) {
        begin_pixels(s);
    } else {
        while (s->batch && s->active) { pixel(s); s->x++; s->batch--; }
        if (!s->active) { update(s); return; }
        if (s->x < s->width) { begin_pixels(s); }
        else if (++s->y < s->height) {
            s->x = 0; s->phase = 2; schedule(s, hblank(s));
        } else {
            if (s->valid_frame) {
                s->regs[9] = (s->regs[9] & ~2u) | 1;
                s->frames++;
                capture(s);
            } else {
                /* An incomplete input frame does not assert frame done. */
                s->invalid_frames++;
            }
            if (s->regs[0] & 16) {
                s->phase = 0;
                schedule(s, (uint64_t)((s->regs[3] >> 8) & 255) * (s->width + hblank(s)) +
                            (s->regs[3] & 255) + 1);
            } else { s->active = false; s->clock_work = int128_zero(); }
        }
    }
    update(s);
}
static void start(VenusRGB *s)
{
    if (s->starved) {
        error_report("VenusA RGB restart after FIFO underrun requires controller reset");
        fail(s, 0, true, s->regs[0]);
        return;
    }
    s->width = s->regs[4] & 65535; s->height = s->regs[4] >> 16;
    unsigned in = (s->regs[1] >> 4) & 3, out = (s->regs[1] >> 6) & 3;
    if (!(s->regs[0] & 1) || !s->width || !s->height ||
        s->width > MAX_WIDTH || s->height > MAX_HEIGHT || in == 3 || out == 3 ||
        (s->regs[1] & (1u << 10)) || ((s->regs[1] >> 8) & 3) == 3 ||
        (s->regs[2] >> 29) > 3) {
        fail(s, 0, true, s->regs[0]); return;
    }
    timer_del(s->event);
    if (s->capture_path) { s->pixels = g_realloc(s->pixels, (size_t)s->width * s->height * 3); }
    s->active = true; s->valid_frame = true; s->phase = 0;
    s->clock_work = int128_zero();
    s->event_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    schedule(s, 4);
    update(s);
}
static uint64_t read_reg(void *opaque, hwaddr off, unsigned size)
{
    VenusRGB *s = opaque;
    sync_pixels(s);
    if (size != 4 || (off & 3) || off > 0x24) { fail(s, off, false, 0); return 0; }
    if (off == 0x14) { return !!(raw(s) & ~s->regs[6] & 63); }
    if (off == 0x1c) { return 0; }
    if (off == 0x20) { return raw(s) & ~s->regs[6] & 63; }
    if (off == 0x24) { return raw(s); }
    return s->regs[off / 4];
}
static void write_reg(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    VenusRGB *s = opaque;
    sync_pixels(s);
    uint32_t v = value;
    if (size != 4 || (off & 3)) { fail(s, off, true, v); return; }
    if (off == 0xf4) {
        if (!s->clock_enabled || s->fifo_capacity - s->count < 4) { fail(s, off, true, v); return; }
        for (unsigned i = 0; i < 4; i++) {
            s->fifo[(s->head + s->count++) % s->fifo_capacity] = v >> (i * 8);
        }
        update(s); return;
    }
    switch (off) {
    case 0:
        if (v & ~0x11111u) { fail(s, off, true, v); return; }
        s->regs[0] = v & ~0x100u;
        if (!(v & 1)) { s->active = false; timer_del(s->event); s->clock_work = int128_zero(); }
        if (v & 0x100) { start(s); }
        break;
    case 4:
        if (v & ~0x307f1u) { fail(s, off, true, v); return; }
        if (s->count && ((v ^ s->regs[1]) & 0x30) && !(v & 0x30000)) {
            fail(s, off, true, v); return;
        }
        if (v & 0x30000) { s->head = s->count = 0; }
        if (v & 1) { s->active = false; timer_del(s->event); s->clock_work = int128_zero(); }
        s->regs[1] = v & ~0x30001u;
        s->fifo_capacity = 32 * bpp(s);
        break;
    case 8: case 12: case 16:
        if (s->active) { fail(s, off, true, v); return; }
        s->regs[off / 4] = v; break;
    case 0x18:
        if (v & ~63u) { fail(s, off, true, v); return; }
        s->regs[6] = v; break;
    case 0x1c:
        if (v & ~63u) { fail(s, off, true, v); return; }
        s->regs[9] &= ~v; break;
    default: fail(s, off, true, v); return;
    }
    update(s);
}
static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
void venusa_rgb_reset(VenusRGB *s)
{
    timer_del(s->event);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[2] = 0x00440c2b;
    s->regs[3] = 0x00000c2b;
    s->regs[4] = 0x002b002b;
    s->regs[6] = 63;
    s->fifo_capacity = 96;
    s->head = s->count = 0; s->active = false;
    s->starved = s->syncing = false;
    s->clock_work = int128_zero();
    s->event_epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    update(s);
}
void venusa_rgb_clock(VenusRGB *s, bool enabled, uint64_t n, uint64_t d)
{
    sync_pixels(s);
    sync_clock(s);
    timer_del(s->event);
    if (!d) { fail(s, 0, true, 0); return; }
    if (s->clock_d && s->clock_d != d) {
        /* Keep the outstanding fraction of a pixel across divider changes. */
        Int128 old = int128_make64(s->clock_d);
        uint64_t whole = int128_get64(int128_divu(s->clock_work, old));
        uint64_t rest = int128_get64(int128_remu(s->clock_work, old));
        s->clock_work = int128_add(multiply(whole, d),
                                  int128_divu(multiply(rest, d), old));
    }
    s->clock_enabled = enabled; s->clock_n = n; s->clock_d = d;
    arm(s);
    update(s);
}
void venusa_rgb_init(VenusRGB *s, Object *owner, hwaddr base, VenusRGBCallbacks cb)
{
    s->base = base; s->cb = cb;
    s->capture_path = g_strdup(g_getenv("LISEM_QEMU_RGB_CAPTURE"));
    s->event = timer_new_ns(QEMU_CLOCK_VIRTUAL, expire, s);
    memory_region_init_io(&s->io, owner, &ops, s, "venusa-rgb", 0x1000);
    venusa_rgb_reset(s);
}
