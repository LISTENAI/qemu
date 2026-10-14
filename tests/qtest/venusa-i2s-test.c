/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "listenai-test-utils.h"
#include "qemu/bswap.h"
#define APC 0x44700000
#define CLK 0x43100034
static char *flash, *source, *capture;
static QTestState *machine(const char *peer)
{
    g_autofree char *drive =
        g_strdup_printf("if=pflash,format=raw,file=%s", flash);
    g_autofree char *quoted_drive = listenai_test_quote_arg(drive);
    g_autofree char *quoted_peer = listenai_test_quote_arg(peer ? peer : "");
    return qtest_initf("-machine venusa-rd-evb -drive %s %s %s",
                       quoted_drive, peer ? "-device" : "",
                       peer ? quoted_peer : "");
}

static QTestState *start(bool peer_master, bool attached)
{
    g_autofree char *peer =
        attached
            ? g_strdup_printf(
                  "listenai-i2s-peer,gpio=/machine/soc,master=%s,rate=16000,"
                  "frames=8,start-ns=%u,input-s32le=%s,output-s32le=%s",
                  peer_master ? "on" : "off", peer_master ? 1000000 : 0, source,
                  capture)
            : g_strdup("");
    QTestState *q = machine(attached ? peer : NULL);
    qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x8400);
    qtest_writel(q, APC, 3);
    for (unsigned i = 4; i <= 7; i++) {
        qtest_writel(q, 0x44100000 + 4 * i, 9);
    }
    qtest_writel(q, APC + 0x10, 0x02010001);
    qtest_writel(q, APC + 0x1c, 0x02010001);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(q, APC + 0x38,
                     (uint16_t)(0x1230 + i) |
                         ((uint32_t)(uint16_t)(-0x3210 - i) << 16));
    }
    if (!peer_master) {
        qtest_writel(q, APC + 0x28, (48u << 16) | (1u << 26));
    }
    qtest_writel(q, APC + 0x24, peer_master ? 0x80093000 : 0x80303000);
    return q;
}
static void check_capture(void)
{
    char *bytes = NULL;
    gsize length;
    g_assert_true(g_file_get_contents(capture, &bytes, &length, NULL));
    g_assert_cmpuint(length, ==, 64);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmpint((int32_t)ldl_le_p(bytes + 8 * i), ==, 0x1230 + i);
        g_assert_cmpint((int32_t)ldl_le_p(bytes + 8 * i + 4), ==,
                        -0x3210 - (int)i);
    }
    g_free(bytes);
}
static void transfer(bool peer_master)
{
    QTestState *q = start(peer_master, true);
    qtest_clock_step(q, peer_master ? 2000000 : 514000);
    if (!peer_master) {
        qtest_writel(q, APC + 0x24, 0x00303000);
    }
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0x800080);
    for (unsigned i = 0; i < 8; i++) {
        uint32_t expected =
            (uint16_t)(-0x2340 - i) | ((uint32_t)(0x4320 + i) << 16);
        g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, expected);
    }
    listenai_test_quit(q);
    check_capture();
}
static void slave(void)
{
    transfer(true);
}
static void master(void)
{
    transfer(false);
}
static void absent(void)
{
    QTestState *q = start(true, false);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0x800080);
    qtest_quit(q);
}
static void wrong_mux(void)
{
    QTestState *q = start(true, true);
    qtest_writel(q, 0x44100010, 0);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0x800080);
    qtest_quit(q);
}
static void boundary(void)
{
    QTestState *q = start(true, true);
    qtest_clock_step(q, 1063476);
    /* The right word's last bit has not arrived. */
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0x100010);
    g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0x4320dcc0);
    qtest_quit(q);
}
static void clock_gate(void)
{
    QTestState *q = start(true, true);
    qtest_writel(q, CLK, qtest_readl(q, CLK) & ~0x8000u);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0x800080);
    qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x8000);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    qtest_quit(q);
}
static void reset_partial(void)
{
    QTestState *q = start(true, true);
    qtest_clock_step(q, 1010000);
    qtest_writel(q, 0x43100014, 1u << 20);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    qtest_quit(q);
}
static void system_reset_active(const void *opaque)
{
    bool peer_master = GPOINTER_TO_UINT(opaque);
    QTestState *q = start(peer_master, true);
    uint32_t base = 0x40000000, dst = 0x20010000;
    qtest_writel(q, 0x431000a0, 1u << 14);
    qtest_writel(q, dst, 0xdeadbeef);
    qtest_writel(q, base, APC + 0x50);
    qtest_writel(q, base + 8, dst);
    qtest_writel(q, base + 0x18, 0x00200425);
    qtest_writel(q, base + 0x1c, 1);
    qtest_writel(q, base + 0x44, 14u << 7);
    qtest_writel(q, base + 0x398, 1);
    qtest_writel(q, base + 0x3a0, 0x101);
    qtest_clock_step(q, peer_master ? 1063476 : 64999);
    qtest_system_reset(q);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, dst), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(q, base + 0x2c0), ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    qtest_quit(q);
}

static void dma(void)
{
    QTestState *q = start(true, true);
    uint32_t base = 0x40000000, dst = 0x20010000;
    qtest_writel(q, 0x431000a0, 1u << 14);
    for (unsigned i = 0; i < 10; i++) {
        qtest_writel(q, dst + 4 * i, 0xdeadbeef);
    }
    qtest_writel(q, base, APC + 0x50);
    qtest_writel(q, base + 8, dst + 4);
    qtest_writel(q, base + 0x18, 0x00200425);
    qtest_writel(q, base + 0x1c, 8);
    qtest_writel(q, base + 0x44, 14u << 7);
    qtest_writel(q, base + 0x398, 1);
    qtest_writel(q, base + 0x3a0, 0x101);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, base + 0x2c0), ==, 1);
    g_assert_cmphex(qtest_readl(q, base + 0x3a0), ==, 0);
    g_assert_cmphex(qtest_readl(q, dst), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(q, dst + 36), ==, 0xdeadbeef);
    for (unsigned i = 0; i < 8; i++) {
        uint32_t expected =
            (uint16_t)(-0x2340 - i) | ((uint32_t)(0x4320 + i) << 16);
        g_assert_cmphex(qtest_readl(q, dst + 4 + 4 * i), ==, expected);
    }
    qtest_quit(q);
}
static void wide(const void *opaque)
{
    unsigned width = GPOINTER_TO_UINT(opaque);
    unsigned code = width == 20 ? 1 : width == 24 ? 2 : 3;
    g_autofree char *peer = g_strdup_printf(
        "listenai-i2s-peer,gpio=/machine/soc,width=%u,frames=8,"
        "start-ns=1000000,input-s32le=%s,output-s32le=%s",
        width, source, capture);
    QTestState *q = machine(peer);
    qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x8400);
    qtest_writel(q, APC, 3);
    for (unsigned i = 4; i <= 7; i++) {
        qtest_writel(q, 0x44100000 + 4 * i, 9);
    }
    qtest_writel(q, APC + 0x10, 0x02050005);
    qtest_writel(q, APC + 0x1c, 0x02050005);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(q, APC + 0x38, 0x12345678 + i);
        qtest_writel(q, APC + 0x38, -0x4321 - i);
    }
    qtest_writel(q, APC + 0x24, 0x80093000 | code << 10);
    qtest_clock_step(q, 2000000);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(q, APC + 0x50), ==,
                        (uint32_t)(-0x2340 - i));
        g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0x4320 + i);
    }
    listenai_test_quit(q);
    char *bytes = NULL;
    gsize length;
    g_assert_true(g_file_get_contents(capture, &bytes, &length, NULL));
    g_assert_cmpuint(length, ==, 64);
    for (unsigned i = 0; i < 8; i++) {
        int32_t expected =
            (int32_t)((0x12345678 + i) << (32 - width)) >> (32 - width);
        g_assert_cmpint((int32_t)ldl_le_p(bytes + 8 * i), ==, expected);
        g_assert_cmpint((int32_t)ldl_le_p(bytes + 8 * i + 4), ==,
                        -0x4321 - (int)i);
    }
    g_free(bytes);
}
static void absent_data(void)
{
    QTestState *q = machine("listenai-i2s-peer,gpio=/machine/soc,"
                            "frames=8,start-ns=1000000");
    qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x8400);
    qtest_writel(q, APC, 3);
    for (unsigned i = 4; i <= 7; i++) {
        qtest_writel(q, 0x44100000 + 4 * i, 9);
    }
    qtest_writel(q, APC + 0x1c, 0x02010001);
    qtest_writel(q, APC + 0x24, 0x80093000);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    qtest_quit(q);
}
static void reject_mode(const void *opaque)
{
    if (g_test_subprocess()) {
        QTestState *q = start(true, false);
        qtest_set_expected_status(q, 1);
        qtest_writel(q, APC + 0x24, 0x80093000 | GPOINTER_TO_UINT(opaque));
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*address=0x44700024*");
}
static void reject_extended(const void *opaque)
{
    if (g_test_subprocess()) {
        QTestState *q = start(true, false);
        qtest_set_expected_status(q, 1);
        qtest_writel(q, APC + 0x28, (48u << 16) | (1u << 26) |
                                     GPOINTER_TO_UINT(opaque));
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*address=0x44700028*");
}

static void measured_fifo_phase(void)
{
    static const unsigned widths[] = { 16, 24, 32, 16, 16 };
    static const unsigned pads[] = { 0, 0, 0, 9, 0 };
    static const unsigned divisors[] = { 120, 120, 120, 120, 48 };
    for (unsigned c = 0; c < ARRAY_SIZE(widths); c++) {
        unsigned width = widths[c], slot = width + pads[c];
        unsigned code = width == 24 ? 2 : width == 32 ? 3 : 0;
        unsigned format = width == 24 ? 1 : width == 32 ? 2 : 0;
        uint64_t bit_ns = divisors[c] * UINT64_C(1000000000) / 24000000;
        uint64_t tx_ns = (8 + slot + 1) * bit_ns;
        uint64_t rx_ns = (8 + slot + width) * bit_ns;
        QTestState *q = machine(NULL);
        qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x8400);
        qtest_writel(q, APC, 3);
        qtest_writel(q, APC + 0x10, 0x02010001 | format << 1 | format << 17);
        qtest_writel(q, APC + 0x1c, 0x02010001 | format << 1 | format << 17);
        qtest_writel(q, APC + 0x38, format ? 0x693cc3 : 0x69a53cc3);
        if (format) {
            qtest_writel(q, APC + 0x38, 0x5a66a5);
        }
        qtest_writel(q, APC + 0x28, divisors[c] << 16 | 1u << 26);
        qtest_writel(q, APC + 0x24, 0x88303004 | code << 10 | pads[c] << 5);
        qtest_clock_step(q, tx_ns - 1);
        g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0x100010);
        qtest_clock_step(q, 1);
        g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0);
        qtest_clock_step(q, rx_ns - tx_ns - 1);
        g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
        qtest_clock_step(q, 1);
        g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0x100010);
        g_assert_cmphex(qtest_readl(q, APC + 0x50), ==,
                       format ? 0x693cc3 : 0x69a53cc3);
        if (format) {
            g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0x5a66a5);
        }
        qtest_clock_step(q, 2 * slot * bit_ns - 1);
        g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
        qtest_clock_step(q, 1);
        g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0x100010);
        qtest_quit(q);
    }
}

static void stop_at_fifo_empty(void)
{
    for (unsigned stop = 0; stop < 2; stop++) {
        QTestState *q = machine(NULL);
        qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x8400);
        qtest_writel(q, APC, 3);
        qtest_writel(q, APC + 0x10, 0x02010001);
        qtest_writel(q, APC + 0x1c, 0x02010001);
        qtest_writel(q, APC + 0x38, 0x69a53cc3);
        qtest_writel(q, APC + 0x28, (120u << 16) | (1u << 26));
        qtest_writel(q, APC + 0x24, 0x88303004);
        qtest_clock_step(q, 125000);
        g_assert_cmphex(qtest_readl(q, APC + 0x10), ==, 0x02010001);
        g_assert_cmphex(qtest_readl(q, APC + 0x1c), ==, 0x02010001);
        if (stop) {
            qtest_writel(q, APC + 0x24, 0x08303004);
        }
        qtest_clock_step(q, 375000);
        g_assert_cmphex(qtest_readl(q, APC + 0x1c), ==,
                       stop ? 0x02010001 : 0x02210021);
        g_assert_cmphex(qtest_readl(q, APC + 0x94), ==, stop ? 0x411 : 0x415);
        qtest_quit(q);
    }
}

static void peer_word(QTestState *q, bool right, uint16_t word)
{
    uint32_t levels = 0x10 | (right ? 0x20 : 0);
    qtest_writel(q, 0x43600024, levels);
    for (int bit = 16; bit >= 0; bit--) {
        uint32_t sample = bit < 16 && ((word >> bit) & 1) ? 0x80 : 0;
        qtest_writel(q, 0x43600024, levels | sample);
        qtest_writel(q, 0x43600024, (levels & ~0x10u) | sample);
        qtest_writel(q, 0x43600024, levels | sample);
    }
}
static void incomplete_frame(void)
{
    g_autofree char *peer = g_strdup_printf(
        "listenai-i2s-peer,gpio=/machine/soc,master=off,frames=1,"
        "output-s32le=%s", capture);
    QTestState *q = machine(peer);
    for (unsigned pin = 4; pin <= 7; pin++) {
        qtest_writel(q, 0x44100000 + 4 * pin, 0);
    }
    qtest_writel(q, 0x43600028, 0xb0);
    qtest_writel(q, 0x43600024, 0x10);
    peer_word(q, true, 0x7788); /* No left word: cannot form a stereo frame. */
    peer_word(q, false, 0x1234);
    peer_word(q, true, (uint16_t)-0x4321);
    listenai_test_quit(q);
    char *bytes = NULL;
    gsize length;
    g_assert_true(g_file_get_contents(capture, &bytes, &length, NULL));
    g_assert_cmpuint(length, ==, 8);
    g_assert_cmpint((int32_t)ldl_le_p(bytes), ==, 0x1234);
    g_assert_cmpint((int32_t)ldl_le_p(bytes + 4), ==, -0x4321);
    g_free(bytes);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-i2s-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    {
        gsize size = 16 * 1024 * 1024;
        g_autofree char *image = g_malloc0(size);

        g_assert_true(g_file_set_contents(flash, image, size, NULL));
    }
    fd = g_file_open_tmp("venusa-i2s source-XXXXXX", &source, NULL);
    g_assert_cmpint(fd, >=, 0);
    uint8_t pcm[64];
    for (unsigned i = 0; i < 8; i++) {
        stl_le_p(pcm + 8 * i, -0x2340 - i);
        stl_le_p(pcm + 8 * i + 4, 0x4320 + i);
    }
    g_assert_cmpint(write(fd, pcm, sizeof(pcm)), ==, sizeof(pcm));
    close(fd);
    fd = g_file_open_tmp("venusa-i2s capture-XXXXXX", &capture, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qtest_add_func("venusa/i2s/external-master", slave);
    qtest_add_func("venusa/i2s/external-slave", master);
    qtest_add_func("venusa/i2s/absent-peer", absent);
    qtest_add_func("venusa/i2s/wrong-pinmux", wrong_mux);
    qtest_add_func("venusa/i2s/last-bit-boundary", boundary);
    qtest_add_func("venusa/i2s/clock-gate", clock_gate);
    qtest_add_func("venusa/i2s/reset-partial-word", reset_partial);
    qtest_add_data_func("venusa/i2s/system-reset-master",
                        GUINT_TO_POINTER(0), system_reset_active);
    qtest_add_data_func("venusa/i2s/system-reset-slave",
                        GUINT_TO_POINTER(1), system_reset_active);
    qtest_add_func("venusa/i2s/receive-dma", dma);
    qtest_add_data_func("venusa/i2s/20-bit", GUINT_TO_POINTER(20), wide);
    qtest_add_data_func("venusa/i2s/24-bit", GUINT_TO_POINTER(24), wide);
    qtest_add_data_func("venusa/i2s/32-bit", GUINT_TO_POINTER(32), wide);
    qtest_add_func("venusa/i2s/absent-data", absent_data);
    qtest_add_data_func("venusa/i2s/reject/right-justified",
                        GUINT_TO_POINTER(16), reject_mode);
    qtest_add_data_func("venusa/i2s/reject/lsb-first", GUINT_TO_POINTER(8),
                        reject_mode);
    qtest_add_data_func("venusa/i2s/reject/clock-gate-mode",
                        GUINT_TO_POINTER(0x4000), reject_mode);
    qtest_add_func("venusa/i2s/partial-frame-capture", incomplete_frame);
    qtest_add_func("venusa/i2s/measured-fifo-phase", measured_fifo_phase);
    qtest_add_data_func("venusa/i2s/reject/same-edge",
                       GUINT_TO_POINTER(1u << 27), reject_extended);
    qtest_add_data_func("venusa/i2s/reject/tdm-slots",
                       GUINT_TO_POINTER(1), reject_extended);
    qtest_add_data_func("venusa/i2s/reject/long-sync",
                       GUINT_TO_POINTER(0x100), reject_extended);
    qtest_add_func("venusa/i2s/stop-at-fifo-empty", stop_at_fifo_empty);
    int result = g_test_run();
    unlink(flash);
    unlink(source);
    unlink(capture);
    g_free(flash);
    g_free(source);
    g_free(capture);
    return result;
}
