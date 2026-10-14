/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#define DMA 0x40000000
#define DEST 0x20010000
static char *flash;
typedef struct UARTBoard { const char *machine; uint32_t uart, request; } UARTBoard;
static const UARTBoard boards[] = {
    { "arcs-mini", 0x46a00000, 8 },
    { "venusa-rd-evb", 0x43800000, 0 },
};
static QTestState *open_uart(const UARTBoard *b)
{
    g_autofree char *path = g_shell_quote(flash);
    g_autofree char *drive = b->request ? g_strdup("") :
        g_strdup_printf("-drive if=pflash,format=raw,file=%s", path);
    QTestState *q = qtest_initf("-machine %s %s", b->machine, drive);
    qtest_writel(q, b->uart, 0x01400003);
    qtest_writel(q, b->uart + 12, 0xc0);
    qtest_writel(q, DMA + 0x398, 1);
    return q;
}
static void start(QTestState *q, const UARTBoard *b, unsigned count)
{
    qtest_writel(q, DMA, b->uart + 8);
    qtest_writel(q, DMA + 8, DEST);
    qtest_writel(q, DMA + 0x18, 0x200401);
    qtest_writel(q, DMA + 0x1c, count);
    qtest_writel(q, DMA + 0x44, b->request << 7);
    qtest_writel(q, DMA + 0x3a0, 0x101);
}
static void threshold(const void *opaque)
{
    const UARTBoard *b = opaque;
    for (unsigned threshold = 0; threshold <= 2; threshold++) {
        QTestState *q = open_uart(b);
        qtest_writel(q, b->uart + 20, threshold);
        start(q, b, 32);
        for (unsigned i = 0; i < 32; i++) { qtest_writel(q, b->uart + 8, i); }
        g_assert_cmphex(qtest_readl(q, b->uart + 4) & 127, ==, threshold);
        g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==,
                       threshold ? 32 - threshold : 0x100020);
        for (unsigned i = 0; i < 32 - threshold; i++) {
            g_assert_cmphex(qtest_readb(q, DEST + i), ==, i);
        }
        qtest_clock_step(q, 1000000);
        uint32_t cause = qtest_readl(q, b->uart + 16);
        g_assert_cmphex(cause & 0x00c000c0, ==,
                       threshold ? 0x00800080 : 0x00400040);
        if (threshold) {
            /* Reducing a live threshold drains the real tail, completes DMA,
             * and leaves an already-latched timeout for software W1C. */
            qtest_writel(q, b->uart + 20, 0);
            g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 0x100020);
            g_assert_cmphex(qtest_readl(q, b->uart + 16) & 0xc000c0, ==, 0xc000c0);
        }
        qtest_writel(q, b->uart + 16, 0xc0);
        g_assert_cmphex(qtest_readl(q, b->uart + 16), ==, 0x40000);
        start(q, b, 32);
        qtest_clock_step(q, 10000000);
        g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 0);
        g_assert_cmphex(qtest_readl(q, b->uart + 16), ==, 0x40000);
        qtest_writel(q, b->uart + 24, 64);
        qtest_writel(q, b->uart, 0x01000003);
        qtest_writel(q, b->uart, 0x01400003);
        qtest_writel(q, b->uart + 16, 0x80);
        qtest_writel(q, b->uart + 8, 0xa5);
        qtest_clock_step(q, 999999);
        g_assert_cmphex(qtest_readl(q, b->uart + 16), ==, 0x40000);
        qtest_clock_step(q, 1);
        /* VenusA requires a queued byte for DMA idle; its empty partial
         * transfer is indistinguishable before and after a previous Done. */
        g_assert_cmphex(qtest_readl(q, b->uart + 16), ==,
                       b->request ? 0x840080 : 0x40000);
        g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 1);
        g_assert_cmphex(qtest_readb(q, DEST), ==, 0xa5);
        qtest_writel(q, DMA + 0x3a0, 0x100);
        qtest_writel(q, b->request ? 0x4600000c : 0x43100014, 1);
        qtest_writel(q, b->uart, 0x01400003);
        qtest_writel(q, b->uart + 12, 0x80);
        qtest_writel(q, DMA + 0x398, 1);
        start(q, b, 32);
        qtest_writel(q, b->uart + 8, 0x5a);
        qtest_clock_step(q, 999999);
        g_assert_cmphex(qtest_readl(q, b->uart + 16), ==, 0x40000);
        qtest_clock_step(q, 1);
        g_assert_cmphex(qtest_readl(q, b->uart + 16), ==,
                       b->request ? 0x840080 : 0x40000);
        g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 1);
        g_assert_cmphex(qtest_readb(q, DEST), ==, 0x5a);
        qtest_quit(q);
    }
}
static void tail_after_done(void)
{
    const UARTBoard *b = &boards[1];
    QTestState *q = open_uart(b);
    qtest_writel(q, b->uart + 20, 1);
    start(q, b, 32);
    for (unsigned i = 0; i < 33; i++) { qtest_writel(q, b->uart + 8, i); }
    g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 0x100020);
    g_assert_cmphex(qtest_readl(q, b->uart + 4) & 127, ==, 1);
    start(q, b, 32);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, b->uart + 16) & 0xc000c0, ==, 0x400040);
    /* The tail predates Done. Only later received bytes qualify a new idle. */
    for (unsigned i = 33; i < 39; i++) { qtest_writel(q, b->uart + 8, i); }
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, b->uart + 16) & 0xc000c0, ==, 0xc000c0);
    g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 6);
    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(qtest_readb(q, DEST + i), ==, 32 + i);
    }
    g_assert_cmphex(qtest_readl(q, b->uart + 8), ==, 38);
    qtest_quit(q);
}

static void mux_and_cancel(void)
{
    UARTBoard b = { "venusa-rd-evb", 0x43900000, 6 };
    g_autofree char *path = g_shell_quote(flash);
    QTestState *q = qtest_initf("-machine venusa-rd-evb -drive "
                               "if=pflash,format=raw,file=%s", path);
    qtest_writel(q, b.uart, 0x01400003);
    qtest_writel(q, DMA + 0x398, 1);
    start(q, &b, 64);
    qtest_writel(q, b.uart + 20, 63);
    for (unsigned i = 0; i < 64; i++) { qtest_writel(q, b.uart + 8, i); }
    g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 0);
    /* Request 6 selects UART1 only when its SoC mux is enabled. */
    qtest_writel(q, 0x431000a0, 1u << 6);
    g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 1);
    g_assert_cmphex(qtest_readl(q, b.uart + 4) & 127, ==, 63);
    qtest_writel(q, b.uart + 20, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 0x1c), ==, 0x100040);
    g_assert_cmphex(qtest_readl(q, b.uart + 16) & 0x400000, ==, 0x400000);
    qtest_writel(q, b.uart + 16, 0x40);
    start(q, &b, 32);
    qtest_writel(q, b.uart + 8, 0x80);
    qtest_writel(q, DMA + 0x3a0, 0x100);
    /* Aborting a partial transfer does not fabricate a DMA Done pulse. */
    g_assert_cmphex(qtest_readl(q, b.uart + 16) & 0x400000, ==, 0);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, b.uart + 16), ==, 0);
    g_assert_cmphex(qtest_readl(q, DMA + 0x3a0), ==, 0);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_setenv("ARCS_QEMU_BUDGET_NS", "1000000000", true);
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("listenai-uart-dma-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    for (unsigned i = 0; i < G_N_ELEMENTS(boards); i++) {
        g_autofree char *name = g_strdup_printf("listenai/uart/%s/dma", boards[i].machine);
        qtest_add_data_func(name, &boards[i], threshold);
    }
    qtest_add_func("listenai/uart/venusa-rd-evb/mux-cancel", mux_and_cancel);
    qtest_add_func("listenai/uart/venusa-rd-evb/tail-after-done", tail_after_done);
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
