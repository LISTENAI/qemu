/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "listenai-test-utils.h"
#include "qemu/bswap.h"

#define APC 0x45b00000
#define AP 0x45800000
#define DMA 0x45900000
static char *source, *capture;
static unsigned peer_start_ns;
static void pin(QTestState *q, unsigned n, bool value);
static void word(QTestState *q, bool right, uint16_t value, unsigned bits);

static QTestState *start(unsigned port, bool master, unsigned width, bool peer)
{
    g_autofree char *arg =
        peer ? g_strdup_printf(
                   "listenai-i2s-peer,gpio=/machine/soc,master=%s,"
                   "bclk-pin=12,ws-pin=13,input-pin=14,output-pin=15,"
                   "rate=16000,width=%u,frames=8,start-ns=%u,"
                   "input-s32le=%s,output-s32le=%s",
                   master ? "off" : "on", width,
                   master ? peer_start_ns : 1000000, source, capture)
             : g_strdup("");
    g_autofree char *quoted = listenai_test_quote_arg(arg);
    QTestState *q = qtest_initf("-machine arcs-evb %s %s",
                                peer ? "-device" : "", peer ? quoted : "");
    qtest_writel(q, AP + 8, 0x10400);
    qtest_writel(q, APC, 1);
    for (unsigned p = 12; p <= 15; p++) {
        qtest_writel(q, 0x47500000 + p * 4, 9 + port);
    }
    unsigned format = width == 16 ? 0 : width == 32 ? 2 : 1;
    uint32_t fifo = 0x02010001 | format * 0x20002;
    qtest_writel(q, APC + 12 + port * 4, fifo | (port ? 0 : 0x10000000));
    qtest_writel(q, APC + 20 + port * 4, fifo | (port ? 0 : 0x10000000));
    for (unsigned i = 0; i < 8; i++) {
        if (width == 16) {
            qtest_writel(q, APC + 0xf4 + port * 8,
                         (0x1230 + i) |
                             ((uint32_t)(uint16_t)(-0x3210 - i) << 16));
        } else {
            qtest_writel(q, APC + 0xf4 + port * 8, 0x1230 + i);
            qtest_writel(q, APC + 0xf4 + port * 8, -0x3210 - i);
        }
    }
    unsigned code = width == 16 ? 0 : width == 20 ? 1 : width == 24 ? 2 : 3;
    qtest_writel(q, APC + 0xe8 + port * 8, (48u << 16) | (1u << 26));
    qtest_writel(q, APC + 0xe4 + port * 8,
                 (master ? 0x80303000 : 0x80093000) | (code << 10));
    return q;
}

static void check_capture(void)
{
    g_autofree char *data = NULL;
    gsize size;
    g_assert_true(g_file_get_contents(capture, &data, &size, NULL));
    g_assert_cmpuint(size, ==, 64);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmpint((int32_t)ldl_le_p(data + 8 * i), ==, 0x1230 + i);
        g_assert_cmpint((int32_t)ldl_le_p(data + 8 * i + 4), ==,
                        -0x3210 - (int)i);
    }
}

static void transfer(const void *opaque)
{
    unsigned flags = GPOINTER_TO_UINT(opaque), port = flags & 1;
    bool master = flags & 2;
    unsigned width = flags >> 8;
    QTestState *q = start(port, master, width, true);
    qtest_clock_step(q, master ? width * 32000 + 2000 : 2200000);
    qtest_writel(q, APC + 0xe4 + port * 8, 0x00202000);
    g_assert_cmphex(qtest_readl(q, APC + 20 + port * 4) & 0x1f001f0, ==,
                    0x800080);
    for (unsigned i = 0; i < 8; i++) {
        if (width == 16) {
            g_assert_cmphex(qtest_readl(q, APC + 0x104 + port * 8), ==,
                            (uint16_t)(-0x2340 - i) |
                                ((uint32_t)(0x4320 + i) << 16));
        } else {
            uint32_t mask = width == 32 ? UINT32_MAX : 0xffffff;
            g_assert_cmphex(qtest_readl(q, APC + 0x104 + port * 8), ==,
                            (-0x2340 - i) & mask);
            g_assert_cmphex(qtest_readl(q, APC + 0x104 + port * 8), ==,
                            0x4320 + i);
        }
    }
    listenai_test_quit(q);
    check_capture();
}

static void late_peer(const void *opaque)
{
    peer_start_ns = GPOINTER_TO_UINT(opaque);
    QTestState *q = start(0, true, 16, true);
    qtest_clock_step(q, 514000);
    qtest_writel(q, APC + 0xe4, 0x00303000);
    listenai_test_quit(q);
    peer_start_ns = 0;
    g_autofree char *data = NULL;
    gsize size;
    g_assert_true(g_file_get_contents(capture, &data, &size, NULL));
    /* Joining after the first falling edge discards the incomplete frame. */
    g_assert_cmpuint(size, ==, 7 * 8);
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmpint((int32_t)ldl_le_p(data + i * 8), ==, 0x1231 + i);
        g_assert_cmpint((int32_t)ldl_le_p(data + i * 8 + 4), ==,
                        -0x3211 - (int)i);
    }
}

static void measured_divisor(const void *opaque)
{
    unsigned divisor = GPOINTER_TO_UINT(opaque);
    for (unsigned port = 0; port < 2; port++) {
        QTestState *q = start(port, false, 16, false);
        qtest_writel(q, APC + 0xe4 + port * 8, 0x00202000);
        qtest_writel(q, APC + 0xe8 + port * 8,
                     divisor << 16 | 1u << 26);
        g_assert_cmphex(qtest_readl(q, APC + 0xe8 + port * 8), ==,
                        divisor << 16);
        qtest_writel(q, APC + 0xe4 + port * 8, 0x88303004);
        /* Internal loopback: eight priming BCLKs, then 32 clocks/frame. */
        uint64_t first = (40 * UINT64_C(1000000000) * divisor + 23999999) /
                         24000000;
        qtest_clock_step(q, first - 1);
        g_assert_cmphex(qtest_readl(q, APC + 20 + port * 4) & 0x1f001f0,
                        ==, 0);
        qtest_clock_step(q, 1);
        g_assert_cmphex(qtest_readl(q, APC + 20 + port * 4) & 0x1f001f0,
                        ==, 0x100010);
        g_assert_cmphex(qtest_readl(q, APC + 0x104 + port * 8), ==,
                        (uint16_t)0x1230 | (uint32_t)(uint16_t)-0x3210 << 16);
        uint64_t second = (72 * UINT64_C(1000000000) * divisor + 23999999) /
                          24000000;
        qtest_clock_step(q, second - first - 1);
        g_assert_cmphex(qtest_readl(q, APC + 20 + port * 4) & 0x1f001f0,
                        ==, 0);
        qtest_clock_step(q, 1);
        g_assert_cmphex(qtest_readl(q, APC + 20 + port * 4) & 0x1f001f0,
                        ==, 0x100010);
        qtest_quit(q);
    }
}

static void output_release(void)
{
    QTestState *q = start(0, true, 16, false);
    qtest_irq_intercept_out_named(q, "/machine/soc", "pad-out");
    pin(q, 12, false);
    pin(q, 13, false);
    qtest_clock_step(q, 33000);
    g_assert_true(qtest_get_irq(q, 12));
    g_assert_true(qtest_get_irq(q, 13));
    qtest_writel(q, APC + 0xe4, 0x00303000);
    g_assert_false(qtest_get_irq(q, 12));
    g_assert_false(qtest_get_irq(q, 13));
    qtest_writel(q, APC + 0xe4, 0x80013000);
    word(q, false, 0x1234, 16);
    word(q, true, 0x5678, 16);
    pin(q, 12, false);
    pin(q, 13, false);
    g_assert_false(qtest_get_irq(q, 12));
    g_assert_false(qtest_get_irq(q, 13));
    g_assert_cmphex(qtest_readl(q, APC + 0x104), ==, 0x56781234);
    qtest_writel(q, AP, 0x20);
    g_assert_false(qtest_get_irq(q, 12));
    g_assert_false(qtest_get_irq(q, 13));
    qtest_quit(q);
}

static void system_reset_active(const void *opaque)
{
    bool master = GPOINTER_TO_UINT(opaque);
    QTestState *q = start(0, master, 16, true);
    qtest_writel(q, 0x28000100, 0xdeadbeef);
    qtest_writel(q, DMA + 0x28, 1);
    qtest_writel(q, DMA + 0x54, APC + 0x104);
    qtest_writel(q, DMA + 0x5c, 0x28000100);
    qtest_writel(q, DMA + 0x2c, 1);
    qtest_writel(q, DMA, 0xb0000283);
    qtest_clock_step(q, master ? 64999 : 1063476);
    qtest_system_reset(q);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, 0x28000100), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(q, DMA + 0x158), ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0);
    qtest_quit(q);
}

static void absent(void)
{
    QTestState *q = start(0, false, 16, false);
    qtest_clock_step(q, 3000000);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 12) & 0x1f001f0, ==, 0x800080);
    qtest_quit(q);
}

static void boundary(void)
{
    QTestState *q = start(0, false, 16, true);
    qtest_clock_step(q, 1063476);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0x100010);
    g_assert_cmphex(qtest_readl(q, APC + 0x104), ==, 0x4320dcc0);
    qtest_quit(q);
}

static void reset(void)
{
    QTestState *q = start(0, false, 16, true);
    qtest_clock_step(q, 1040000);
    qtest_writel(q, AP, 0x20);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, APC + 0xe4), ==, 0x00202000);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0);
    qtest_quit(q);
}

static void gated(void)
{
    QTestState *q = start(0, false, 16, true);
    qtest_writel(q, AP + 8, 0x10000);
    qtest_clock_step(q, 3000000);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0);
    qtest_quit(q);
}

static void dma_receive(void)
{
    QTestState *q = start(0, false, 16, true);
    /*
     * Mixed stereo RX0 request 11, word peripheral->memory, eight real words.
     */
    qtest_writel(q, 0x280000fc, 0xa55a1234);
    qtest_writel(q, 0x28000120, 0xa55a5678);
    qtest_writel(q, DMA + 0x28, 1);
    qtest_writel(q, DMA + 0x54, APC + 0x104);
    qtest_writel(q, DMA + 0x5c, 0x28000100);
    qtest_writel(q, DMA + 0x2c, 8);
    qtest_writel(q, DMA, 0xb0000283);
    qtest_clock_step(q, 2000000);
    g_assert_cmphex(qtest_readl(q, DMA + 0x158) & 1, ==, 1);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(q, 0x28000100 + 4 * i), ==,
                        (uint16_t)(-0x2340 - i) |
                            ((uint32_t)(0x4320 + i) << 16));
    }
    g_assert_cmphex(qtest_readl(q, 0x280000fc), ==, 0xa55a1234);
    g_assert_cmphex(qtest_readl(q, 0x28000120), ==, 0xa55a5678);
    qtest_writel(q, DMA + 0x154, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x158) & 1, ==, 0);
    qtest_quit(q);
}

static void pin(QTestState *q, unsigned n, bool value)
{
    qtest_set_irq_in(q, "/machine/soc", "pad-in", n, value);
}

static void word(QTestState *q, bool right, uint16_t value, unsigned bits)
{
    pin(q, 13, right);
    for (unsigned i = bits; i > 0; i--) {
        pin(q, 12, false);
        pin(q, 14, (value >> (i - 1)) & 1);
        pin(q, 12, true);
    }
}

static void reset_wire(const void *opaque)
{
    bool right = GPOINTER_TO_UINT(opaque);
    QTestState *q = start(0, false, 16, false);
    qtest_writel(q, APC + 0xe4, 0x00013000);
    qtest_writel(q, APC + 0xe4, 0x80013000); /* aligned RX for manual edges */
    word(q, right, 0xa5, 8);
    qtest_writel(q, AP, 0x20);
    qtest_writel(q, APC + 0xe4, 0x80013000);
    /*
     * The wire remains at the same WS level through reset. Neither the
     * discarded first half nor its remaining bits may complete a frame.
     */
    word(q, right, 0x5a, 8);
    if (!right) {
        word(q, true, 0x7788, 16);
    }
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0);
    word(q, false, 0x1234, 16);
    word(q, true, 0xabcd, 16);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0x100010);
    g_assert_cmphex(qtest_readl(q, APC + 0x104), ==, 0xabcd1234);
    qtest_quit(q);
}

static void master_pause(void)
{
    QTestState *q = start(0, true, 16, true);
    qtest_clock_step(q, 10500);
    qtest_writel(q, AP + 8, 0x10000);
    unsigned tx = qtest_readl(q, APC + 12), rx = qtest_readl(q, APC + 20);
    qtest_clock_step(q, 100000000);
    g_assert_cmphex(qtest_readl(q, APC + 12), ==, tx);
    g_assert_cmphex(qtest_readl(q, APC + 20), ==, rx);
    qtest_writel(q, AP + 8, 0x10400);
    qtest_clock_step(q, 503500);
    qtest_writel(q, APC + 0xe4, 0x00303000);
    g_assert_cmphex(qtest_readl(q, APC + 20) & 0x1f001f0, ==, 0x800080);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(q, APC + 0x104), ==,
                        (uint16_t)(-0x2340 - i) |
                            ((uint32_t)(0x4320 + i) << 16));
    }
    listenai_test_quit(q);
    check_capture();
}

static void reject_change(const void *opaque)
{
    unsigned which = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start(0, false, 16, true);
        qtest_clock_step(q, 1010000);
        qtest_set_expected_status(q, 1);
        if (which == 0) {
            qtest_writel(q, APC + 0xe4, 0x80093400);
        }
        if (which == 1) {
            qtest_writel(q, APC + 0xe8, (3u << 16) | (1u << 26));
        }
        if (which == 2) {
            qtest_writel(q, AP + 8, 0x10000);
        }
        if (which == 3) {
            qtest_writel(q, 0x47500038, 0);
        }
        if (which == 4) {
            qtest_writel(q, APC + 0x0c, 0x02010001);
        }
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*ARCS unsupported write*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_setenv("ARCS_QEMU_BUDGET_NS", "1000000000", true);
    int fd = g_file_open_tmp("arcs-i2s source-XXXXXX", &source, NULL);
    g_assert_cmpint(fd, >=, 0);
    uint8_t data[64];
    for (unsigned i = 0; i < 8; i++) {
        stl_le_p(data + i * 8, -0x2340 - i);
        stl_le_p(data + i * 8 + 4, 0x4320 + i);
    }
    g_assert_cmpint(write(fd, data, sizeof(data)), ==, sizeof(data));
    close(fd);
    fd = g_file_open_tmp("arcs-i2s capture-XXXXXX", &capture, NULL);
    close(fd);
    static const unsigned widths[] = {16, 20, 24, 32};
    for (unsigned port = 0; port < 2; port++) {
        for (unsigned master = 0; master < 2; master++) {
            for (unsigned i = 0; i < ARRAY_SIZE(widths); i++) {
                g_autofree char *name =
                    g_strdup_printf("arcs/i2s/port%u/%s/%ubit", port,
                                    master ? "master" : "slave", widths[i]);
                qtest_add_data_func(
                    name, GUINT_TO_POINTER(port | master * 2 | widths[i] << 8),
                    transfer);
            }
        }
    }
    qtest_add_data_func("arcs/i2s/peer-start-1ns", GUINT_TO_POINTER(1),
                        late_peer);
    qtest_add_data_func("arcs/i2s/peer-joins-running", GUINT_TO_POINTER(10001),
                        late_peer);
    static const unsigned divisors[] = { 3, 15, 127 };
    for (unsigned i = 0; i < ARRAY_SIZE(divisors); i++) {
        g_autofree char *name =
            g_strdup_printf("arcs/i2s/measured-divisor/%u", divisors[i]);
        qtest_add_data_func(name, GUINT_TO_POINTER(divisors[i]),
                            measured_divisor);
    }
    qtest_add_data_func("arcs/i2s/system-reset-master",
                        GUINT_TO_POINTER(1), system_reset_active);
    qtest_add_data_func("arcs/i2s/system-reset-slave",
                        GUINT_TO_POINTER(0), system_reset_active);
    qtest_add_func("arcs/i2s/output-release", output_release);
    qtest_add_func("arcs/i2s/absent-peer", absent);
    qtest_add_func("arcs/i2s/last-bit-boundary", boundary);
    qtest_add_func("arcs/i2s/reset-partial-frame", reset);
    qtest_add_func("arcs/i2s/clock-gate", gated);
    qtest_add_func("arcs/i2s/dma-receive", dma_receive);
    qtest_add_data_func("arcs/i2s/reset-ws-low", GUINT_TO_POINTER(0),
                        reset_wire);
    qtest_add_data_func("arcs/i2s/reset-ws-high", GUINT_TO_POINTER(1),
                        reset_wire);
    qtest_add_func("arcs/i2s/master-clock-pause", master_pause);
    static const char *changes[] = {"format", "divider", "slave-clock",
                                    "pinmux", "codec-mixed"};
    for (unsigned i = 0; i < ARRAY_SIZE(changes); i++) {
        g_autofree char *name =
            g_strdup_printf("arcs/i2s/reject-active/%s", changes[i]);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), reject_change);
    }
    int result = g_test_run();
    unlink(source);
    unlink(capture);
    g_free(source);
    g_free(capture);
    return result;
}
