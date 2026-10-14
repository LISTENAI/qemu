/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#define BASE 0x42000800
#define FIFO 0x42001000
#define DMA 0x40000000
static char *flash, *pixels;
static QTestState *start(bool source)
{
    g_autofree char *f = g_shell_quote(flash);
    g_autofree char *p = g_shell_quote(pixels);
    g_autofree char *extra = source ? g_strdup_printf(" -device listenai-video-source,sink=/machine/soc,file=%s,width=4,height=4,pixel-hz=1000000,frames=1,delay-ns=1000000,period-ns=1000000", p) : g_strdup("");
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s%s", f, extra);
    qtest_writel(q, 0x43100034, 0x800);
    return q;
}
static void pin(QTestState *q, unsigned n, unsigned value)
{ qtest_set_irq_in(q, "/machine/soc", "dvp-input", n, value); }
static void byte(QTestState *q, unsigned value)
{ pin(q, 3, value); pin(q, 2, 1); pin(q, 2, 0); }
static void begin(QTestState *q)
{ pin(q, 0, 1); byte(q, 0); pin(q, 0, 0); byte(q, 0); pin(q, 1, 1); }
static void configure(QTestState *q, unsigned width, unsigned height)
{
    qtest_writel(q, BASE, width); qtest_writel(q, BASE + 4, height);
    qtest_writel(q, BASE + 0x10, 2); qtest_writel(q, BASE + 0x24, 0xfff);
    qtest_writel(q, BASE + 0x1c, 1);
}
static void receive(void)
{
    QTestState *q = start(false); configure(q, 4, 1);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_writel(q, BASE + 0x24, 0x10);
    begin(q);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x90);
    for (unsigned i = 0; i < 8; i++) { byte(q, 0x10 + i); }
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x13121110);
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x17161514);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x200, ==, 0x200);
    g_assert_cmpint(qtest_readb(q, 0xe0021000 + 57 * 4), ==, 1);
    qtest_writel(q, BASE + 0x28, 0xfff);
    g_assert_cmpint(qtest_readb(q, 0xe0021000 + 57 * 4), ==, 0);
    g_assert_cmphex(qtest_readl(q, BASE + 0x28), ==, 0);
    qtest_quit(q);
}
static void overflow_reset(void)
{
    QTestState *q = start(false); configure(q, 130, 1); begin(q);
    for (unsigned i = 0; i < 260; i++) { byte(q, i & 255); }
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x28, ==, 0x28);
    for (unsigned i = 0; i < 64; i++) {
        uint32_t expected = (i * 4) | ((i * 4 + 1) << 8) | ((i * 4 + 2) << 16) | ((i * 4 + 3) << 24);
        g_assert_cmphex(qtest_readl(q, FIFO), ==, expected);
    }
    qtest_writel(q, 0x43100014, 1u << 21);
    g_assert_cmphex(qtest_readl(q, BASE + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_quit(q);
}
static void dma_source(void)
{
    QTestState *q = start(true);
    qtest_writel(q, 0x431000a0, (1u << 3) | (1u << 15) | (1u << 19));
    qtest_writel(q, DMA + 0x398, 1);
    qtest_writel(q, DMA, FIFO); qtest_writel(q, DMA + 8, 0x20030000);
    qtest_writel(q, DMA + 0x18, 1 | (2 << 1) | (2 << 4) | (2 << 9) | (2 << 20));
    qtest_writel(q, DMA + 0x1c, 8);
    qtest_writel(q, DMA + 0x44, 3 << 7);
    qtest_writel(q, DMA + 0x3a0, 0x101);
    qtest_writel(q, BASE + 0x20, 0x101);
    configure(q, 4, 4);
    qtest_clock_step(q, 1005999);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, 0x20030000), ==, 0x83828180);
    qtest_clock_step(q, 40000);
    for (unsigned i = 0; i < 8; i++) {
        uint32_t a = 0x80 + i * 4;
        g_assert_cmphex(qtest_readl(q, 0x20030000 + i * 4), ==, a | ((a+1)<<8) | ((a+2)<<16) | ((a+3)<<24));
    }
    g_assert_cmphex(qtest_readl(q, DMA + 0x2c0) & 1, ==, 1);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34) & 0x240, ==, 0x240);
    qtest_quit(q);
}
static void crop_gate_cancel(void)
{
    QTestState *q = start(false);
    qtest_writel(q, BASE + 8, 1); qtest_writel(q, BASE + 0xc, 1);
    configure(q, 2, 1);
    begin(q);
    for (unsigned i = 0; i < 8; i++) { byte(q, i); }
    pin(q, 1, 0); byte(q, 0); pin(q, 1, 1);
    for (unsigned i = 0; i < 8; i++) { byte(q, 0x20 + i); }
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0x25242322);
    qtest_writel(q, BASE + 0x14, 1);
    g_assert_cmphex(qtest_readl(q, BASE), ==, 2);
    g_assert_cmphex(qtest_readl(q, BASE + 4), ==, 1);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_writel(q, 0x43100034, 0);
    begin(q); for (unsigned i = 0; i < 8; i++) { byte(q, 0x50 + i); }
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_writel(q, 0x43100034, 0x800);
    qtest_writel(q, BASE + 0x1c, 0);
    begin(q); for (unsigned i = 0; i < 8; i++) { byte(q, 0x70 + i); }
    g_assert_cmphex(qtest_readl(q, FIFO), ==, 0);
    g_assert_cmphex(qtest_readl(q, BASE + 0x34), ==, 0x10);
    qtest_quit(q);
}
static void rejected(const void *opaque)
{
    uint32_t offset = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start(false); qtest_set_expected_status(q, 1);
        qtest_writel(q, BASE + offset, offset == 0x18 ? 3 : 0x80000000);
        qtest_quit(q); return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*VenusA unsupported write*address=0x420008*");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-dvp-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0); g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0); close(fd);
    fd = g_file_open_tmp("venusa-dvp-pixels-XXXXXX", &pixels, NULL);
    g_assert_cmpint(fd, >=, 0);
    uint8_t data[32]; for (unsigned i = 0; i < 32; i++) { data[i] = 0x80 + i; }
    g_assert_cmpint(write(fd, data, sizeof(data)), ==, sizeof(data)); close(fd);
    qtest_add_func("venusa/dvp/receive", receive);
    qtest_add_func("venusa/dvp/overflow-reset", overflow_reset);
    qtest_add_func("venusa/dvp/dma-source", dma_source);
    qtest_add_func("venusa/dvp/crop-gate-cancel", crop_gate_cancel);
    qtest_add_data_func("venusa/dvp/reject/format", GUINT_TO_POINTER(0x18), rejected);
    qtest_add_data_func("venusa/dvp/reject/control", GUINT_TO_POINTER(0x10), rejected);
    int result = g_test_run(); unlink(flash); unlink(pixels); g_free(flash); g_free(pixels); return result;
}
