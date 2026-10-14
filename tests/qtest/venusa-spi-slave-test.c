/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "listenai-test-utils.h"
#define MASTER 0x43e00000
#define SLAVE 0x43f00000
static char *flash;
static QTestState *start_wiring(bool reverse)
{
    g_autofree char *path = listenai_test_quote_arg(flash);
    QTestState *q = qtest_initf(
        "-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s "
        "-device "
        "listenai-gpio-wire,gpio=/machine/soc,source=%u,destination=%u "
        "-device "
        "listenai-gpio-wire,gpio=/machine/soc,source=%u,destination=%u "
        "-device "
        "listenai-gpio-wire,gpio=/machine/soc,source=%u,destination=%u "
        "-device "
        "listenai-gpio-wire,gpio=/machine/soc,source=%u,destination=%u",
        path, reverse ? 22 : 16, reverse ? 16 : 22, reverse ? 23 : 19,
        reverse ? 19 : 23, reverse ? 21 : 17, reverse ? 17 : 21,
        reverse ? 18 : 24, reverse ? 24 : 18);
    for (unsigned i = 16; i <= 19; i++) {
        qtest_writel(q, 0x44100000 + i * 4, 5);
    }
    for (unsigned i = 21; i <= 24; i++) {
        qtest_writel(q, 0x44100000 + i * 4, 6);
    }
    for (unsigned i = 0; i < 2; i++) {
        qtest_writel(q, 0x4310001c + i * 4, 0x13800000);
        qtest_writel(q, 0x4310001c + i * 4, 0x93800000);
    }
    qtest_writel(q, MASTER + 0x40, 1); /* 2MHz; half-edge 250ns. */
    return q;
}
static QTestState *start(void)
{
    return start_wiring(false);
}
static void duplex(const void *opaque)
{
    unsigned spec = GPOINTER_TO_UINT(opaque), mode = spec & 3;
    unsigned bits = (spec >> 4) & 63;
    bool merge = spec & 0x400, lsb = spec & 0x800;
    unsigned count = merge ? 8 : 2;
    uint32_t fmt =
        ((bits - 1) << 8) | mode | (merge ? 128 : 0) | (lsb ? 8 : 0);
    uint32_t a[] = {0x7351a69c, 0x2190cbfe}, b[] = {0xc7812345, 0xabcdef09};
    uint32_t mask = bits == 32 || merge ? UINT32_MAX : (1u << bits) - 1;
    QTestState *q = start();
    if (spec & 0x1000) {
        qtest_irq_intercept_out_named(q, "/machine/soc", "pad-out");
    }
    qtest_writel(q, SLAVE + 0x10, fmt | 4);
    qtest_writel(q, SLAVE + 0x38, 16);
    qtest_writel(q, SLAVE + 0x60, 0x10000);
    qtest_writel(q, MASTER + 0x10, fmt);
    qtest_writel(q, MASTER + 0x18, count - 1);
    qtest_writel(q, MASTER + 0x1c, count - 1);
    for (unsigned i = 0; i < 2; i++) {
        qtest_writel(q, SLAVE + 0x2c, b[i]);
        qtest_writel(q, MASTER + 0x2c, a[i]);
    }
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, count * bits * 500 - 1);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x34) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x34) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 16);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x64), ==, count);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x68), ==, count);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(q, MASTER + 0x2c), ==, b[i] & mask);
        g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, a[i] & mask);
    }
    qtest_writel(q, SLAVE + 0x3c, 16);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    qtest_quit(q);
}
static void gpio_drive(QTestState *q, bool cs, bool clk, bool data)
{
    qtest_writel(q, 0x43600024, (cs << 17) | (clk << 16) | (data << 19));
}
static void bitbang_start(QTestState *q, uint32_t format, unsigned mode)
{
    for (unsigned i = 16; i <= 19; i++) {
        qtest_writel(q, 0x44100000 + i * 4, 0);
    }
    gpio_drive(q, true, false, false);
    qtest_writel(q, 0x43600028, (1u << 16) | (1u << 17) | (1u << 19));
    qtest_writel(q, SLAVE + 0x10, format | 4);
    qtest_writel(q, SLAVE + 0x20, mode << 24);
    qtest_writel(q, SLAVE + 0x38, 0x13);
}
static unsigned shift_byte(QTestState *q, unsigned value)
{
    unsigned received = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_drive(q, false, false, (value >> i) & 1);
        gpio_drive(q, false, true, (value >> i) & 1);
        received = (received << 1) | ((qtest_readl(q, 0x43600020) >> 18) & 1);
        gpio_drive(q, false, false, (value >> i) & 1);
    }
    return received;
}
static void byte(QTestState *q, unsigned value)
{
    shift_byte(q, value);
}
static void partial_cancel(void)
{
    QTestState *q = start();
    bitbang_start(q, 0x700, 2);
    qtest_writel(q, SLAVE + 0x60, 0x10000);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34), ==, 0x404000);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    gpio_drive(q, false, false, false);
    for (unsigned i = 0; i < 7; i++) {
        gpio_drive(q, false, true, true);
        gpio_drive(q, false, false, true);
    }
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x64), ==, 0);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34) & 0x1f00, ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 16);
    qtest_writel(q, SLAVE + 0x3c, 16);
    gpio_drive(q, false, false, false);
    byte(q, 0xa7);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0xa7);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x64), ==, 1);
    qtest_writel(q, SLAVE + 0x3c, 16);
    gpio_drive(q, false, false, false);
    gpio_drive(q, false, true, true);
    qtest_writel(q, SLAVE + 0x30, 7);
    byte(q, 0x56);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34), ==, 0x404000);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    qtest_quit(q);
}
static void overflow_underflow(void)
{
    QTestState *q = start();
    bitbang_start(q, 0x700, 0);
    gpio_drive(q, false, false, false);
    for (unsigned i = 0; i < 9; i++) {
        byte(q, 0x20 + i);
    }
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0x13);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x60), ==, 0x60000);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x64), ==, 9);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x68), ==, 9);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0x20 + i);
    }
    qtest_writel(q, SLAVE + 0x60, 0x20000);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x60), ==, 0x40000);
    qtest_writel(q, SLAVE + 0x60, 0x40000);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x60), ==, 0);
    qtest_writel(q, SLAVE + 0x3c, 0x13);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    qtest_quit(q);
}
static void merge_partial_and_pinmux(void)
{
    QTestState *q = start();
    bitbang_start(q, 0x780, 2);
    gpio_drive(q, false, false, false);
    byte(q, 0x91);
    byte(q, 0x37);
    byte(q, 0xd5);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34) & 0x1f00, ==, 0);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0xd53791);
    qtest_writel(q, SLAVE + 0x3c, 16);
    qtest_writel(q, 0x44100000 + 22 * 4, 0); /* SCK not connected to SPI. */
    gpio_drive(q, false, false, false);
    byte(q, 0xe4);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x64), ==, 3);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34) & 0x1f00, ==, 0);
    qtest_quit(q);
}
static void clock_gate(void)
{
    QTestState *q = start();
    qtest_writel(q, SLAVE + 0x10, 0x704);
    qtest_writel(q, SLAVE + 0x2c, 0xc9);
    qtest_writel(q, MASTER + 0x10, 0x700);
    qtest_writel(q, MASTER + 0x2c, 0x6b);
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, 1379);
    qtest_writel(q, 0x4310001c, 0x93000000);
    qtest_clock_step(q, 123456);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x64), ==, 0);
    qtest_writel(q, 0x4310001c, 0x93800000);
    qtest_clock_step(q, 2620);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x34) & 1, ==, 1);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x34) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x2c), ==, 0xc9);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0x6b);
    qtest_quit(q);
}
static void receive_dma_guard(void)
{
    QTestState *q = start();
    bitbang_start(q, 0x780, 2);
    const uint32_t dma = 0x40000000, data = 0x20010004;
    for (unsigned i = 0; i < 5; i++) {
        qtest_writel(q, data - 4 + i * 4, 0xabababab);
    }
    qtest_writel(q, dma, SLAVE + 0x2c);
    qtest_writel(q, dma + 8, data);
    qtest_writel(q, dma + 0x18, 0x200425);
    qtest_writel(q, dma + 0x1c, 2);
    qtest_writel(q, dma + 0x44, 8u << 7);
    qtest_writel(q, dma + 0x398, 1);
    qtest_writel(q, dma + 0x3a0, 0x101);
    qtest_writel(q, SLAVE + 0x30, 8);
    gpio_drive(q, false, false, false);
    for (unsigned i = 0; i < 8; i++) {
        byte(q, i + 1);
    }
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, data), ==, 0x04030201);
    g_assert_cmphex(qtest_readl(q, data + 4), ==, 0x08070605);
    g_assert_cmphex(qtest_readl(q, data - 4), ==, 0xabababab);
    g_assert_cmphex(qtest_readl(q, data + 8), ==, 0xabababab);
    g_assert_cmphex(qtest_readl(q, dma + 0x2c0), ==, 1);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34) & 0x1f00, ==, 0);
    qtest_quit(q);
}
static void reset_before_last_bit(void)
{
    QTestState *q = start();
    const uint32_t dma = 0x40000000, data = 0x20010000;
    bitbang_start(q, 0x700, 2);
    gpio_drive(q, false, false, false);
    qtest_writel(q, data, 0xa55a1234);
    qtest_writel(q, dma, SLAVE + 0x2c);
    qtest_writel(q, dma + 8, data);
    qtest_writel(q, dma + 0x18, 0x200425);
    qtest_writel(q, dma + 0x1c, 1);
    qtest_writel(q, dma + 0x44, 8u << 7);
    qtest_writel(q, dma + 0x398, 1);
    qtest_writel(q, dma + 0x3a0, 0x101);
    qtest_writel(q, SLAVE + 0x30, 8);
    for (unsigned bit = 0; bit < 7; bit++) {
        gpio_drive(q, false, false, true);
        gpio_drive(q, false, true, true);
    }
    gpio_drive(q, false, false, true);
    qtest_system_reset(q);
    qtest_clock_step(q, 10000);
    g_assert_cmphex(qtest_readl(q, data), ==, 0xa55a1234);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x64), ==, 0);
    g_assert_cmphex(qtest_readl(q, dma + 0x2c0), ==, 0);
    qtest_quit(q);
}

static void duplex_dma(void)
{
    QTestState *q = start();
    const uint32_t dma = 0x40000000, mem = 0x20020000;
    for (unsigned array = 0; array < 4; array++) {
        for (unsigned word = 0; word < 27; word++) {
            uint32_t value =
                array < 2 && word > 0 && word <= 25
                    ? 0x9180a000u + array * 0x1020304u + word * 0x10007u
                    : 0xdeadbeef;
            qtest_writel(q, mem + array * 0x100 + word * 4, value);
        }
    }
    qtest_writel(q, MASTER + 0x10, 0x780);
    qtest_writel(q, SLAVE + 0x10, 0x784);
    qtest_writel(q, MASTER + 0x18, 99);
    qtest_writel(q, MASTER + 0x1c, 99);
    qtest_writel(q, dma + 0x398, 1);
    for (unsigned ch = 0; ch < 4; ch++) {
        unsigned peripheral = ch & 1;
        bool rx = ch >= 2;
        uint32_t base = dma + ch * 0x58;
        uint32_t data = mem + ch * 0x100 + 4;
        uint32_t port = (peripheral ? SLAVE : MASTER) + 0x2c;
        unsigned req = 6 + 2 * peripheral + !rx;
        qtest_writel(q, base, rx ? port : data);
        qtest_writel(q, base + 8, rx ? data : port);
        qtest_writel(q, base + 0x18, rx ? 0x200425 : 0x100125);
        qtest_writel(q, base + 0x1c, 25);
        qtest_writel(q, base + 0x44, req << (rx ? 7 : 11));
        qtest_writel(q, dma + 0x3a0, 0x101 << ch);
    }
    qtest_writel(q, MASTER + 0x30, 24);
    qtest_writel(q, SLAVE + 0x30, 24);
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, 399999);
    g_assert_cmphex(qtest_readl(q, mem + 0x200 + 100), ==, 0xdeadbeef);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, dma + 0x2c0), ==, 15);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x64), ==, 100);
    for (unsigned side = 0; side < 2; side++) {
        uint32_t dst = mem + (side + 2) * 0x100;
        uint32_t src = mem + (side ^ 1) * 0x100;
        g_assert_cmphex(qtest_readl(q, dst), ==, 0xdeadbeef);
        for (unsigned i = 1; i <= 25; i++) {
            g_assert_cmphex(qtest_readl(q, dst + i * 4), ==,
                            qtest_readl(q, src + i * 4));
        }
        g_assert_cmphex(qtest_readl(q, dst + 104), ==, 0xdeadbeef);
    }
    qtest_quit(q);
}
static void slave_lost_clock(const void *opaque)
{
    bool mux = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start();
        bitbang_start(q, 0x700, 2);
        gpio_drive(q, false, false, false);
        for (unsigned i = 0; i < 3; i++) {
            gpio_drive(q, false, true, true);
            gpio_drive(q, false, false, true);
        }
        qtest_set_expected_status(q, 1);
        qtest_writel(q, mux ? 0x44100058 : 0x43100020, mux ? 0 : 0x93000000);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(mux ? "*unsupported write*0x44100058*"
                                  : "*unsupported write*0x43100020*");
}
static void late_transmit_fifo(void)
{
    QTestState *q = start();
    bitbang_start(q, 0x700, 1);
    gpio_drive(q, false, false, false);
    qtest_writel(q, SLAVE + 0x2c, 0xe7);
    g_assert_cmphex(shift_byte(q, 0), ==, 0xe7);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x60) & 0x40000, ==, 0);
    qtest_quit(q);
}
static void reverse_controller_roles(void)
{
    QTestState *q = start_wiring(true);
    qtest_writel(q, SLAVE + 0x40, 1);
    qtest_writel(q, MASTER + 0x10, 0xf07);
    qtest_writel(q, SLAVE + 0x10, 0xf03);
    qtest_writel(q, MASTER + 0x2c, 0x61e2);
    qtest_writel(q, SLAVE + 0x2c, 0xabc7);
    qtest_writel(q, SLAVE + 0x24, 0);
    qtest_clock_step(q, 8000);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x2c), ==, 0xabc7);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0x61e2);
    qtest_quit(q);
}
static void slave_irq_mask(void)
{
    QTestState *q = start();
    bitbang_start(q, 0x700, 2);
    qtest_writel(q, SLAVE + 0x38, 0);
    gpio_drive(q, false, false, false);
    byte(q, 0x53);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readb(q, 0xe0021084), ==, 0);
    qtest_writel(q, SLAVE + 0x38, 16);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readb(q, 0xe0021084), ==, 0);
    gpio_drive(q, false, false, false);
    byte(q, 0x69);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 16);
    g_assert_cmphex(qtest_readb(q, 0xe0021084), ==, 1);
    qtest_writel(q, SLAVE + 0x38, 0);
    g_assert_cmphex(qtest_readb(q, 0xe0021084), ==, 0);
    qtest_writel(q, SLAVE + 0x38, 16);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    qtest_writel(q, SLAVE + 0x3c, 16);
    g_assert_cmphex(qtest_readb(q, 0xe0021084), ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0x53);
    qtest_quit(q);
}
static void rejected_mode(const void *opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start();
        bitbang_start(q, mode == 0 ? 0x710 : 0x700, 2);
        qtest_set_expected_status(q, 1);
        if (mode == 1) {
            qtest_writel(q, SLAVE + 0x20, 0x42000000);
        }
        gpio_drive(q, false, false, false);
        if (mode == 2) {
            qtest_writel(q, SLAVE + 0x10, 0xf04);
        }
        if (mode == 3) {
            qtest_writel(q, SLAVE + 0x30, 2);
        }
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*unsupported write*");
}
static void manual_cs_idle_clock(void)
{
    QTestState *q = start();
    qtest_writel(q, SLAVE + 0x10, 0x707);
    qtest_writel(q, MASTER + 0x10, 0x703);
    /* CPOL is already visible before manual CS is asserted or CMD issued. */
    g_assert_cmphex(qtest_readl(q, 0x43600020) & (1u << 22), ==, 1u << 22);
    qtest_writel(q, SLAVE + 0x38, 16);
    qtest_writel(q, SLAVE + 0x2c, 0xa9);
    qtest_writel(q, MASTER + 0x2c, 0x36);
    qtest_writel(q, MASTER + 0x30, 1u << 21);
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, 4000);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x2c), ==, 0xa9);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0x36);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    qtest_writel(q, MASTER + 0x30, (1u << 21) | (1u << 22));
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 16);
    qtest_quit(q);
}
static void observer_ssi_route(const void *opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        g_autofree char *path = listenai_test_quote_arg(flash);
        static const char *routes[] = {
            "",
            "",
            "-device listenai-gpio-wire,gpio=/machine/soc,"
            "source=16,destination=22",
            "-device listenai-gpio-wire,gpio=/machine/soc,"
            "source=17,destination=20",
            "-device listenai-gpio-wire,gpio=/machine/soc,"
            "source=19,destination=20",
        };
        QTestState *q =
            qtest_initf("-machine venusa-rd-evb -drive "
                        "if=pflash,format=raw,file=%s "
                        "-device arcs-st7789,id=panel,bus=venusa-spi0 %s",
                        path, routes[mode]);
        for (unsigned pad = 16; pad <= 19; pad++) {
            qtest_writel(q, 0x44100000 + pad * 4, 5);
        }
        if (mode == 1) {
            qtest_irq_intercept_out_named(q, "/machine/soc", "pad-out");
        }
        qtest_writel(q, 0x4310001c, 0x13800000);
        qtest_writel(q, 0x4310001c, 0x93800000);
        qtest_writel(q, MASTER + 0x10, 0x700);
        qtest_writel(q, MASTER + 0x40, 1);
        qtest_writel(q, MASTER + 0x20, 0x01000000);
        qtest_writel(q, MASTER + 0x2c, mode == 2 ? 0 : 0xff);
        qtest_set_expected_status(q, 1);
        qtest_writel(q, MASTER + 0x24, 0);
        qtest_clock_step(q, 4000);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    /*
     * Observation must preserve the actual SSI device's command rejection.
     * A real wire plus an SSI device is rejected as an ambiguous route.
     */
    g_test_trap_assert_stderr(mode == 2 ? "*unsupported write*0x43e00024*"
                                        : "*ST7789*unimplemented command*");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-spi-slave-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    {
        gsize size = 16 * 1024 * 1024;
        g_autofree char *image = g_malloc0(size);
        g_assert_true(g_file_set_contents(flash, image, size, NULL));
    }
    for (unsigned mode = 0; mode < 4; mode++) {
        unsigned widths[] = {8, 16, 32};
        for (unsigned i = 0; i < 3; i++) {
            for (unsigned lsb = 0; lsb < 2; lsb++) {
                g_autofree char *name = g_strdup_printf(
                    "spi-slave/mode%u/width%u/lsb%u", mode, widths[i], lsb);
                qtest_add_data_func(
                    name, GUINT_TO_POINTER(mode | widths[i] << 4 | lsb << 11),
                    duplex);
            }
        }
        g_autofree char *name =
            g_strdup_printf("spi-slave/mode%u/merge8", mode);
        qtest_add_data_func(name, GUINT_TO_POINTER(mode | 8 << 4 | 0x400),
                            duplex);
    }
    qtest_add_func("spi-slave/partial-cancel", partial_cancel);
    qtest_add_func("spi-slave/overflow-underflow", overflow_underflow);
    qtest_add_func("spi-slave/partial-merge-pinmux", merge_partial_and_pinmux);
    qtest_add_func("spi-slave/clock-gate", clock_gate);
    qtest_add_func("spi-slave/receive-dma-guard", receive_dma_guard);
    qtest_add_func("spi-slave/duplex-dma", duplex_dma);
    qtest_add_func("spi-slave/reset-before-last-bit", reset_before_last_bit);
    qtest_add_data_func("spi-slave/lost-clock", GUINT_TO_POINTER(0),
                        slave_lost_clock);
    qtest_add_data_func("spi-slave/lost-pinmux", GUINT_TO_POINTER(1),
                        slave_lost_clock);
    qtest_add_func("spi-slave/late-transmit-fifo", late_transmit_fifo);
    qtest_add_func("spi-slave/reverse-roles", reverse_controller_roles);
    qtest_add_func("spi-slave/irq-mask", slave_irq_mask);
    const char *reject_names[] = {"bidirectional", "command-phase",
                                  "live-format", "live-fifo-reset"};
    for (unsigned i = 0; i < G_N_ELEMENTS(reject_names); i++) {
        g_autofree char *name =
            g_strdup_printf("spi-slave/reject/%s", reject_names[i]);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), rejected_mode);
    }
    qtest_add_func("spi-slave/manual-cs-idle-clock", manual_cs_idle_clock);
    qtest_add_data_func("spi-slave/ssi-baseline", GUINT_TO_POINTER(0),
                        observer_ssi_route);
    qtest_add_data_func("spi-slave/ssi-observed", GUINT_TO_POINTER(1),
                        observer_ssi_route);
    qtest_add_data_func("spi-slave/ssi-wire-conflict", GUINT_TO_POINTER(2),
                        observer_ssi_route);
    qtest_add_data_func("spi-slave/ssi-cs-wire", GUINT_TO_POINTER(3),
                        observer_ssi_route);
    qtest_add_data_func("spi-slave/ssi-mosi-wire", GUINT_TO_POINTER(4),
                        observer_ssi_route);
    qtest_add_data_func("spi-slave/observed-digital-route",
                        GUINT_TO_POINTER(0x1480), duplex);
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
