/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
static char *flash;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
        "if=pflash,format=raw,file=%s "
        "-device at24c-eeprom,bus=venusa-i2c0,address=0x50,rom-size=256 "
        "-device at24c-eeprom,bus=venusa-i2c1,address=0x50,rom-size=256", path);
    qtest_writel(q, 0x43100034, 3);
    return q;
}
static void command(QTestState *q, uint32_t base, uint32_t control)
{
    qtest_writel(q, base + 0x18, 0x3f8);
    qtest_writel(q, base + 0x24, control);
    qtest_writel(q, base + 0x28, 1);
}
static void eeprom(void)
{
    QTestState *q = start();
    for (unsigned n = 0; n < 2; n++) {
        uint32_t base = 0x43b00000 + n * 0x100000;
        uint32_t irq = 0xe0021000 + (29 + n) * 4;
        const uint8_t values[] = { 0x10, 0x55, 0xaa, 0x12, 0x34 };
        qtest_writel(q, base + 0x2c, 5);
        qtest_writel(q, base + 0x1c, 0x50);
        qtest_writel(q, base + 0x14, 0x208);
        for (unsigned i = 0; i < sizeof(values); i++) {
            qtest_writel(q, base + 0x20, values[i]);
        }
        command(q, base, 0x1e05);
        qtest_clock_step(q, 2999);
        g_assert_cmphex(qtest_readl(q, base + 0x18) & 0x208, ==, 0);
        qtest_clock_step(q, 1);
        g_assert_cmphex(qtest_readl(q, base + 0x18) & 0x408, ==, 0x408);
        g_assert_cmpuint(qtest_readb(q, irq), ==, 1);
        qtest_writel(q, base + 0x18, 8);
        g_assert_cmpuint(qtest_readb(q, irq), ==, 0);
        qtest_clock_step(q, 15000);
        g_assert_cmphex(qtest_readl(q, base + 0x18) & 0x220, ==, 0x220);
        qtest_writel(q, base + 0x20, 0x10);
        command(q, base, 0x1c01); /* Keep the bus for a repeated START. */
        qtest_clock_step(q, 6000);
        g_assert_cmphex(qtest_readl(q, base + 0x18) & 0xa00, ==, 0xa00);
        command(q, base, 0x1f04);
        qtest_clock_step(q, 15000);
        for (unsigned i = 1; i < sizeof(values); i++) {
            g_assert_cmphex(qtest_readl(q, base + 0x20), ==, values[i]);
        }
        g_assert_cmphex(qtest_readl(q, base + 0x18) & 0xa00, ==, 0x200);
        /* An absent address must not ACK or produce fabricated EEPROM data. */
        qtest_writel(q, base + 0x1c, 0x51);
        command(q, base, 0x1a00);
        qtest_clock_step(q, 3000);
        g_assert_cmphex(qtest_readl(q, base + 0x18) & 0x608, ==, 0x200);
    }
    qtest_quit(q);
}
static void fifo_profile(void)
{
    QTestState *q = start();
    for (unsigned n = 0; n < 2; n++) {
        uint32_t base = 0x43b00000 + n * 0x100000;
        g_assert_cmphex(qtest_readl(q, base), ==, 0x02021011);
        g_assert_cmphex(qtest_readl(q, base + 0x10), ==, 3);
        for (unsigned rx = 0; rx < 2; rx++) {
            qtest_writel(q, base + 0x28, 4);
            qtest_writel(q, base + 0x24, rx ? 0x100 : 0);
            for (unsigned count = 0; count <= 16; count++) {
                uint32_t status = qtest_readl(q, base + 0x18);
                g_assert_cmpuint(!!(status & 1), ==, count == 0);
                g_assert_cmpuint(!!(status & 2), ==, count == 16);
                g_assert_cmpuint(!!(status & 4), ==,
                                 rx ? count >= 8 : count <= 8);
                if (count < 16) { qtest_writel(q, base + 0x20, count); }
            }
            for (unsigned count = 0; count < 16; count++) {
                g_assert_cmpuint(qtest_readl(q, base + 0x20), ==, count);
            }
        }
        qtest_writel(q, base + 0x28, 5);
        g_assert_cmphex(qtest_readl(q, base + 0x18) & 3, ==, 1);
    }
    qtest_quit(q);
}

static void receive_backpressure(void)
{
    QTestState *q = start();
    uint32_t base = 0x43b00000;
    qtest_writel(q, base + 0x2c, 5);
    qtest_writel(q, base + 0x1c, 0x50);
    command(q, base, 0x1f14); /* Address plus twenty EEPROM bytes. */
    qtest_clock_step(q, 17 * 3000);
    g_assert_cmphex(qtest_readl(q, base + 0x18) & 0x202, ==, 2);
    g_assert_cmpuint(qtest_readl(q, base + 0x24) & 255, ==, 4);
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, base + 0x24) & 255, ==, 4);
    for (unsigned i = 0; i < 4; i++) { qtest_readl(q, base + 0x20); }
    qtest_clock_step(q, 12000);
    g_assert_cmphex(qtest_readl(q, base + 0x18) & 0x202, ==, 0x202);
    qtest_writel(q, base + 0x28, 5);
    command(q, base, 0x1f14);
    qtest_clock_step(q, 6000);
    qtest_writel(q, base + 0x28, 5);
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, base + 0x18) & 0x203, ==, 1);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-i2c-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("venusa/i2c/eeprom-repeated-start-nack", eeprom);
    qtest_add_func("venusa/i2c/fifo-profile", fifo_profile);
    qtest_add_func("venusa/i2c/receive-backpressure-reset", receive_backpressure);
    int result = g_test_run();
    unlink(flash); g_free(flash);
    return result;
}
