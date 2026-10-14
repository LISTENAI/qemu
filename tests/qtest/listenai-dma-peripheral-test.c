/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#define DMA 0x40000000
#define UART 0x43800000
static char *flash;
static void descriptor(QTestState *q, uint32_t address, uint32_t destination,
                        uint32_t next)
{
    qtest_writel(q, address, UART + 8);
    qtest_writel(q, address + 4, destination);
    qtest_writel(q, address + 8, next);
    qtest_writel(q, address + 12, 0x18200401);
    qtest_writel(q, address + 16, 2);
}
static void linked_receive(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
                               "if=pflash,format=raw,file=%s", path);
    qtest_writel(q, UART, 0x01400003);
    descriptor(q, 0x20010200, 0x20010000, 0x20010220);
    descriptor(q, 0x20010220, 0x20010010, 0);
    qtest_writel(q, DMA + 0x10, 0x20010200);
    qtest_writel(q, DMA + 0x18, 0x18000000);
    qtest_writel(q, DMA + 0x310, 0x101);
    qtest_writel(q, DMA + 0x318, 0x101);
    qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c8), ==, 0);
    qtest_writel(q, UART + 8, 0x12);
    g_assert_cmphex(qtest_readb(q, 0x20010000), ==, 0x12);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c8), ==, 0);
    qtest_writel(q, UART + 8, 0x34);
    g_assert_cmphex(qtest_readb(q, 0x20010001), ==, 0x34);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c8), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 8), ==, 0x20010010);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3a0), ==, 1);
    qtest_writel(q, DMA + 0x340, 1);
    qtest_writel(q, DMA + 0x40, 0x100);
    qtest_writel(q, UART + 8, 0x56);
    g_assert_cmphex(qtest_readb(q, 0x20010010), ==, 0);
    qtest_writel(q, DMA + 0x40, 0);
    g_assert_cmphex(qtest_readb(q, 0x20010010), ==, 0x56);
    qtest_writel(q, UART + 8, 0x78);
    g_assert_cmphex(qtest_readb(q, 0x20010011), ==, 0x78);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c8), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3a0), ==, 0);
    /* Cyclic descriptors remain bounded by actual available peripheral data.
     * Disabling the channel cancels a partially received next block. */
    descriptor(q, 0x20010220, 0x20010010, 0x20010200);
    qtest_writel(q, DMA + 0x10, 0x20010200);
    qtest_writel(q, DMA + 0x18, 0x18000000);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    for (unsigned i = 0; i < 9; i++) { qtest_writel(q, UART + 8, 0xa0 + i); }
    g_assert_cmphex(qtest_readb(q, 0x20010000), ==, 0xa8);
    g_assert_cmphex(qtest_readb(q, 0x20010001), ==, 0xa5);
    qtest_writel(q, DMA + 0x3a0, 0x100);
    qtest_writel(q, UART + 8, 0xff);
    g_assert_cmphex(qtest_readb(q, 0x20010001), ==, 0xa5);
    qtest_quit(q);
}
static void linked_transmit(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
                               "if=pflash,format=raw,file=%s", path);
    qtest_writel(q, UART, 0x01400003);
    qtest_writel(q, 0x20010000, 0x78563412);
    for (unsigned i = 0; i < 2; i++) {
        uint32_t addr = 0x20010200 + i * 32;
        qtest_writel(q, addr, 0x20010000 + i * 2);
        qtest_writel(q, addr + 4, UART + 8);
        qtest_writel(q, addr + 8, i ? 0 : addr + 32);
        qtest_writel(q, addr + 12, 0x18100101);
        qtest_writel(q, addr + 16, 2);
    }
    qtest_writel(q, DMA + 0x10, 0x20010200);
    qtest_writel(q, DMA + 0x18, 0x18000000);
    qtest_writel(q, DMA + 0x44, 1u << 11);
    qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0), ==, 1);
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c8), ==, 1);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(q, UART + 8), ==,
                       (0x78563412u >> (i * 8)) & 255);
    }
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("listenai-dma-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("listenai/dma/peripheral-linked-receive", linked_receive);
    qtest_add_func("listenai/dma/peripheral-linked-transmit", linked_transmit);
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
