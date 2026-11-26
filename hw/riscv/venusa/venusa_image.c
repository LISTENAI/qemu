/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VenusA image-controller bus, IRQ and clock wiring. */
#include "qemu/osdep.h"
#include "hw/irq.h"
#include "system/address-spaces.h"
#include "hw/riscv/venusa/venusa.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

static void fault(void *opaque, hwaddr address, unsigned size, bool write,
                  uint64_t value)
{ venusa_fail(opaque, address, size, write, value); }
static void dma_irq(void *opaque, unsigned channel, bool level)
{ venusa_irq(opaque, 49 + channel, level); }
static void rgb_irq(void *opaque, bool level)
{ venusa_irq(opaque, 60, level); }
static void jpeg_irq(void *opaque, int n, int level)
{ venusa_irq(opaque, 61, level); }
static void jpeg_fatal(void *opaque, const char *message)
{
    error_report("%s", message);
    venusa_report(opaque, "peripheral-error");
    exit(1);
}
static void request_changed(void *opaque)
{
    VenusASoC *s = opaque;
    if (s->dma2d.event) { venusa_dma2d_request(&s->dma2d); }
}
static void jpeg_request_changed(void *opaque, int n, int level)
{ request_changed(opaque); }
static bool memory_allowed(void *opaque, hwaddr address, unsigned size, bool write)
{
    uint64_t end = address + (uint64_t)size;
    if (!size || end > UINT64_C(0x100000000)) { return false; }
    static const uint32_t ranges[][2] = {
        {0x80000,0x82000}, {0x100000,0x102000}, {0x200000,0x202000},
        {0x300000,0x302000}, {0x20000000,0x20086000},
        {0x38000000,0x39000000}
    };
    for (unsigned i=0;i<G_N_ELEMENTS(ranges);i++) {
        if (address >= ranges[i][0] && end <= ranges[i][1]) { return true; }
    }
    return !write && (end <= 0x8000 ||
        (address >= 0x30000000 && end <= 0x31000000));
}
static bool source_complete(void *opaque, unsigned handshake, hwaddr address)
{
    VenusASoC *s = opaque;
    uint32_t remaining, input;
    bool complete;
    return handshake == 4 && address == 0x42003000 &&
        listenai_jpeg_output_info(&s->jpeg, &remaining, &input, &complete) && complete;
}
static bool request(void *opaque, unsigned handshake, hwaddr address, bool write)
{
    VenusASoC *s = opaque;
    if (handshake == 4 && address == 0x42003000 && !write) {
        return listenai_jpeg_request(&s->jpeg, false);
    }
    if (handshake == 5 && address == 0x42002800 && write) {
        return listenai_jpeg_request(&s->jpeg, true);
    }
    if (handshake != 0 || address != 0x4200b0f4 || !write ||
        (s->syscfg[0xa0 / 4] & ((1u << 16) | 1u))) {
        venusa_fail(s, address, 4, write, handshake);
    }
    return venusa_rgb_request(&s->rgb);
}
void venusa_image_clock(VenusASoC *s)
{
    if (!s->rgb.event || !s->dma2d.event) { return; }
    if (s->jpeg.event) {
        listenai_jpeg_clock(&s->jpeg, (s->syscfg[0x34 / 4] & (1u << 8)) != 0);
    }
    uint32_t cfg = s->syscfg[0x30 / 4];
    if ((cfg & (1u << 26)) && !(s->rgb_clock_config & (1u << 26))) {
        s->rgb_div = (cfg >> 22) & 15;
        if (!s->rgb_div) { s->rgb_div = 16; }
    }
    s->rgb_clock_config = cfg;
    uint64_t n = 24000000, d = s->rgb_div ? s->rgb_div : 1;
    bool enabled = s->rgb_div && (cfg & (1u << 27));
    if (cfg & (1u << 21)) {
        enabled &= (s->buscfg[2] & 1) && (s->buscfg[2] & (1u << 26));
        uint64_t mult = (uint64_t)(s->buscfg[6] & 0x1ff) << 27;
        if (s->buscfg[6] & (1u << 9)) { mult += s->buscfg[5] & 0x7ffffff; }
        n *= mult; d *= (UINT64_C(1) << 27) * (((s->buscfg[3] >> 12) & 15) + 6);
    }
    uint64_t a = n, b = d;
    while (b) { uint64_t r=a%b; a=b; b=r; }
    venusa_rgb_clock(&s->rgb, enabled, n/a, d/a);
    venusa_hclk_ratio(s, &n, &d);
    venusa_dma2d_clock(&s->dma2d, (s->syscfg[0x34/4] & 16) != 0, n, d);
}
void venusa_image_init(VenusASoC *s)
{
    VenusADMA2DCallbacks dma = { .opaque=s, .irq=dma_irq, .fail=fault,
        .memory_allowed=memory_allowed, .request=request, .source_complete=source_complete };
    VenusARGBCallbacks rgb = { .opaque=s, .irq=rgb_irq, .fail=fault,
        .request_changed=request_changed };
    venusa_dma2d_init(&s->dma2d, OBJECT(s), 0x44900000, dma);
    venusa_rgb_init(&s->rgb, OBJECT(s), 0x4200b000, rgb);
    memory_region_add_subregion(get_system_memory(), 0x44900000, &s->dma2d.io);
    memory_region_add_subregion(get_system_memory(), 0x4200b000, &s->rgb.io);
    object_initialize_child(OBJECT(s), "jpeg", &s->jpeg, TYPE_LISTENAI_JPEG);
    s->jpeg.base = 0x42001800;
    s->jpeg.opaque = s; s->jpeg.fail = fault; s->jpeg.fatal = jpeg_fatal;
    SysBusDevice *bus = SYS_BUS_DEVICE(&s->jpeg);
    sysbus_realize(bus, &error_fatal);
    for (unsigned i = 0; i < 9; i++) {
        sysbus_mmio_map(bus, i, s->jpeg.base + s->jpeg.windows[i].offset);
    }
    sysbus_connect_irq(bus, 0, qemu_allocate_irq(jpeg_irq, s, 0));
    for (unsigned i = 1; i < 3; i++) {
        sysbus_connect_irq(bus, i, qemu_allocate_irq(jpeg_request_changed, s, i));
    }
    s->rgb_div = 0;
}
void venusa_image_reset(VenusASoC *s)
{
    s->rgb_div=0; s->rgb_clock_config=0;
    venusa_dma2d_reset(&s->dma2d); venusa_rgb_reset(&s->rgb);
    listenai_jpeg_reset(&s->jpeg);
    venusa_image_clock(s);
}
