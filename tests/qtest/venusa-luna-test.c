/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define LUNA 0x46000000
static char *flash;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    return qtest_initf("-machine venusa-rd-evb -drive "
                       "if=pflash,format=raw,file=%s", path);
}
static void control(void)
{
    QTestState *q = start();
    qtest_writel(q, LUNA + 0xc0, 0x7fff);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xc0), ==, 0x7fff);
    qtest_writel(q, LUNA + 0xd4, 0xfffffffe);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xd4), ==, 0xfffffe);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xd8), ==, 0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xdc), ==, 0);
    qtest_writel(q, LUNA + 0x100, 4);
    qtest_writel(q, LUNA + 0x1d8, UINT32_MAX);
    qtest_writel(q, LUNA + 0x1d4, UINT32_MAX);
    qtest_writel(q, LUNA + 0xe0, 0x44c0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0x100), ==, 4);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xe0), ==, 0x44c0);
    qtest_writel(q, 0x43100014, 1u << 27);
    g_assert_cmphex(qtest_readl(q, LUNA + 0x100), ==, 0);
    g_assert_cmphex(qtest_readl(q, LUNA + 0xd8), ==, 0);
    g_assert_cmpuint(qtest_readb(q, 0xe00210a8), ==, 0);
    qtest_quit(q);
}
static void reject(const void *opaque)
{
    bool scheduler = GPOINTER_TO_UINT(opaque);
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        qtest_writel(q, LUNA + (scheduler ? 0x100 : 0xc0), 1);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(scheduler ? "*LUNA task scheduler execution unsupported*" :
                             "*LUNA partial reset or completion-clear mask unsupported*");
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-luna-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("venusa/luna/control-reset", control);
    qtest_add_data_func("venusa/luna/reject-partial-reset", GUINT_TO_POINTER(0), reject);
    qtest_add_data_func("venusa/luna/reject-scheduler", GUINT_TO_POINTER(1), reject);
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
