/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#define SPI 0x43e00000
#define CFG 0x4310001c
#define PENDING (0xe0021000 + 32 * 4)
static char *flash;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
                               "if=pflash,format=raw,file=%s", path);
    qtest_writel(q, CFG, 0x13800000);
    qtest_writel(q, CFG, 0x93800000); /* CORE24M N1/M3 = 8MHz. */
    qtest_writel(q, SPI + 0x10, 0x700);
    qtest_writel(q, SPI + 0x40, 1);   /* 2MHz serial clock, 4us / byte. */
    return q;
}
static void fifo_and_pause(void)
{
    QTestState *q = start();
    g_assert_cmphex(qtest_readl(q, SPI + 0x7c), ==, 0x4322);
    qtest_writel(q, SPI + 0x1c, 16);
    qtest_writel(q, SPI + 0x20, 0x02000000);
    qtest_writel(q, SPI + 0x24, 0);
    qtest_clock_step(q, 31999);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x9f01, ==, 0x701);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x9f01, ==, 0x8801);
    qtest_clock_step(q, 1000000); /* Full FIFO pauses, rather than dropping. */
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x9f01, ==, 0x8801);
    for (unsigned i = 0; i < 9; i++) {
        qtest_readl(q, SPI + 0x2c);
        qtest_clock_step(q, 3999);
        g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x1f00, ==, 0x700);
        qtest_clock_step(q, 1);
    }
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x9f01, ==, 0x8800);
    for (unsigned i = 0; i < 8; i++) { qtest_readl(q, SPI + 0x2c); }
    g_assert_cmphex(qtest_readl(q, SPI + 0x34), ==, 0x404000);
    qtest_quit(q);
}
static void dma_receive_mux(void)
{
    QTestState *q = start();
    const uint32_t dma = 0x40000000, data = 0x20010000;
    for (unsigned i = 0; i < 17; i++) { qtest_writel(q, data + i * 4, 0xa5a5a5a5); }
    qtest_writel(q, dma, SPI + 0x2c);
    qtest_writel(q, dma + 8, data);
    qtest_writel(q, dma + 0x18, 0x200425); /* P2M, 32-bit fixed source. */
    qtest_writel(q, dma + 0x1c, 17);
    qtest_writel(q, dma + 0x40, 0x100); /* Suspend while the first frame arrives. */
    qtest_writel(q, dma + 0x44, 6u << 7);
    qtest_writel(q, dma + 0x398, 1);
    qtest_writel(q, dma + 0x3a0, 0x101);
    qtest_writel(q, SPI + 0x30, 8);
    qtest_writel(q, SPI + 0x1c, 16);
    qtest_writel(q, SPI + 0x20, 0x02000000);
    qtest_writel(q, SPI + 0x24, 0);
    qtest_clock_step(q, 3999);
    g_assert_cmphex(qtest_readl(q, data), ==, 0xa5a5a5a5);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x1f00, ==, 0x100);
    /* UART1 shares request6/7 through the mux, but is not its selected owner. */
    qtest_writel(q, 0x43900000, 0x01400003);
    qtest_writel(q, dma + 0x40, 0);
    g_assert_cmphex(qtest_readl(q, dma + 8), ==, data + 4);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x1f00, ==, 0);
    qtest_clock_step(q, 64000);
    g_assert_cmphex(qtest_readl(q, dma + 8), ==, data + 68);
    g_assert_cmphex(qtest_readl(q, dma + 0x2c0), ==, 1);
    for (unsigned i = 0; i < 17; i++) {
        /* No SSI slave is attached: assert the transport result, not a device ID. */
        g_assert_cmphex(qtest_readl(q, data + i * 4), ==, 0);
    }
    qtest_quit(q);
}
static void receive_interrupt(void)
{
    QTestState *q = start();
    qtest_writel(q, SPI + 0x30, 1u << 8);
    qtest_writel(q, SPI + 0x38, 20);
    qtest_writel(q, SPI + 0x1c, 0);
    qtest_writel(q, SPI + 0x20, 0x02000000);
    qtest_writel(q, SPI + 0x24, 0);
    qtest_clock_step(q, 4000);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 20);
    qtest_writel(q, SPI + 0x3c, 20);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 4);
    qtest_readl(q, SPI + 0x2c);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 4);
    qtest_writel(q, SPI + 0x3c, 4);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 0);
    g_assert_cmpuint(qtest_readb(q, PENDING), ==, 0);
    qtest_writel(q, SPI + 0x30, 0);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 4);
    qtest_writel(q, SPI + 0x3c, 4);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 4);
    qtest_quit(q);
}
static void transmit_clock_and_reset(void)
{
    QTestState *q = start();
    for (unsigned i = 0; i < 8; i++) { qtest_writel(q, SPI + 0x2c, i); }
    g_assert_cmphex(qtest_readl(q, SPI + 0x34), ==, 0x884000);
    qtest_writel(q, SPI + 0x18, 7);
    qtest_writel(q, SPI + 0x20, 0x01000000);
    qtest_writel(q, SPI + 0x38, 16);
    qtest_writel(q, SPI + 0x24, 0);
    qtest_clock_step(q, 1000);
    qtest_writel(q, CFG, 0x93000000); /* Gate with 3us left in first byte. */
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x1f0001, ==, 0x80001);
    qtest_writel(q, CFG, 0x93800000);
    qtest_clock_step(q, 2999);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x1f0000, ==, 0x80000);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34) & 0x1f0000, ==, 0x70000);
    qtest_clock_step(q, 28000);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 16);
    g_assert_cmpuint(qtest_readb(q, PENDING), ==, 1);
    qtest_writel(q, SPI + 0x3c, 16);
    g_assert_cmpuint(qtest_readb(q, PENDING), ==, 0);
    qtest_writel(q, SPI + 0x2c, 0xa5);
    qtest_writel(q, SPI + 0x18, 0);
    qtest_writel(q, SPI + 0x24, 0);
    qtest_clock_step(q, 1000);
    qtest_writel(q, 0x43100014, 1u << 7);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, SPI + 0x34), ==, 0x404000);
    g_assert_cmphex(qtest_readl(q, SPI + 0x3c), ==, 0);
    qtest_quit(q);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-spi-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("venusa/spi/fifo-pause", fifo_and_pause);
    qtest_add_func("venusa/spi/clock-reset", transmit_clock_and_reset);
    qtest_add_func("venusa/spi/receive-irq", receive_interrupt);
    qtest_add_func("venusa/spi/dma-mux", dma_receive_mux);
    int result = g_test_run();
    unlink(flash); g_free(flash);
    return result;
}
