/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define ECLIC_BASE 0xe0020000
#define IRQ_BASE (ECLIC_BASE + 0x1000)

static char *flash_path;

static void test_parameters_and_reset(void)
{
    g_autofree char *flash_arg = g_shell_quote(flash_path);
    QTestState *qts = qtest_initf("-machine venusa-rd-evb "
                                "-drive if=pflash,format=raw,file=%s", flash_arg);

    g_assert_cmphex(qtest_readl(qts, ECLIC_BASE + 4), ==, 0x00602053);
    g_assert_cmphex(qtest_readb(qts, ECLIC_BASE), ==, 7);
    g_assert_cmphex(qtest_readb(qts, IRQ_BASE + 82 * 4 + 3), ==, 0x1f);
    qtest_writeb(qts, IRQ_BASE + 82 * 4 + 3, 0x80);
    g_assert_cmphex(qtest_readb(qts, IRQ_BASE + 82 * 4 + 3), ==, 0x9f);
    qtest_writeb(qts, IRQ_BASE + 82 * 4 + 1, 0xff);
    g_assert_cmphex(qtest_readb(qts, IRQ_BASE + 82 * 4 + 1), ==, 1);
    qtest_writeb(qts, ECLIC_BASE + 11, 0xff);
    g_assert_cmphex(qtest_readb(qts, ECLIC_BASE + 11), ==, 0xff);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, ECLIC_BASE + 4), ==, 0x00602053);
    g_assert_cmphex(qtest_readb(qts, ECLIC_BASE), ==, 7);
    g_assert_cmphex(qtest_readb(qts, ECLIC_BASE + 11), ==, 0);
    g_assert_cmphex(qtest_readb(qts, IRQ_BASE + 82 * 4 + 1), ==, 0);
    g_assert_cmphex(qtest_readb(qts, IRQ_BASE + 82 * 4 + 3), ==, 0x1f);
    qtest_quit(qts);
}

static void create_image(const char *pattern, char **path, size_t size)
{
    g_autofree uint8_t *data = g_malloc0(size);
    int fd = g_file_open_tmp(pattern, path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, data, size), ==, size);
    close(fd);
}

int main(int argc, char **argv)
{
    int result;
    g_test_init(&argc, &argv, NULL);
    create_image("listenai-eclic-flash-XXXXXX", &flash_path, 16 * 1024 * 1024);
    qtest_add_func("listenai/eclic/parameters-reset", test_parameters_and_reset);
    result = g_test_run();
    unlink(flash_path);
    g_free(flash_path);
    return result;
}
