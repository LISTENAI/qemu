/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#define APC 0x44700000
#define CLK 0x43100034
#define IRQ(i) (0xe0021000 + (i) * 4)
static char *flash;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
                               "if=pflash,format=raw,file=%s", path);
    qtest_writel(q, CLK, qtest_readl(q, CLK) | 0xc600);
    qtest_writel(q, APC, 3);
    return q;
}
static void loopback(void)
{
    QTestState *q = start();
    qtest_writel(q, APC + 0x10, 0x02010001);
    qtest_writel(q, APC + 0x1c, 0x02010001);
    qtest_writel(q, APC + 0x38, 0x22331122);
    g_assert_cmphex(qtest_readl(q, APC + 0x94), ==, 0x411);
    g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0x100010);
    qtest_writel(q, APC + 0x28, (30u << 16) | (1u << 26));
    qtest_writel(q, APC + 0x24, (1u << 31) | (1u << 21) | (1u << 13) | 4 | (9u << 5));
    qtest_clock_step(q, 62499);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0x100010);
    g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0x22331122);
    qtest_writel(q, APC + 0x38, 0x66775544);
    qtest_clock_step(q, 12500);
    qtest_writel(q, CLK, qtest_readl(q, CLK) & ~(1u << 15));
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x10) & 0x1f001f0, ==, 0x100010);
    qtest_writel(q, CLK, qtest_readl(q, CLK) | (1u << 15));
    qtest_clock_step(q, 49999);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c) & 0x1f001f0, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0x66775544);
    qtest_quit(q);
}
static void initial_fifo_wait(void)
{
    QTestState *q = start();
    qtest_writel(q, APC + 0x10, 0x02010001);
    qtest_writel(q, APC + 0x1c, 0x02010001);
    qtest_writel(q, APC + 0x28, (30u << 16) | (1u << 26));
    qtest_writel(q, APC + 0x24, 0x80202124);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c), ==, 0x02010001);
    qtest_writel(q, APC + 0x38, 0x12345678);
    qtest_clock_step(q, 62499);
    g_assert_cmphex(qtest_readl(q, APC + 0x1c), ==, 0x02010001);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0x12345678);
    qtest_clock_step(q, 62500); /* Started clocks continue through underrun. */
    g_assert_cmphex(qtest_readl(q, APC + 0x1c), ==, 0x02110011);
    g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0);
    qtest_quit(q);
}

static void serial_alignment(void)
{
    static const uint32_t options[] = { 0, 1u << 13,
        (1u << 13) | (1u << 20), (1u << 13) | (1u << 17) };
    static const uint32_t results[] = { 0x11190891, 0x22331122,
                                        0x22331122, 0x44662244 };
    for (unsigned j = 0; j < ARRAY_SIZE(options); j++) {
        QTestState *q = start();
        qtest_writel(q, APC + 0x10, 0x02010001);
        qtest_writel(q, APC + 0x1c, 0x02010001);
        for (unsigned i = 0; i < 16; i++) {
            qtest_writel(q, APC + 0x38, 0x22331122);
        }
        qtest_writel(q, APC + 0x28, (30u << 16) | (1u << 26));
        qtest_writel(q, APC + 0x24, 0x80200124 | options[j]);
        qtest_clock_step(q, 750000);
        g_assert_cmphex(qtest_readl(q, APC + 0x10), ==, 0x02410041);
        g_assert_cmphex(qtest_readl(q, APC + 0x1c), ==, 0x02c100c1);
        g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, results[j]);
        qtest_quit(q);
    }
}
static void interrupts(void)
{
    QTestState *q = start();
    qtest_writel(q, APC + 0x0c, 1);
    g_assert_cmphex(qtest_readl(q, APC + 0x90) & 17, ==, 17);
    qtest_writel(q, APC + 0x60, ~1u);
    g_assert_cmpuint(qtest_readb(q, IRQ(40)), ==, 1);
    g_assert_cmpuint(qtest_readb(q, IRQ(41)), ==, 0);
    qtest_writel(q, APC + 0xc0, ~1u);
    g_assert_cmpuint(qtest_readb(q, IRQ(41)), ==, 1);
    qtest_writel(q, APC + 0x78, 1);
    g_assert_cmpuint(qtest_readb(q, IRQ(40)), ==, 0);
    g_assert_cmpuint(qtest_readb(q, IRQ(41)), ==, 1);
    qtest_writel(q, APC + 0xd8, 1);
    g_assert_cmpuint(qtest_readb(q, IRQ(41)), ==, 0);
    for (unsigned i = 0; i < 16; i++) { qtest_writel(q, APC + 0x34, i); }
    g_assert_cmphex(qtest_readl(q, APC + 0x0c) & 0x1f0, ==, 0x100);
    qtest_writel(q, APC + 0x34, 99);
    g_assert_cmphex(qtest_readl(q, APC + 0x90) & 10, ==, 10);
    qtest_writel(q, APC + 0x0c, 9);
    g_assert_cmphex(qtest_readl(q, APC + 0x0c), ==, 1);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, APC + 0x90), ==, 0);
    qtest_quit(q);
}
static void linked_audio_format(bool wide)
{
    QTestState *q = start();
    const uint32_t dma = 0x40000000, src = 0x20010000, dst = 0x20011000;
    uint32_t format = wide ? 0x02050005 : 0x02010001;
    qtest_writel(q, APC + 0x10, format);
    qtest_writel(q, APC + 0x1c, format);
    qtest_writel(q, 0x431000a0, (1u << 9) | (1u << 14));
    for (unsigned i = 0; i < 48; i++) {
        qtest_writel(q, src + i * 4, 0x10000000 + i * 0x00010001);
        qtest_writel(q, dst + i * 4, 0xdeadbeef);
    }
    for (unsigned dir = 0; dir < 2; dir++) {
        uint32_t desc = 0x20012000 + dir * 0x100;
        uint32_t ctl = dir ? 0x18200425 : 0x18100125;
        for (unsigned block = 0; block < 2; block++) {
            uint32_t d = desc + 32 * block;
            qtest_writel(q, d, dir ? APC + 0x50 : src + block * 96);
            qtest_writel(q, d + 4, dir ? dst + block * 96 : APC + 0x38);
            qtest_writel(q, d + 8, block ? 0 : d + 32);
            qtest_writel(q, d + 12, ctl);
            qtest_writel(q, d + 16, 24);
        }
        uint32_t reg = dma + dir * 0x58;
        qtest_writel(q, reg + 0x10, desc);
        qtest_writel(q, reg + 0x18, 0x18000000);
        qtest_writel(q, reg + 0x44, dir ? 14u << 7 : 9u << 11);
    }
    qtest_writel(q, dma + 0x398, 1);
    qtest_writel(q, dma + 0x3a0, 0x303);
    g_assert_cmphex(qtest_readl(q, dma + 0x1c), ==, wide ? 8 : 16);
    g_assert_cmphex(qtest_readl(q, dma + 0x2c0), ==, 0);
    qtest_writel(q, APC + 0x28, (30u << 16) | (1u << 26));
    qtest_writel(q, APC + 0x24, wide ? 0x80202d24 : 0x80202124);
    for (unsigned i = 0; i < (wide ? 24 : 48); i++) {
        qtest_clock_step(q, wide ? 102500 : 62500);
    }
    g_assert_cmphex(qtest_readl(q, dma + 0x2c0), ==, 3);
    g_assert_cmphex(qtest_readl(q, dma + 0x3a0), ==, 0);
    for (unsigned i = 0; i < 48; i++) {
        g_assert_cmphex(qtest_readl(q, dst + i * 4), ==,
                       0x10000000 + i * 0x00010001);
    }
    qtest_quit(q);
}

static void linked_audio(void) { linked_audio_format(false); }
static void linked_audio_wide(void) { linked_audio_format(true); }

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-audio-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("venusa/audio/loopback-clock", loopback);
    qtest_add_func("venusa/audio/fifo-interrupts", interrupts);
    qtest_add_func("venusa/audio/serial-alignment", serial_alignment);
    qtest_add_func("venusa/audio/linked-dma-loopback", linked_audio);
    qtest_add_func("venusa/audio/linked-dma-wide-stereo", linked_audio_wide);
    qtest_add_func("venusa/audio/initial-fifo-wait", initial_fifo_wait);
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
