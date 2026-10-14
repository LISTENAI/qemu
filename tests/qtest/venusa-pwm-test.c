/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define GPT 0x44000000
#define CTRL (GPT + 0xc)
#define PENDING (GPT + 0x84)
#define CYCLE (1u << 13)
#define PAD14 (0x44100000 + 14 * 4)
static char *flash_path, *otp_path;

static QTestState *start(void)
{
    QTestState *q = qtest_initf("-machine venusa-rd-evb "
        "-drive if=pflash,format=raw,file=%s", flash_path);
    qtest_irq_intercept_out_named(q, "/machine/soc", "pad-out");
    qtest_writel(q, 0x43100028, 0x40);
    qtest_writel(q, 0x43100014, 0x200);
    qtest_writel(q, PAD14, 11);
    qtest_writel(q, GPT + 4, 0x0180095f); /* 10 kHz T0 / 2400. */
    qtest_writel(q, CTRL, 0x01000023);
    qtest_writel(q, GPT + 0x14, 8); /* 10 ticks = 1 ms. */
    qtest_writew(q, GPT + 0x24, 2); /* Three high ticks. */
    qtest_writel(q, GPT + 0xa0, 4);
    qtest_writel(q, CTRL, 0x01008023);
    qtest_writel(q, GPT + 0x1c, 0x04000001);
    return q;
}

static void level(QTestState *q, bool expected)
{
    g_assert_cmpint(qtest_get_irq(q, 14), ==, expected);
}

static void waveform(void)
{
    QTestState *q = start();
    level(q, false);
    qtest_clock_step(q, 699999);
    level(q, false);
    qtest_clock_step(q, 1);
    level(q, true);
    qtest_clock_step(q, 299999);
    level(q, true);
    qtest_clock_step(q, 1);
    level(q, false);
    g_assert_cmphex(qtest_readl(q, PENDING), ==, CYCLE);
    g_assert_cmphex(qtest_readl(q, GPT + 0x80) >> 16, ==, 0);
    qtest_writel(q, GPT + 0x80, 0x7bf0 & ~CYCLE);
    g_assert_cmphex(qtest_readl(q, GPT + 0x80) >> 16, ==, CYCLE);
    qtest_writel(q, PENDING, CYCLE << 16);
    g_assert_cmphex(qtest_readl(q, PENDING), ==, 0);
    qtest_writel(q, CTRL, 0x01100023); /* High first, same high duration. */
    level(q, true);
    qtest_clock_step(q, 300000);
    level(q, false);
    qtest_clock_step(q, 700000);
    level(q, true);
    qtest_quit(q);
}

static void shadow_and_gate(void)
{
    QTestState *q = start();
    qtest_clock_step(q, 100000);
    qtest_writel(q, GPT + 0x24, 5);
    qtest_writel(q, GPT + 0xa0, 1);
    qtest_clock_step(q, 599999);
    level(q, false);
    qtest_clock_step(q, 1);
    level(q, true); /* Old 30% cycle still in progress. */
    qtest_clock_step(q, 300000);
    level(q, false);
    qtest_clock_step(q, 399999);
    level(q, false);
    qtest_clock_step(q, 1);
    level(q, true); /* New 60% cycle. */
    qtest_writel(q, 0x43100028, 0);
    qtest_clock_step(q, 123456789);
    level(q, true);
    qtest_writel(q, 0x43100028, 0x40);
    qtest_clock_step(q, 599999);
    level(q, true);
    qtest_clock_step(q, 1);
    level(q, false);
    qtest_writel(q, CTRL, 0x01014023); /* Stop with idle high. */
    level(q, true);
    qtest_clock_step(q, 10000000);
    level(q, true);
    qtest_writel(q, 0x43100014, 0x200);
    level(q, false);
    qtest_clock_step(q, 10000000);
    level(q, false);
    g_assert_cmphex(qtest_readl(q, PENDING), ==, 0);
    qtest_quit(q);
}

static void pinmux(void)
{
    QTestState *q = start();
    qtest_clock_step(q, 700000);
    level(q, true);
    qtest_writel(q, PAD14, 0);
    level(q, false);
    qtest_writel(q, PAD14, 11);
    level(q, true);
    qtest_clock_step(q, 300000);
    level(q, false);
    qtest_quit(q);
}

static void gated_shadow(void)
{
    QTestState *q = start();
    qtest_clock_step(q, 450000);
    qtest_writel(q, 0x43100028, 0);
    qtest_writew(q, GPT + 0x24, 5);
    qtest_writel(q, GPT + 0xa0, 1);
    level(q, false); /* Deferred update cannot change a stopped clock's phase. */
    qtest_clock_step(q, 1000000000);
    level(q, false);
    qtest_writel(q, 0x43100028, 0x40);
    qtest_clock_step(q, 249999);
    level(q, false);
    qtest_clock_step(q, 1);
    level(q, true);
    qtest_clock_step(q, 300000);
    level(q, false);
    qtest_clock_step(q, 400000);
    level(q, true);
    qtest_quit(q);
}

static void second_channel(void)
{
    QTestState *q = start();
    qtest_writel(q, GPT + 8, 0x018004af); /* Independent 20 kHz. */
    qtest_writel(q, GPT + 0x10, 0x0f000023);
    qtest_writew(q, GPT + 0x18, 18); /* Also 1 ms. */
    qtest_writel(q, GPT + 0x40, 0x00070003);
    qtest_writew(q, GPT + 0x44, 11);
    qtest_writew(q, GPT + 0x46, 15);
    g_assert_cmphex(qtest_readl(q, GPT + 0x44), ==, 0x000f000b);
    qtest_writel(q, GPT + 0xa0, 8);
    qtest_writel(q, GPT + 0x10, 0x0f008023);
    qtest_writel(q, GPT + 0x1c, 0x0c000002);
    qtest_clock_step(q, 999999);
    g_assert_cmphex(qtest_readl(q, PENDING) & (CYCLE | (CYCLE << 1)), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, PENDING), ==, CYCLE | (CYCLE << 1));
    qtest_writel(q, GPT + 0x10, 0x0f004023);
    qtest_writel(q, PENDING, (CYCLE | (CYCLE << 1)) << 16);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, PENDING), ==, CYCLE);
    qtest_quit(q);
}

typedef struct Rejected { const char *name; unsigned offset; uint32_t value; } Rejected;
static const Rejected rejected[] = {
    { "phase-delay", 0x30, 1 },
    { "start-delay", 0x2c, 1 },
    { "repeat", 0x90, 1 },
    { "threshold", 0x94, 1 },
    { "synchronized", 0x1c, 0x14000000 },
    { "saturated-duty", 0x24, 9 },
    { "count-direction", 0x1c, 0x04000041 },
    { "threshold-interrupt", 0x80, 0x7be0 },
};
static void reject(const void *opaque)
{
    const Rejected *r = opaque;
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        qtest_writel(q, GPT + r->offset, r->value);
        if (r->offset == 0x24) { qtest_writel(q, GPT + 0xa0, 4); }
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_autofree char *message = g_strdup_printf(
        "*VenusA unsupported write*address=0x%08x size=4*", GPT + r->offset);
    g_test_trap_assert_stderr(message);
}

static void image(const char *pattern, char **path, off_t length)
{
    int fd = g_file_open_tmp(pattern, path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, length), ==, 0);
    close(fd);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (g_test_subprocess()) {
        flash_path = g_strdup(g_getenv("VENUSA_PWM_TEST_FLASH"));
        otp_path = g_strdup(g_getenv("LISEM_QEMU_OTP_IMAGE"));
    } else {
        image("venusa-pwm-flash-XXXXXX", &flash_path, 16 * 1024 * 1024);
        image("venusa-pwm-otp-XXXXXX", &otp_path, 64);
        g_setenv("VENUSA_PWM_TEST_FLASH", flash_path, true);
        g_setenv("LISEM_QEMU_OTP_IMAGE", otp_path, true);
    }
    qtest_add_func("venusa/pwm/waveform", waveform);
    qtest_add_func("venusa/pwm/shadow-gate-reset", shadow_and_gate);
    qtest_add_func("venusa/pwm/pinmux", pinmux);
    qtest_add_func("venusa/pwm/gated-shadow", gated_shadow);
    qtest_add_func("venusa/pwm/second-channel", second_channel);
    for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
        g_autofree char *name = g_strdup_printf("venusa/pwm/reject/%s", rejected[i].name);
        qtest_add_data_func(name, &rejected[i], reject);
    }
    int result = g_test_run();
    if (!g_test_subprocess()) {
        unlink(flash_path);
        unlink(otp_path);
    }
    g_free(flash_path);
    g_free(otp_path);
    return result;
}
