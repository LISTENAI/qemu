/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define DMA 0x44900000
#define RGB 0x4200b000
#define RAM 0x20010000
#define CTRL_COPY (1u | (2u << 1) | (2u << 3) | (2u << 5) | (2u << 13) | (2u << 15))
#define CTRL_RGB (1u | (1u << 1) | (2u << 3) | (2u << 5) | (2u << 10) | (2u << 13) | (2u << 15))
static char *flash, *capture_path;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s", path);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_writel(q, 0x43100030, (1u << 27) | (1u << 26) | (1u << 22));
    qtest_writel(q, 0x43100014, (1u << 16) | (1u << 25));
    unlink(capture_path);
    return q;
}
static void descriptor(QTestState *q, unsigned ch, uint32_t src, uint32_t dst, uint32_t size)
{
    qtest_writel(q, DMA + 0xa0 + ch * 4, src);
    qtest_writel(q, DMA + 0xc0 + ch * 4, dst);
    qtest_writel(q, DMA + 0xe0 + ch * 4, size);
    qtest_writel(q, DMA + 0x200 + ch * 4, 1);
}
static void normal_shadow(void)
{
    QTestState *q = start();
    for (unsigned i = 0; i < 32; i++) { qtest_writel(q, RAM + i * 4, 0x12340000 + i); }
    for (unsigned ch = 0; ch < 6; ch++) {
        g_assert_cmphex(qtest_readl(q, DMA + ch * 4), ==, 0x0019e040);
        qtest_writel(q, DMA + ch * 4, CTRL_COPY);
        qtest_writel(q, DMA + 0x220 + ch * 4, 1);
        qtest_writel(q, DMA + 0x240 + ch * 4, 0x1e);
        descriptor(q, ch, RAM, RAM + 0x1000 + ch * 256, 32);
        g_assert_cmpuint(qtest_readl(q, DMA + 0x200 + ch * 4), ==, 0);
        descriptor(q, ch, RAM + 32, RAM + 0x1020 + ch * 256, 32);
        g_assert_cmpuint(qtest_readl(q, DMA + 0x200 + ch * 4), ==, 1);

    }
    qtest_clock_step(q, 100000);
    for (unsigned ch = 0; ch < 6; ch++) {
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(qtest_readl(q, RAM + 0x1000 + ch * 256 + i * 4), ==, 0x12340000 + i);
        }
        g_assert_cmpuint(qtest_readl(q, DMA + 0x340 + ch * 4), ==, 1);
        g_assert_cmpuint(qtest_readl(q, DMA + 0x320 + ch * 4), ==, 0);
        g_assert_cmphex(qtest_readl(q, DMA + 0x280 + ch * 4) & 5, ==, 5);
        g_assert_cmpuint(qtest_readb(q, 0xe0021000 + (49 + ch) * 4), ==, 1);
        qtest_writel(q, DMA + 0x260 + ch * 4, 1);
        g_assert_cmpuint(qtest_readb(q, 0xe0021000 + (49 + ch) * 4), ==, 0);
    }
    qtest_quit(q);
}
static void cancel_gate(void)
{
    QTestState *q = start();
    qtest_writel(q, RAM, 0x76543210);
    qtest_writel(q, DMA, CTRL_COPY);
    descriptor(q, 0, RAM, RAM + 0x100, 4);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) & ~16u);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x100), ==, 0);
    qtest_writel(q, 0x43100034, qtest_readl(q, 0x43100034) | 16);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x100), ==, 0x76543210);
    descriptor(q, 0, RAM, RAM + 0x104, 4);
    qtest_writel(q, DMA + 0x360, 1);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x104), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280) & 16, ==, 16);
    descriptor(q, 0, RAM, RAM + 0x108, 4);
    qtest_writel(q, 0x43100014, 1u << 16);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x108), ==, 0);
    qtest_quit(q);
}
static void rgb_pixels(void)
{
    QTestState *q = start();
    const uint16_t values[] = { 0xf800,0x07e0,0x001f,0xffff,0,0xffe0,0xf81f,0x07ff };
    for (unsigned i = 0; i < 8; i++) { qtest_writew(q, RAM + i * 2, values[i]); }
    qtest_writel(q, RGB + 4, 0x30001 | (2u << 4) | (2u << 6));
    qtest_writel(q, RGB + 8, 1 | (1u << 8) | (1u << 16) | (1u << 20));
    qtest_writel(q, RGB + 12, 1 | (1u << 8));
    qtest_writel(q, RGB + 16, 4 | (2u << 16));
    qtest_writel(q, RGB + 0x18, 0x3c);
    qtest_writel(q, DMA, CTRL_RGB);
    descriptor(q, 0, RAM, RGB + 0xf4, 16);
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 60 * 4), ==, 1);
    qtest_writel(q, RGB + 0x1c, 3);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 60 * 4), ==, 0);
    gchar *contents = NULL; gsize len = 0;
    g_assert_true(g_file_get_contents(capture_path, &contents, &len, NULL));
    const uint8_t expected[] = {255,0,0,0,255,0,0,0,255,255,255,255,
                               0,0,0,255,255,0,255,0,255,0,255,255};
    g_assert_cmpuint(len, ==, 11 + sizeof(expected));
    g_assert_cmpmem(contents, 11, "P6\n4 2\n255\n", 11);
    g_assert_cmpmem(contents + 11, sizeof(expected), expected, sizeof(expected));
    g_free(contents);
    qtest_quit(q);
}
static void starvation_no_frame(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x30021);
    qtest_writel(q, RGB + 8, 0);
    qtest_writel(q, RGB + 12, 0);
    qtest_writel(q, RGB + 16, 4 | (2u << 16));
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x17, ==, 0x16);
    g_assert_false(g_file_test(capture_path, G_FILE_TEST_EXISTS));
    for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    g_assert_false(g_file_test(capture_path, G_FILE_TEST_EXISTS));
    qtest_writel(q, 0x43100014, 1u << 25);
    qtest_writel(q, RGB + 4, 0xa0);
    qtest_writel(q, RGB + 8, 0x00110101);
    qtest_writel(q, RGB + 12, 0x101);
    qtest_writel(q, RGB + 16, 0x00020004);
    for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    g_assert_true(g_file_test(capture_path, G_FILE_TEST_EXISTS));
    qtest_quit(q);
}
static void rgb_clock_boundaries(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x30021);
    qtest_writel(q, RGB + 8, 1 | (1u << 8) | (1u << 16) | (1u << 20));
    qtest_writel(q, RGB + 12, 1 | (1u << 8));
    qtest_writel(q, RGB + 16, 4 | (2u << 16));
    for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_writel(q, RGB, 0x101);
    qtest_clock_step(q, 83); /* Preserve a fractional startup pixel across a divider change. */
    uint32_t cfg = qtest_readl(q, 0x43100030);
    qtest_writel(q, 0x43100030, (cfg & ~((1u << 26) | (15u << 22))) | (2u << 22));
    qtest_writel(q, 0x43100030, (cfg & ~(15u << 22)) | (2u << 22));
    qtest_clock_step(q, 167);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 2, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 2, ==, 2);
    cfg = qtest_readl(q, 0x43100030);
    qtest_writel(q, 0x43100030, cfg & ~(1u << 27));
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, 0);
    qtest_writel(q, 0x43100030, cfg);
    qtest_clock_step(q, 2249); /* 27 remaining pixel ticks at 12MHz, including the fraction. */
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, 1);
    qtest_quit(q);
}
static void rgb_pixel_deadline(void)
{
    for (unsigned late = 0; late < 2; late++) {
        QTestState *q = start();
        qtest_writel(q, RGB + 4, 0x300a0);
        qtest_writel(q, RGB + 8, 0x00110101);
        qtest_writel(q, RGB + 12, 0x101);
        qtest_writel(q, RGB + 16, 0x00020004);
        qtest_writel(q, RGB, 0x101);
        qtest_clock_step(q, late ? 876 : 874);
        for (unsigned i = 0; i < 4; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
        qtest_clock_step(q, 10000);
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 1, ==, !late);
        g_assert_cmpint(g_file_test(capture_path, G_FILE_TEST_EXISTS), ==, !late);
        qtest_quit(q);
    }
}
static void rgb_frame_period(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x300a0);
    qtest_writel(q, RGB + 8, 0x00110101);
    qtest_writel(q, RGB + 12, 0x101);
    qtest_writel(q, RGB + 16, 0x00020004);
    for (unsigned i = 0; i < 8; i++) { qtest_writel(q, RGB + 0xf4, 0xf80007e0); }
    qtest_writel(q, RGB, 0x111);
    qtest_clock_step(q, 167);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    qtest_clock_step(q, 1125);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    qtest_clock_step(q, 374);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    qtest_clock_step(q, 1); /* 36 clocks between consecutive SOF edges. */
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    qtest_clock_step(q, 1125);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 1);
    qtest_clock_step(q, 375);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2);
    qtest_clock_step(q, 1125);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 3, ==, 2); /* No third frame data. */
    qtest_quit(q);
}
static void rgb_packed_formats(void)
{
    for (unsigned input = 0; input < 2; input++) {
        for (unsigned output = 0; output < 3; output++) {
            for (unsigned swap = 0; swap < 2; swap++) {
                QTestState *q = start();
                qtest_writel(q, RGB + 4, input << 4 | output << 6);
                qtest_writel(q, RGB + 8, 0x00110101 | swap << 28);
                qtest_writel(q, RGB + 12, 0x101);
                qtest_writel(q, RGB + 16, 0x00020004);
                uint8_t bytes[32], expected[24];
                unsigned size = input ? 4 : 3;
                for (unsigned i = 0; i < 8; i++) {
                    bytes[i * size] = 0x96;
                    bytes[i * size + 1] = 0x5a;
                    bytes[i * size + 2] = 0x33;
                    if (input) { bytes[i * size + 3] = 0xa5; }
                    uint8_t r = swap ? 0x96 : 0x33, g = 0x5a, b = swap ? 0x33 : 0x96;
                    if (output == 1) { r = (r >> 2) * 255 / 63; g = (g >> 2) * 255 / 63; b = (b >> 2) * 255 / 63; }
                    if (output == 2) { r = (r >> 3) * 255 / 31; g = (g >> 2) * 255 / 63; b = (b >> 3) * 255 / 31; }
                    expected[i * 3] = r; expected[i * 3 + 1] = g; expected[i * 3 + 2] = b;
                }
                for (unsigned i = 0; i < size * 8; i += 4) {
                    qtest_writel(q, RGB + 0xf4, (uint32_t)bytes[i] | (uint32_t)bytes[i+1] << 8 |
                                 (uint32_t)bytes[i+2] << 16 | (uint32_t)bytes[i+3] << 24);
                }
                qtest_writel(q, RGB, 0x101);
                qtest_clock_step(q, 100000);
                gchar *contents = NULL; gsize len = 0;
                g_assert_true(g_file_get_contents(capture_path, &contents, &len, NULL));
                g_assert_cmpuint(len, ==, 35);
                g_assert_cmpmem(contents + 11, sizeof(expected), expected, sizeof(expected));
                g_free(contents);
                qtest_quit(q);
            }
        }
    }
}
static void rgb_fifo_latches(void)
{
    QTestState *q = start();
    qtest_writel(q, RGB + 4, 0x300a1);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x14);
    for (unsigned i = 0; i < 16; i++) { qtest_writel(q, RGB + 0xf4, i); }
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x3c);
    qtest_writel(q, RGB + 0x1c, 63);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x28);
    qtest_writel(q, RGB + 4, 0x300a0);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x3c);
    qtest_writel(q, RGB + 0x1c, 63);
    g_assert_cmphex(qtest_readl(q, RGB + 0x24), ==, 0x14);
    qtest_quit(q);
}
static void shadow_irq_and_byte_lanes(void)
{
    QTestState *q = start();
    for (unsigned i = 0; i < 24; i++) { qtest_writeb(q, DMA + 0x220 + i, 0); }
    qtest_writel(q, RAM, 0x12345678);
    qtest_writel(q, DMA, CTRL_COPY);
    qtest_writel(q, DMA + 0x220, 1);
    qtest_writel(q, DMA + 0x240, 30);
    descriptor(q, 0, RAM, RAM + 0x1000, 16);
    qtest_writeb(q, DMA + 0x220, 0);
    qtest_writeb(q, DMA + 0x240, 31);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, DMA + 0x440), ==, 0x80000010);
    g_assert_cmphex(qtest_readl(q, DMA + 0x380), ==, RAM);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3a0), ==, RAM + 0x1000);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2a0), ==, 1);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 49 * 4), ==, 1);
    qtest_writeb(q, DMA + 0x260, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x280), ==, 4);
    g_assert_cmpuint(qtest_readb(q, 0xe0021000 + 49 * 4), ==, 0);
    qtest_quit(q);
}
static void narrow_and_address_modes(void)
{
    QTestState *q = start();
    uint8_t source[64], actual[32], expected[32], guard[32];
    for (unsigned i = 0; i < sizeof(source); i++) { source[i] = i + 1; }
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(q, RAM, source, sizeof(source));
    for (unsigned width = 0; width < 3; width++) {
        unsigned unit = 1u << width;
        for (unsigned mode = 0; mode < 3; mode++) {
            qtest_writel(q, DMA + 0x360, 1);
            uint32_t ctrl = 5 | width << 3 | width << 5;
            if (mode == 0) { ctrl |= 1u << 8; } /* Source descending. */
            if (mode == 1) { ctrl |= 2u << 8; } /* Source fixed. */
            if (mode == 2) { ctrl |= 2u << 10; } /* Destination fixed. */
            qtest_writel(q, DMA, ctrl);
            qtest_memwrite(q, RAM + 0x1000, guard, sizeof(guard));
            descriptor(q, 0, mode == 0 ? RAM + 32 : RAM, RAM + 0x1000, 16);
            qtest_clock_step(q, 100000);
            memcpy(expected, guard, sizeof(guard));
            for (unsigned i = 0; i < 16; i += unit) {
                unsigned src = mode == 0 ? 32 - i : mode == 1 ? 0 : i;
                unsigned dst = mode == 2 ? 0 : i;
                memcpy(expected + dst, source + src, unit);
            }
            qtest_memread(q, RAM + 0x1000, actual, sizeof(actual));
            g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
        }
    }
    qtest_quit(q);
}
static void psram_alias(void)
{
    QTestState *q = start();
    qtest_writel(q, 0x38000020, 0x11223344);
    qtest_writel(q, DMA, CTRL_COPY);
    descriptor(q, 0, 0x38800020, RAM, 4);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0x11223344);
    descriptor(q, 0, RAM, 0x38800030, 4);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, 0x38000030), ==, 0x11223344);
    qtest_quit(q);
}
static void priority_burst_order(void)
{
    QTestState *q = start();
    qtest_writel(q, RAM, 0xfeed0123);
    qtest_writel(q, DMA, CTRL_COPY | (3u << 19));
    qtest_writel(q, DMA + 4, CTRL_COPY);
    descriptor(q, 0, RAM + 0x100, RAM + 0x200, 4);
    descriptor(q, 1, RAM, RAM + 0x100, 4);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, RAM + 0x100), ==, 0xfeed0123);
    g_assert_cmphex(qtest_readl(q, RAM + 0x200), ==, 0xfeed0123);
    qtest_quit(q);
}
static void rgb_format_capacity(void)
{
    QTestState *q = start();
    const unsigned words[] = {24, 32, 16};
    for (unsigned format = 0; format < 3; format++) {
        qtest_writel(q, RGB + 4, 0x30000 | (format << 4));
        qtest_writel(q, RGB + 0x1c, 63);
        for (unsigned i = 0; i < words[format] - 1; i++) {
            qtest_writel(q, RGB + 0xf4, 0);
        }
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x28, ==, 0);
        qtest_writel(q, RGB + 0xf4, 0);
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x28, ==, 0x28);
        qtest_writel(q, RGB + 8, 0x112233);
        qtest_writel(q, RGB + 16, 0x00030004);
        qtest_writel(q, RGB + 4, (format << 4) | 1);
        g_assert_cmphex(qtest_readl(q, RGB + 4), ==, format << 4);
        g_assert_cmphex(qtest_readl(q, RGB + 8), ==, 0x112233);
        g_assert_cmphex(qtest_readl(q, RGB + 16), ==, 0x00030004);
        g_assert_cmphex(qtest_readl(q, RGB + 0x24) & 0x28, ==, 0x28);
    }
    qtest_quit(q);
}
typedef struct Rejected { const char *name; uint32_t address, value; } Rejected;
static const Rejected rejected[] = {
    { "image-mode", DMA + 0x100, 1 },
    { "outside-memory", DMA + 0x200, 1 },
    { "command-fifo", DMA + 0x4c4, 16 },
    { "read-only-status", DMA + 0x280, 1 },
    { "reserved-register", DMA + 0x18, 0 },
    { "serial-rgb", RGB, 0x101 },
    { "pending-shadow", DMA + 0xa0, RAM + 64 },
};
static void reject(const void *opaque)
{
    const Rejected *r = opaque;
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        if (r->address == RGB) {
            qtest_writel(q, RGB + 4, 0x420);
            qtest_writel(q, RGB + 16, 0x00020004);
        }
        if (r->address == DMA + 0xa0) {
            qtest_writel(q, DMA, CTRL_COPY);
            descriptor(q, 0, RAM, RAM + 0x1000, 32);
            descriptor(q, 0, RAM, RAM + 0x1020, 32);
        }
        if (r->address == DMA + 0x200) {
            qtest_writel(q, DMA, CTRL_COPY);
            qtest_writel(q, DMA + 0xa0, 0x28000000);
            qtest_writel(q, DMA + 0xc0, RAM);
            qtest_writel(q, DMA + 0xe0, 4);
        }
        qtest_writel(q, r->address, r->value);
        if (r->address == DMA + 0x200) { qtest_clock_step(q, 100000); }
        qtest_quit(q); return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-image-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0); g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0); close(fd);
    fd = g_file_open_tmp("venusa-rgb-capture-XXXXXX", &capture_path, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_setenv("LISEM_QEMU_RGB_CAPTURE", capture_path, true);
    qtest_add_func("venusa/dma2d/normal-shadow", normal_shadow);
    qtest_add_func("venusa/dma2d/clock-cancel-reset", cancel_gate);
    qtest_add_func("venusa/dma2d/shadow-irq-byte-lanes", shadow_irq_and_byte_lanes);
    qtest_add_func("venusa/dma2d/narrow-address-modes", narrow_and_address_modes);
    qtest_add_func("venusa/dma2d/priority-burst-order", priority_burst_order);
    qtest_add_func("venusa/dma2d/psram-alias", psram_alias);
    qtest_add_func("venusa/rgb/format-capacity-soft-reset", rgb_format_capacity);
    qtest_add_func("venusa/rgb/actual-pixels", rgb_pixels);
    qtest_add_func("venusa/rgb/starvation-no-frame", starvation_no_frame);
    qtest_add_func("venusa/rgb/clock-phase", rgb_clock_boundaries);
    qtest_add_func("venusa/rgb/pixel-deadline", rgb_pixel_deadline);
    qtest_add_func("venusa/rgb/frame-period", rgb_frame_period);
    qtest_add_func("venusa/rgb/fifo-latches", rgb_fifo_latches);
    qtest_add_func("venusa/rgb/packed-formats", rgb_packed_formats);
    for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
        g_autofree char *name = g_strdup_printf("venusa/image/reject/%s", rejected[i].name);
        qtest_add_data_func(name, &rejected[i], reject);
    }
    int result = g_test_run();
    unlink(flash); unlink(capture_path); g_free(flash); g_free(capture_path);
    return result;
}
