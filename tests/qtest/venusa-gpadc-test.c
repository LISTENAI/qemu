/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

#define ADC 0x43500000
#define ADC_IRQ (0xe0021000 + 23 * 4)
#define CLOCK 0x4310002c
#define RESET 0x43100014

static char *flash_path, *otp_path;

static QTestState *start(void)
{
    g_autofree char *flash = g_shell_quote(flash_path);
    QTestState *q = qtest_initf("-machine venusa-rd-evb "
                              "-drive if=pflash,format=raw,file=%s", flash);
    qtest_writel(q, CLOCK, qtest_readl(q, CLOCK) | 1);
    qtest_writel(q, RESET, 1u << 14);
    return q;
}

static void convert(QTestState *q, unsigned channels, unsigned rounds)
{
    qtest_writel(q, ADC + 4, 1);
    qtest_writel(q, ADC + 0x78, 8);
    qtest_writel(q, ADC + 0x0c, channels);
    qtest_writel(q, ADC, 0xa0821 | (rounds << 24));
    g_assert_cmphex(qtest_readl(q, ADC) & 1, ==, 0);
    g_assert_cmphex(qtest_readl(q, ADC + 0x78) & 8, ==, 8);
}

static void reset_and_voltage(void)
{
    QTestState *q = start();
    g_assert_cmphex(qtest_readl(q, ADC), ==, 0x010a0820);
    g_assert_cmphex(qtest_readl(q, ADC + 4), ==, 0);
    g_assert_cmphex(qtest_readl(q, ADC + 8), ==, 0x25);
    g_assert_cmphex(qtest_readl(q, ADC + 0x60), ==, 0x3f0002);
    g_assert_cmphex(qtest_readl(q, ADC + 0x78), ==, 0x3f0000);
    for (unsigned channel = 0; channel < 6; channel++) {
        g_assert_cmpuint(qtest_readl(q, ADC + 0xd4 + 4 * channel), ==, 8);
    }
    qtest_writel(q, ADC + 4, 3);
    g_assert_cmphex(qtest_readl(q, ADC + 4), ==, 0);
    /* Voltages are external sources, independent of peripheral reset. */
    qtest_set_irq_in(q, "/machine/soc", "adc-voltage-uv", 3, 550000);
    qtest_writel(q, ADC + 8, 0x20);
    convert(q, 1u << 3, 3);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xe0) >> 8, ==, 3);
    for (unsigned n = 0; n < 3; n++) {
        g_assert_cmpuint(qtest_readl(q, ADC + 0xa0), ==, 512);
    }
    qtest_writel(q, 0x45000084, 3);
    convert(q, 1u << 3, 1);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xa0), ==, 341);
    qtest_writel(q, 0x45000084, 1);
    qtest_writel(q, ADC + 8, 0x24);
    convert(q, 1u << 3, 1);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xa0), ==, 170);
    qtest_writel(q, ADC + 8, 0x20);
    qtest_set_irq_in(q, "/machine/soc", "adc-voltage-uv", 3, 3300000);
    convert(q, 1u << 3, 1);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xa0), ==, 1023);
    qtest_writel(q, RESET, 1u << 14);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xe0), ==, 8);
    qtest_writel(q, ADC + 8, 0x20);
    convert(q, 1u << 3, 1);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xa0), ==, 1023);
    qtest_quit(q);
}

static void complete_irq(void)
{
    QTestState *q = start();
    qtest_writel(q, ADC + 0x60, UINT32_MAX);
    convert(q, 0x3f, 2);
    g_assert_cmphex(qtest_readl(q, ADC + 0x84), ==, 0);
    g_assert_cmphex(qtest_readb(q, ADC_IRQ) & 1, ==, 0);
    /* A new trigger is ignored until COMPLETE is acknowledged. */
    qtest_writel(q, ADC, 0x010a0821);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xd8) >> 8, ==, 2);
    g_assert_cmphex(qtest_readl(q, ADC + 4) & 1, ==, 1);
    qtest_writel(q, ADC + 4, 1);
    qtest_writel(q, ADC, 0x010a0821);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xd8) >> 8, ==, 2);
    /* Unmask an already pending conversion; W1C must deassert it. */
    qtest_writel(q, ADC + 0x60, 0x3f0007);
    g_assert_cmphex(qtest_readl(q, ADC + 0x84) & 8, ==, 8);
    g_assert_cmphex(qtest_readb(q, ADC_IRQ) & 1, ==, 1);
    qtest_writeb(q, ADC + 0x78, 8);
    g_assert_cmphex(qtest_readl(q, ADC + 0x78) & 8, ==, 0);
    g_assert_cmphex(qtest_readb(q, ADC_IRQ) & 1, ==, 0);
    qtest_writel(q, ADC, 0x010a0821);
    g_assert_cmphex(qtest_readl(q, ADC + 4) & 1, ==, 1);
    qtest_writel(q, ADC + 0x78, 8);
    /* No STOP is needed after acknowledging completion. */
    qtest_writel(q, ADC, 0x010a0821);
    for (unsigned channel = 0; channel < 6; channel++) {
        g_assert_cmpuint(qtest_readl(q, ADC + 0xd4 + 4 * channel) >> 8, ==, 4);
        g_assert_cmpuint(qtest_readl(q, ADC + 0x94 + 4 * channel), ==, 0);
        qtest_writel(q, ADC + 0xd4 + 4 * channel, 0x17);
        g_assert_cmpuint(qtest_readl(q, ADC + 0xd4 + 4 * channel), ==, 7);
    }
    qtest_quit(q);
}

static void fifo_boundaries(void)
{
    QTestState *q = start();
    qtest_writel(q, ADC + 0x60, UINT32_MAX);
    qtest_writel(q, ADC + 8, 0x20);
    qtest_writel(q, ADC + 0xe0, 3);
    qtest_set_irq_in(q, "/machine/soc", "adc-voltage-uv", 3, 275000);
    convert(q, 8, 2);
    g_assert_cmphex(qtest_readl(q, ADC + 0x7c) & 0x80000, ==, 0);
    convert(q, 8, 1);
    g_assert_cmphex(qtest_readl(q, ADC + 0x7c) & 0x80000, ==, 0x80000);
    qtest_writel(q, ADC + 0x7c, 0x80000);
    g_assert_cmphex(qtest_readl(q, ADC + 0x7c) & 0x80000, ==, 0);
    convert(q, 8, 13);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xe0) >> 8, ==, 16);
    g_assert_cmphex(qtest_readl(q, ADC + 0x7c) & 8, ==, 8);
    qtest_set_irq_in(q, "/machine/soc", "adc-voltage-uv", 3, 825000);
    convert(q, 8, 1);
    g_assert_cmpuint(qtest_readl(q, ADC + 0xe0) >> 8, ==, 16);
    g_assert_cmphex(qtest_readl(q, ADC + 0x80) & 8, ==, 8);
    for (unsigned n = 0; n < 16; n++) {
        g_assert_cmpuint(qtest_readl(q, ADC + 0xa0), ==, 256);
    }
    g_assert_cmphex(qtest_readl(q, ADC + 0x78) & 0x80000, ==, 0x80000);
    qtest_readl(q, ADC + 0xa0);
    g_assert_cmphex(qtest_readl(q, ADC + 0x80) & 0x80000, ==, 0x80000);
    qtest_writel(q, ADC + 0x68, 0x370037);
    g_assert_cmphex(qtest_readb(q, ADC_IRQ) & 1, ==, 1);
    qtest_writel(q, ADC + 0x80, 0x80008);
    g_assert_cmphex(qtest_readb(q, ADC_IRQ) & 1, ==, 0);
    qtest_quit(q);
}

typedef struct RejectedAccess {
    const char *name;
    unsigned offset;
    uint32_t value;
    bool write;
} RejectedAccess;

static const RejectedAccess rejected[] = {
    { "reserved-register", 0x10, 0, false },
    { "calibration", 0x40, 1, true },
    { "efuse-calibration", 4, 16, true },
    { "infinite-trigger", 0, 0xa0821, true },
    { "hardware-trigger", 0, 0x010a0822, true },
    { "dma-channel", 0x0c, 0x10001, true },
    { "dma-read", 0x90, 0, false },
};

static void rejected_access(const void *opaque)
{
    const RejectedAccess *access = opaque;
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        /* The fatal guest access closes the synchronous QTest connection. */
        if (access->write) {
            qtest_writel(q, ADC + access->offset, access->value);
        } else {
            qtest_readl(q, ADC + access->offset);
        }
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_autofree char *diagnostic = g_strdup_printf(
        "*VenusA unsupported %s*address=0x%08x size=4*",
        access->write ? "write" : "read", ADC + access->offset);
    g_test_trap_assert_stderr(diagnostic);
}

static void image(const char *pattern, char **path, size_t length)
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
        flash_path = g_strdup(g_getenv("VENUSA_ADC_TEST_FLASH"));
        otp_path = g_strdup(g_getenv("LISEM_QEMU_OTP_IMAGE"));
    } else {
        image("venusa-adc-flash-XXXXXX", &flash_path, 16 * 1024 * 1024);
        image("venusa-adc-otp-XXXXXX", &otp_path, 64);
        g_setenv("VENUSA_ADC_TEST_FLASH", flash_path, true);
        g_setenv("LISEM_QEMU_OTP_IMAGE", otp_path, true);
    }
    qtest_add_func("venusa/gpadc/reset-voltage", reset_and_voltage);
    qtest_add_func("venusa/gpadc/complete-irq", complete_irq);
    qtest_add_func("venusa/gpadc/fifo-boundaries", fifo_boundaries);
    for (unsigned n = 0; n < G_N_ELEMENTS(rejected); n++) {
        g_autofree char *name = g_strdup_printf("venusa/gpadc/reject/%s",
                                               rejected[n].name);
        qtest_add_data_func(name, &rejected[n], rejected_access);
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
