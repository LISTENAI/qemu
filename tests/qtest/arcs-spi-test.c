/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define MASTER 0x47000000
#define SLAVE 0x47100000
#define MUX 0x47500000
#define CLOCK 0x46000014
#define GPIO 0x46700000
#define DMA 0x40000000
#define RAM 0x20010000

static QTestState *start(void)
{
    QTestState *q = qtest_init(
        "-machine arcs-evb "
        "-device listenai-gpio-wire,gpio=/machine/soc,source=15,destination=25 "
        "-device listenai-gpio-wire,gpio=/machine/soc,source=14,destination=24 "
        "-device listenai-gpio-wire,gpio=/machine/soc,source=12,destination=22 "
        "-device "
        "listenai-gpio-wire,gpio=/machine/soc,source=23,destination=13");
    for (unsigned i = 12; i <= 15; i++) {
        qtest_writel(q, MUX + i * 4, 5);
    }
    for (unsigned i = 22; i <= 25; i++) {
        qtest_writel(q, MUX + i * 4, 6);
    }
    qtest_writel(q, CLOCK, 0x13800000);
    qtest_writel(q, CLOCK + 4, 0x13800000);
    qtest_writel(q, MASTER + 0x40, 1); /* 2 MHz, 250 ns per half edge. */
    return q;
}

static void duplex(const void *opaque)
{
    unsigned spec = GPOINTER_TO_UINT(opaque), mode = spec & 3;
    unsigned bits = (spec >> 4) & 63;
    bool merge = spec & 0x400, lsb = spec & 0x800;
    unsigned count = merge ? 8 : 2;
    uint32_t fmt = ((bits - 1) << 8) | mode | (merge ? 128 : 0) | (lsb ? 8 : 0);
    uint32_t a[] = {0x7351a69c, 0x2190cbfe}, b[] = {0xc7812345, 0xabcdef09};
    uint32_t mask = bits == 32 || merge ? UINT32_MAX : (1u << bits) - 1;
    QTestState *q = start();
    if (spec & 0x2000) {
        /* MISO is an output: both selected pads carry the same signal. */
        qtest_writel(q, MUX + 2 * 4, 6);
    }
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
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c) & 16, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x34) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c) & 16, ==, 16);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x64), ==, count);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x68), ==, count);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(q, MASTER + 0x2c), ==, b[i] & mask);
        g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, a[i] & mask);
    }
    qtest_writel(q, SLAVE + 0x3c, 16);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c) & 16, ==, 0);
    qtest_quit(q);
}

static void gpio_drive(QTestState *q, bool cs, bool clk, bool data)
{
    qtest_writel(q, GPIO + 0x24, (cs << 12) | (clk << 15) | (data << 14));
}

static void bitbang_start(QTestState *q)
{
    for (unsigned i = 12; i <= 15; i++) {
        qtest_writel(q, MUX + i * 4, 1);
    }
    gpio_drive(q, true, false, false);
    qtest_writel(q, GPIO + 0x28, (1u << 12) | (1u << 14) | (1u << 15));
    qtest_writel(q, SLAVE + 0x10, 0x704);
    qtest_writel(q, SLAVE + 0x20, 0x02000000);
    qtest_writel(q, SLAVE + 0x38, 0x11);
    gpio_drive(q, false, false, false);
}

static void byte(QTestState *q, unsigned value)
{
    for (int i = 7; i >= 0; i--) {
        gpio_drive(q, false, false, (value >> i) & 1);
        gpio_drive(q, false, true, (value >> i) & 1);
        gpio_drive(q, false, false, (value >> i) & 1);
    }
}

static void fifo_and_cancel(void)
{
    QTestState *q = start();
    bitbang_start(q);
    for (unsigned i = 0; i < 17; i++) {
        byte(q, i);
    }
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34) & 0x9f00, ==, 0x9000);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x64), ==, 17);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c) & 1, ==, 1);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmpuint(qtest_readl(q, SLAVE + 0x2c), ==, i);
    }
    qtest_writel(q, SLAVE + 0x3c, 1);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c) & 1, ==, 0);
    for (unsigned i = 0; i < 3; i++) {
        gpio_drive(q, false, false, true);
        gpio_drive(q, false, true, true);
    }
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34) & 0x1f00, ==, 0);
    qtest_writel(q, SLAVE + 0x3c, 16);
    gpio_drive(q, false, false, false);
    byte(q, 0x69);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x2c), ==, 0x69);
    qtest_writel(q, 0x4600000c, 16); /* SPI1 software reset cancels this CS. */
    byte(q, 0xff);
    gpio_drive(q, true, false, false);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34) & 0x1f01, ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c) & 16, ==, 0);
    qtest_quit(q);
}

static void pad_clock(void)
{
    QTestState *q = qtest_init("-machine arcs-mini");
    qtest_irq_intercept_out_named(q, "/machine/soc", "pad-out");
    qtest_writel(q, MUX + 21 * 4, 5); /* PA21 is another SPI0 SCLK route. */
    qtest_writel(q, MASTER + 0x10, 0x702);
    g_assert_true(qtest_get_irq(q, 21));
    qtest_writel(q, MASTER + 0x10, 0x700);
    g_assert_false(qtest_get_irq(q, 21));
    qtest_writel(q, MUX + 21 * 4, 1);
    qtest_writel(q, GPIO + 0x28, 1u << 21);
    qtest_writel(q, GPIO + 0x24, 0);
    qtest_writel(q, MASTER + 0x10, 0x702);
    g_assert_false(qtest_get_irq(q, 21));
    qtest_quit(q);
}

static void dma_receive(void)
{
    QTestState *q = start();
    bitbang_start(q);
    qtest_writel(q, RAM, 0xaaaa5555);
    qtest_writel(q, RAM + 4, 0xaaaa5555);
    qtest_writel(q, DMA, SLAVE + 0x2c);
    qtest_writel(q, DMA + 8, RAM);
    qtest_writel(q, DMA + 0x18, 0x200425);
    qtest_writel(q, DMA + 0x1c, 1);
    qtest_writel(q, DMA + 0x44, 6u << 7);
    qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    qtest_writel(q, 0x46000094,
                 1u << 12); /* Request6 no longer selects SPI1. */
    qtest_writel(q, SLAVE + 0x30, 8);
    byte(q, 0x96);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0xaaaa5555);
    qtest_writel(q, 0x46000094, 0);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0x96);
    g_assert_cmphex(qtest_readl(q, RAM + 4), ==, 0xaaaa5555);
    g_assert_cmphex(qtest_readl(q, DMA + 8), ==, RAM + 4);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0), ==, 1);
    qtest_quit(q);
}

/* Match the three independently captured ARCS IRQ/threshold matrices. */
static void hardware_contract(const void *opaque)
{
    unsigned index = GPOINTER_TO_UINT(opaque);
    uint32_t spi = MASTER + index * 0x100000;
    uint32_t pending = 0xe0021000 + (46 + index) * 4;
    QTestState *q =
        qtest_init(index == 0 ? "-machine arcs-evb" : "-machine arcs-mini");
    g_assert_cmphex(qtest_readl(q, spi), ==, 0x02002044);
    g_assert_cmphex(qtest_readl(q, spi + 0x7c), ==, 0x4b33);
    qtest_writel(q, CLOCK + index * 4, 0x13800000);
    qtest_writel(q, spi + 0x10, 0x1f00);
    qtest_writel(q, spi + 0x40, 255); /* 32 bits at 8 MHz = 4 us. */
    for (unsigned count = 7; count <= 9; count++) {
        qtest_writel(q, spi + 0x38, 0);
        qtest_writel(q, spi + 0x30, 0x80807);
        for (unsigned n = 0; n < count; n++) {
            qtest_writel(q, spi + 0x2c, n);
        }
        qtest_writel(q, spi + 0x38, 8);
        g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, count <= 8 ? 8 : 0);
        g_assert_cmphex(qtest_readb(q, pending), ==, count <= 8);
    }
    qtest_writel(q, spi + 0x30, 0x80807);
    for (unsigned n = 0; n < 9; n++) {
        qtest_writel(q, spi + 0x2c, n);
    }
    g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 8);
    qtest_writel(q, spi + 0x38, 0);
    qtest_writel(q, spi + 0x38, 8);
    g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 0);
    for (unsigned n = 9; n < 16; n++) {
        qtest_writel(q, spi + 0x2c, n);
    }
    g_assert_cmphex(qtest_readl(q, spi + 0x34), ==, 0x904000);
    for (unsigned count = 7; count <= 9; count++) {
        qtest_writel(q, spi + 0x38, 0);
        qtest_writel(q, spi + 0x30, 0x80807);
        qtest_writel(q, spi + 0x1c, count - 1);
        qtest_writel(q, spi + 0x20, 0x02000000);
        qtest_writel(q, spi + 0x24, 0);
        qtest_clock_step(q, count * 4000);
        qtest_writel(q, spi + 0x38, 4);
        g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, count >= 8 ? 4 : 0);
        g_assert_cmphex(qtest_readb(q, pending), ==, count >= 8);
        for (unsigned n = 0; n < count; n++) {
            qtest_readl(q, spi + 0x2c);
        }
        g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, count >= 8 ? 4 : 0);
        qtest_writel(q, spi + 0x38, 0);
        qtest_writel(q, spi + 0x38, 4);
        g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 0);
    }
    qtest_writel(q, spi + 0x38, 0x18);
    qtest_writel(q, spi + 0x1c, 0);
    qtest_writel(q, spi + 0x24, 0);
    qtest_clock_step(q, 4000);
    for (unsigned n = 0; n < 9; n++) {
        qtest_writel(q, spi + 0x2c, n);
    }
    g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 0x18);
    qtest_writel(q, spi + 0x3c, 8);
    g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 16);
    qtest_writel(q, spi + 0x3c, 0);
    g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 16);
    qtest_writel(q, spi + 0x38, 0);
    qtest_writel(q, spi + 0x38, 16);
    g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 0);
    qtest_writel(q, spi + 0x38, 0);
    qtest_writel(q, spi + 0x24, 0);
    qtest_clock_step(q, 4000);
    qtest_writel(q, spi + 0x38, 16);
    g_assert_cmphex(qtest_readl(q, spi + 0x3c), ==, 0);
    qtest_quit(q);
}

static void clock_and_system_reset(void)
{
    QTestState *q = start();
    qtest_writel(q, SLAVE + 0x10, 0x704);
    qtest_writel(q, SLAVE + 0x20, 0x02000000);
    qtest_writel(q, MASTER + 0x10, 0x700);
    qtest_writel(q, MASTER + 0x20, 0x01000000);
    qtest_writel(q, MASTER + 0x38, 16);
    qtest_writel(q, MASTER + 0x2c, 0x96);
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, 1001);
    qtest_writel(q, CLOCK, 0x13000000);
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, SLAVE + 0x64), ==, 0);
    qtest_writel(q, CLOCK, 0x13800000);
    qtest_clock_step(q, 2998);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x3c), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x3c), ==, 16);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0x96);
    qtest_writel(q, MASTER + 0x2c, 0xa5);
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, 1000);
    qtest_system_reset(q);
    qtest_clock_step(q, 10000);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x34), ==, 0x404000);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x34), ==, 0x404000);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    qtest_quit(q);
}

static void spi2_slave(void)
{
    QTestState *q = qtest_init(
        "-machine arcs-mini "
        "-device listenai-gpio-wire,gpio=/machine/soc,source=11,destination=20 "
        "-device listenai-gpio-wire,gpio=/machine/soc,source=9,destination=19 "
        "-device listenai-gpio-wire,gpio=/machine/soc,source=7,destination=21 "
        "-device listenai-gpio-wire,gpio=/machine/soc,source=18,destination=8");
    for (unsigned n = 7; n <= 11; n++) {
        if (n != 10) {
            qtest_writel(q, MUX + n * 4, 6);
        }
    }
    for (unsigned n = 18; n <= 21; n++) {
        qtest_writel(q, MUX + n * 4, 7);
    }
    qtest_writel(q, CLOCK + 4, 0x13800000);
    qtest_writel(q, CLOCK + 8, 0x13800000);
    uint32_t slave = 0x47200000;
    qtest_writel(q, slave + 0x10, 0x704);
    qtest_writel(q, slave + 0x2c, 0xa5);
    qtest_writel(q, SLAVE + 0x10, 0x700);
    qtest_writel(q, SLAVE + 0x40, 1);
    qtest_writel(q, SLAVE + 0x2c, 0x5a);
    qtest_writel(q, SLAVE + 0x24, 0);
    qtest_clock_step(q, 4000);
    g_assert_cmphex(qtest_readl(q, slave + 0x2c), ==, 0x5a);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x2c), ==, 0xa5);
    /* SPI2 RX request4 remains selected when unrelated UART requests change. */
    qtest_writel(q, RAM, 0xaaaa5555);
    qtest_writel(q, RAM + 4, 0xaaaa5555);
    qtest_writel(q, DMA, slave + 0x2c);
    qtest_writel(q, DMA + 8, RAM);
    qtest_writel(q, DMA + 0x18, 0x200425);
    qtest_writel(q, DMA + 0x1c, 1);
    qtest_writel(q, DMA + 0x44, 4u << 7);
    qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    qtest_writel(q, 0x46000094, 0x101); /* Disable SPI2 RX, select UART2 RX. */
    qtest_writel(q, slave + 0x30, 8);
    qtest_writel(q, slave + 0x2c, 0x69);
    qtest_writel(q, SLAVE + 0x2c, 0xc3);
    qtest_writel(q, SLAVE + 0x24, 0);
    qtest_clock_step(q, 4000);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0xaaaa5555);
    qtest_writel(q, 0x46000094, 1);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0xc3);
    g_assert_cmphex(qtest_readl(q, RAM + 4), ==, 0xaaaa5555);
    g_assert_cmphex(qtest_readl(q, DMA + 8), ==, RAM + 4);
    qtest_quit(q);
}

static void loopback(void)
{
    QTestState *q = qtest_init(
        "-machine arcs-evb "
        "-device "
        "listenai-gpio-wire,gpio=/machine/soc,source=14,destination=13");
    for (unsigned n = 12; n <= 15; n++) {
        qtest_writel(q, MUX + n * 4, 5);
    }
    qtest_writel(q, CLOCK, 0x13800000);
    qtest_writel(q, MASTER + 0x40, 1);
    qtest_writel(q, MASTER + 0x10, 0x1f00);
    qtest_writel(q, MASTER + 0x2c, 0xa568bc19);
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, 16000);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x2c), ==, 0xa568bc19);
    qtest_quit(q);
}

static void pad_directions(void)
{
    QTestState *q = start();
    qtest_irq_intercept_out_named(q, "/machine/soc", "pad-out");
    for (unsigned i = 12; i <= 15; i++) {
        qtest_writel(q, MUX + i * 4, 1);
    }
    gpio_drive(q, true, false, false);
    qtest_writel(q, GPIO + 0x28, (1u << 12) | (1u << 14) | (1u << 15));
    qtest_writel(q, SLAVE + 0x10, 0x704);
    qtest_writel(q, SLAVE + 0x20, 0x01000000);
    /* Clock is now an input and follows the external level. */
    qtest_set_irq_in(q, "/machine/soc", "pad-in", 25, 1);
    g_assert_true(qtest_get_irq(q, 25));
    qtest_set_irq_in(q, "/machine/soc", "pad-in", 25, 0);
    g_assert_false(qtest_get_irq(q, 25));
    qtest_writel(q, SLAVE + 0x2c, 0x80);
    gpio_drive(q, false, false, false);
    qtest_set_irq_in(q, "/machine/soc", "pad-in", 23, 0);
    /* Selected slave drives the first bit. */
    g_assert_true(qtest_get_irq(q, 23));
    gpio_drive(q, true, false, false);
    g_assert_false(qtest_get_irq(q, 23)); /* CS deassertion releases MISO. */
    qtest_writel(q, SLAVE + 0x10, 0x702);
    qtest_set_irq_in(q, "/machine/soc", "pad-in", 25, 0);
    g_assert_true(qtest_get_irq(q, 25)); /* Master drives CPOL independently. */
    qtest_set_irq_in(q, "/machine/soc", "pad-in", 23, 1);
    g_assert_true(qtest_get_irq(q, 23));
    qtest_set_irq_in(q, "/machine/soc", "pad-in", 23, 0);
    g_assert_false(qtest_get_irq(q, 23)); /* Master MISO remains an input. */
    qtest_quit(q);
}

static void reset_before_last_bit(void)
{
    QTestState *q = start();
    bitbang_start(q);
    qtest_writel(q, RAM, 0xa55a1234);
    qtest_writel(q, DMA, SLAVE + 0x2c);
    qtest_writel(q, DMA + 8, RAM);
    qtest_writel(q, DMA + 0x18, 0x200425);
    qtest_writel(q, DMA + 0x1c, 1);
    qtest_writel(q, DMA + 0x44, 6u << 7);
    qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    qtest_writel(q, SLAVE + 0x30, 8);
    for (unsigned bit = 0; bit < 7; bit++) {
        gpio_drive(q, false, false, true);
        gpio_drive(q, false, true, true);
    }
    gpio_drive(q, false, false, true);
    qtest_system_reset(q);
    qtest_clock_step(q, 10000);
    g_assert_cmphex(qtest_readl(q, RAM), ==, 0xa55a1234);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(q, SLAVE + 0x64), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0), ==, 0);
    qtest_quit(q);
}

static void lost_slave_route(const void *opaque)
{
    unsigned mux = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start();
        bitbang_start(q);
        for (unsigned bit = 0; bit < 3; bit++) {
            gpio_drive(q, false, false, true);
            gpio_drive(q, false, true, true);
        }
        qtest_set_expected_status(q, 1);
        qtest_writel(q, mux ? MUX + 25 * 4 : CLOCK + 4, mux ? 1 : 0x13000000);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(mux ? "*unsupported write*0x47500064*"
                                  : "*unsupported write*0x46000018*");
}

static void ssi_route(const void *opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q =
            qtest_initf("-machine arcs-evb "
                        "-device st7789,id=panel,bus=arcs-spi0 %s",
                        mode == 2 ? "-device "
                                    "listenai-gpio-wire,gpio=/machine/"
                                    "soc,source=15,destination=25"
                        : mode == 3 ? "-device "
                                      "listenai-gpio-wire,gpio=/machine/"
                                      "soc,source=12,destination=20"
                        : mode == 4 ? "-device "
                                      "listenai-gpio-wire,gpio=/machine/"
                                      "soc,source=14,destination=20"
                                    : "");
        for (unsigned pad = 12; pad <= 15; pad++) {
            qtest_writel(q, MUX + pad * 4, 5);
        }
        if (mode == 1) {
            qtest_irq_intercept_out_named(q, "/machine/soc", "pad-out");
        }
        /* The child and its SSI device survive a global reset. */
        qtest_system_reset(q);
        for (unsigned pad = 12; pad <= 15; pad++) {
            qtest_writel(q, MUX + pad * 4, 5);
        }
        qtest_writel(q, CLOCK, 0x13800000);
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
    g_test_trap_assert_stderr(mode == 2 ? "*unsupported write*0x47000024*"
                                        : "*ST7789*unimplemented command*");
}

static void panel_byte(QTestState *q, bool data, uint8_t value)
{
    qtest_set_irq_in(q, "/machine/peripheral/panel", NULL, 0, data);
    qtest_writel(q, MASTER + 0x2c, value);
    qtest_writel(q, MASTER + 0x24, 0);
    qtest_clock_step(q, 4000);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x34) & 1, ==, 0);
}

static void controller_reset_keeps_panel(void)
{
    QTestState *q = qtest_init("-machine arcs-evb "
                              "-device st7789,id=panel,bus=arcs-spi0");
    for (unsigned pad = 12; pad <= 15; pad++) {
        qtest_writel(q, MUX + pad * 4, 5);
    }
    qtest_writel(q, CLOCK, 0x13800000);
    qtest_writel(q, MASTER + 0x10, 0x700);
    qtest_writel(q, MASTER + 0x40, 1);
    qtest_writel(q, MASTER + 0x20, 0x01000000);
    panel_byte(q, false, 0x3a);
    panel_byte(q, true, 0x55);
    panel_byte(q, false, 0x2c);
    panel_byte(q, true, 0xf8);

    /* SPI0 reset must preserve the panel's format and partial RGB565 pixel. */
    qtest_writel(q, 0x4600000c, 8);
    g_assert_cmphex(qtest_readl(q, MASTER + 0x10), ==, 0);
    qtest_writel(q, MASTER + 0x10, 0x700);
    qtest_writel(q, MASTER + 0x40, 1);
    qtest_writel(q, MASTER + 0x20, 0x01000000);
    panel_byte(q, true, 0x00);
    /* A new command also rejects a lost/unfinished pixel byte. */
    panel_byte(q, false, 0x2c);
    panel_byte(q, true, 0x07);
    panel_byte(q, true, 0xe0);
    panel_byte(q, false, 0x00);
    qtest_quit(q);
}

static void duplicate_wire(void)
{
    if (g_test_subprocess()) {
        qtest_init(
            "-machine arcs-evb "
            "-device "
            "listenai-gpio-wire,gpio=/machine/soc,source=14,destination=13 "
            "-device "
            "listenai-gpio-wire,gpio=/machine/soc,source=15,destination=13");
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(
        "*Digital wire destination already has a driver*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (unsigned mode = 0; mode < 4; mode++) {
        for (unsigned bits = 8; bits <= 32; bits *= 2) {
            for (unsigned lsb = 0; lsb < 2; lsb++) {
                g_autofree char *name =
                    g_strdup_printf("arcs-spi/mode%u/%ubit/%s", mode, bits,
                                    lsb ? "lsb" : "msb");
                qtest_add_data_func(
                    name,
                    GUINT_TO_POINTER(mode | (bits << 4) | (lsb ? 0x800 : 0)),
                    duplex);
            }
        }
        g_autofree char *name = g_strdup_printf("arcs-spi/mode%u/merge", mode);
        qtest_add_data_func(name, GUINT_TO_POINTER(mode | (8 << 4) | 0x400),
                            duplex);
    }
    qtest_add_data_func("arcs-spi/observed-wire",
                        GUINT_TO_POINTER((8 << 4) | 0x1000), duplex);
    qtest_add_data_func("arcs-spi/output-fanout", GUINT_TO_POINTER(0x2080),
                        duplex);
    qtest_add_func("arcs-spi/fifo-cancel", fifo_and_cancel);
    qtest_add_func("arcs-spi/pad-clock", pad_clock);
    qtest_add_func("arcs-spi/rx-dma-mux", dma_receive);
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("arcs-spi/hardware/%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), hardware_contract);
    }
    qtest_add_func("arcs-spi/clock-system-reset", clock_and_system_reset);
    qtest_add_func("arcs-spi/spi2-slave", spi2_slave);
    qtest_add_func("arcs-spi/mosi-miso-wire", loopback);
    qtest_add_data_func("arcs-spi/lost-slave-clock", GUINT_TO_POINTER(0),
                        lost_slave_route);
    qtest_add_data_func("arcs-spi/lost-slave-mux", GUINT_TO_POINTER(1),
                        lost_slave_route);
    for (unsigned mode = 0; mode < 5; mode++) {
        g_autofree char *name = g_strdup_printf("arcs-spi/ssi-route/%u", mode);
        qtest_add_data_func(name, GUINT_TO_POINTER(mode), ssi_route);
    }
    qtest_add_func("arcs-spi/controller-reset-keeps-panel",
                   controller_reset_keeps_panel);
    qtest_add_func("arcs-spi/wire-single-driver", duplicate_wire);
    qtest_add_func("arcs-spi/pad-directions", pad_directions);
    qtest_add_func("arcs-spi/reset-before-last-bit", reset_before_last_bit);
    return g_test_run();
}
