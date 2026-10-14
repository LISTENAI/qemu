/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#define CAL 0x43400000
#define PENDING (0xe0021000 + 22 * 4)
static char *flash;
static QTestState *start(void)
{
    g_autofree char *path = g_shell_quote(flash);
    return qtest_initf("-machine venusa-rd-evb -drive "
                       "if=pflash,format=raw,file=%s", path);
}
static void load(QTestState *q, uint32_t date, uint32_t time)
{
    qtest_writel(q, CAL + 0xc, time);
    qtest_writel(q, CAL + 0x10, date);
    qtest_writel(q, CAL + 4, 1);
}
static void zero_midnight(void)
{
    QTestState *q = start();
    load(q, 0, 0x173b3b);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 0);
    qtest_clock_step(q, 999999999);
    g_assert_cmphex(qtest_readl(q, CAL + 0x14), ==, 0x173b3b);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, CAL + 0x14), ==, 0);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 0x01000001);
    load(q, 31, 0x173b3b);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 31);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 0x01000101);
    g_assert_cmphex(qtest_readl(q, CAL + 0x14), ==, 0);
    load(q, 0, 0);
    g_assert_cmphex(qtest_readl(q, CAL + 0x14), ==, 0);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 0);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 0x06000101);
    qtest_quit(q);
}
static void zero_alarm_interval(void)
{
    QTestState *q = start();
    load(q, 0, 0);
    qtest_writel(q, CAL, 1);
    qtest_writel(q, CAL + 4, 0x10000);
    qtest_writel(q, CAL + 0x1c, 1);
    qtest_writel(q, CAL + 0x20, 0);
    qtest_writel(q, CAL + 4, 0x30);
    qtest_clock_step(q, 999999999);
    g_assert_cmphex(qtest_readl(q, CAL + 8) & 3, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, CAL + 0x14), ==, 1);
    g_assert_cmphex(qtest_readl(q, CAL + 8) & 3, ==, 3);
    g_assert_cmpuint(qtest_readb(q, PENDING), ==, 1);
    qtest_writel(q, CAL + 4, 0x100);
    g_assert_cmpuint(qtest_readb(q, PENDING), ==, 1);
    qtest_writel(q, CAL + 4, 0x200);
    g_assert_cmpuint(qtest_readb(q, PENDING), ==, 0);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readl(q, CAL + 8) & 3, ==, 1);
    qtest_quit(q);
}
static void civil_date(void)
{
    QTestState *q = start();
    load(q, 0x0318021c, 0x173b3b);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readl(q, CAL + 0x18), ==, 0x0418021d);
    g_assert_cmphex(qtest_readl(q, CAL + 0x14), ==, 0);
    qtest_quit(q);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    int fd = g_file_open_tmp("venusa-calendar-flash-XXXXXX", &flash, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * 1024 * 1024), ==, 0);
    close(fd);
    qtest_add_func("venusa/calendar/zero-midnight", zero_midnight);
    qtest_add_func("venusa/calendar/zero-alarm-interval", zero_alarm_interval);
    qtest_add_func("venusa/calendar/civil-date", civil_date);
    int result = g_test_run();
    unlink(flash); g_free(flash);
    return result;
}
