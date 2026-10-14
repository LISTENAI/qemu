/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define MTIME 0xe0030000
#define ECLIC_PENDING(irq) (0xe0021000 + (irq) * 4)
#define PSRAM_CTRL 0x44600000
#define SD 0x44a00000

static char *flash_path, *sd_path, *otp_path;

static QTestState *start(bool sd)
{
    g_autofree char *flash = g_shell_quote(flash_path);
    g_autofree char *card = g_shell_quote(sd_path);
    g_autofree char *drive = sd ? g_strdup_printf(
        "-drive if=none,id=sdcard,file=%s,format=raw", card) : g_strdup("");
    return qtest_initf("-machine venusa-rd-evb "
                       "-drive if=pflash,format=raw,file=%s %s", flash, drive);
}

static void systimer(void)
{
    QTestState *q = start(false);
    qtest_writel(q, MTIME + 8, 5);
    qtest_writel(q, MTIME + 12, 0);
    qtest_clock_step(q, 4999);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 4);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 5);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 1);
    qtest_writel(q, MTIME + 8, 7);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_clock_step(q, 100);
    qtest_writel(q, MTIME + 0xff8, 1);
    qtest_clock_step(q, 100000);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 5);
    qtest_writel(q, MTIME + 0xff8, 0);
    qtest_clock_step(q, 1899);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 1);
    qtest_writel(q, MTIME + 0xffc, 1);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(3)) & 1, ==, 1);
    qtest_writel(q, MTIME + 0xffc, 0);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(3)) & 1, ==, 0);
    qtest_system_reset(q);
    g_assert_cmpuint(qtest_readl(q, MTIME), ==, 0);
    g_assert_cmpuint(qtest_readb(q, ECLIC_PENDING(7)) & 1, ==, 0);
    qtest_quit(q);
}

static void psram(void)
{
    QTestState *q = start(false);
    static const uint32_t reset_modes[] = {
        0x8d09, 0x938d, 0xa093, 0x40a0, 0x0540, 0, 0, 0, 0x0905, 0x0101,
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(reset_modes); i++) {
        g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x800 + i * 4),
                       ==, reset_modes[i]);
    }
    qtest_writel(q, 0x38700000, 0x11223344);
    g_assert_cmphex(qtest_readl(q, 0x38f00000), ==, 0x11223344);
    qtest_writel(q, 0x38f00000, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(q, 0x38700000), ==, 0xaabbccdd);
    qtest_writel(q, PSRAM_CTRL + 0x800, 0x22);
    g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x800) & 0xff, ==, 0x22);
    g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x820) >> 8, ==, 0x22);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, 0x38700000), ==, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(q, PSRAM_CTRL + 0x800), ==, 0x8d09);
    qtest_quit(q);
}

static uint32_t command(QTestState *q, unsigned index, uint32_t argument,
                        unsigned flags)
{
    qtest_writel(q, SD + 0x30, UINT32_MAX);
    qtest_writel(q, SD + 8, argument);
    qtest_writew(q, SD + 0xe, (index << 8) | flags);
    uint32_t status = qtest_readl(q, SD + 0x30);
    g_assert_cmphex(status & 0xffff8000, ==, 0);
    g_assert_cmphex(status & 1, ==, 1);
    return qtest_readl(q, SD + 0x10);
}

static void sd_transfers(void)
{
    QTestState *q = start(true);
    qtest_clock_step(q, 1000000000);
    g_assert_cmphex(qtest_readl(q, SD + 0x40), ==, 0x27200080);
    g_assert_cmphex(qtest_readl(q, SD + 0x44), ==, 0x2f77);
    g_assert_cmphex(qtest_readl(q, SD + 0x24) & 0x70000, ==, 0x20000);
    qtest_writel(q, SD + 0x34, UINT32_MAX);
    qtest_writel(q, SD + 0x38, UINT32_MAX);
    qtest_writeb(q, SD + 0x29, 15);
    qtest_writew(q, SD + 0x2c, 7);
    command(q, 0, 0, 0);
    g_assert_cmphex(command(q, 8, 0x1aa, 2), ==, 0x1aa);
    command(q, 55, 0, 2);
    g_assert_cmphex(command(q, 41, 0xff8000, 2) & 0x80000000, ==, 0x80000000);
    command(q, 2, 0, 1);
    uint32_t rca = command(q, 3, 0, 2) & 0xffff0000;
    command(q, 7, rca, 3);
    qtest_writew(q, SD + 4, 512);
    qtest_writew(q, SD + 6, 1);
    qtest_writew(q, SD + 12, 2);
    command(q, 24, 512, 0x3a);
    for (unsigned n = 0; n < 128; n++) {
        qtest_writel(q, SD + 32, 0xa5000000 + n);
    }
    g_assert_cmphex(qtest_readl(q, SD + 0x30) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(39)) & 1, ==, 1);
    qtest_writel(q, SD + 0x30, UINT32_MAX);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(39)) & 1, ==, 0);
    qtest_writew(q, SD + 6, 1);
    qtest_writew(q, SD + 12, 0x12);
    command(q, 17, 512, 0x3a);
    for (unsigned n = 0; n < 128; n++) {
        g_assert_cmphex(qtest_readl(q, SD + 32), ==, 0xa5000000 + n);
    }
    qtest_writew(q, SD + 6, 1);
    qtest_writew(q, SD + 12, 0x11);
    qtest_writel(q, SD, 0x20010000);
    g_assert_cmphex(qtest_readw(q, SD + 12) & 1, ==, 1);
    command(q, 17, 512, 0x3a);
    g_assert_cmphex(qtest_readl(q, SD), ==, 0x20010200);
    for (unsigned n = 0; n < 128; n++) {
        g_assert_cmphex(qtest_readl(q, 0x20010000 + n * 4), ==, 0xa5000000 + n);
    }
    g_assert_cmphex(qtest_readl(q, SD + 0x30) & 2, ==, 2);
    qtest_quit(q);
    FILE *f = fopen(sd_path, "rb");
    uint32_t data[128];
    g_assert_nonnull(f);
    g_assert_cmpint(fseek(f, 512, SEEK_SET), ==, 0);
    g_assert_cmpuint(fread(data, sizeof(data), 1, f), ==, 1);
    fclose(f);
    for (unsigned n = 0; n < 128; n++) {
        g_assert_cmphex(le32_to_cpu(data[n]), ==, 0xa5000000 + n);
    }
}

static void dma_channels(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x40000000;
    for (unsigned n = 0; n < 16; n++) {
        qtest_writel(q, 0x20010000 + n * 4, 0x78340000 + n);
    }
    qtest_writel(q, base + 0x310, 0x3f3f);
    qtest_writel(q, base + 0x398, 1);
    for (unsigned channel = 0; channel < 6; channel++) {
        uint32_t off = base + channel * 0x58;
        uint32_t destination = 0x20011000 + channel * 0x100;
        qtest_writel(q, off, 0x20010000);
        qtest_writel(q, off + 8, destination);
        qtest_writel(q, off + 0x18, 0x25);
        qtest_writel(q, off + 0x1c, 16);
        qtest_writel(q, base + 0x3a0, 0x101u << channel);
        for (unsigned n = 0; n < 16; n++) {
            g_assert_cmphex(qtest_readl(q, destination + n * 4), ==, 0x78340000 + n);
        }
        g_assert_cmphex(qtest_readl(q, off), ==, 0x20010040);
        g_assert_cmphex(qtest_readl(q, off + 8), ==, destination + 64);
        g_assert_cmphex(qtest_readl(q, off + 0x1c), ==, 0x100010);
        g_assert_cmphex(qtest_readl(q, base + 0x3a0), ==, 0);
        for (unsigned irq = 0; irq < 6; irq++) {
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(43 + irq)) & 1,
                           ==, irq == channel);
        }
        qtest_writel(q, base + 0x338, 1u << channel);
        g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(43 + channel)) & 1, ==, 0);
    }
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, base + 0x398), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 0x2c0), ==, 0);
    qtest_quit(q);
}

static void calendar(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x43400000;
    qtest_writel(q, base + 0x0c, 0x173b3b);
    qtest_writel(q, base + 0x10, 0x0418021d);
    qtest_writel(q, base + 4, 1);
    qtest_writel(q, base, 2);
    qtest_writel(q, base + 0x1c, 0);
    qtest_writel(q, base + 0x20, 0x00180301);
    qtest_writel(q, base + 4, 0x10030);
    qtest_clock_step(q, 999999999);
    g_assert_cmphex(qtest_readl(q, base + 8) & 3, ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, base + 0x14), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 0x18), ==, 0x05180301);
    g_assert_cmphex(qtest_readl(q, base + 8) & 3, ==, 3);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 1);
    qtest_writel(q, base + 4, 0x100);
    g_assert_cmphex(qtest_readl(q, base + 8) & 3, ==, 1);
    qtest_writel(q, base + 4, 0x20000);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 0);
    qtest_writel(q, base + 4, 0x10000);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 1);
    qtest_writel(q, base + 4, 0x200);
    g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(22)) & 1, ==, 0);
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, base + 0x18), ==, 0x06000101);
    qtest_quit(q);
}

static void dual_timers(void)
{
    QTestState *q = start(false);
    for (unsigned block = 0; block < 2; block++) {
        uint32_t base = 0x43200000 + 0x100000 * block;
        for (unsigned channel = 0; channel < 2; channel++) {
            uint32_t reg = base + 0x20 * channel;
            qtest_writel(q, reg, 2);
            qtest_writel(q, reg + 8, 0xe2);
            qtest_clock_step(q, 187499);
            g_assert_cmphex(qtest_readl(q, reg + 4), ==, 0);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(20 + block)) & 1, ==, 0);
            qtest_clock_step(q, 1);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(20 + block)) & 1, ==, 1);
            g_assert_cmphex(qtest_readl(q, reg + 4), ==, 2);
            qtest_writel(q, reg + 12, 1);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(20 + block)) & 1, ==, 0);
            qtest_writel(q, reg + 8, 0x62);
            qtest_clock_step(q, 1000000);
            g_assert_cmphex(qtest_readl(q, reg + 4), ==, 2);
        }
    }
    qtest_quit(q);
}

static void gpt_timers(void)
{
    QTestState *q = start(false);
    const uint32_t base = 0x44000000;
    qtest_writel(q, 0x43100028, 0x40);
    for (unsigned channel = 0; channel < 2; channel++) {
        for (unsigned down = 0; down < 2; down++) {
            qtest_writel(q, 0x43100014, 0x200);
            g_assert_cmphex(qtest_readl(q, base), ==, 0x10);
            g_assert_cmphex(qtest_readl(q, base + 0x80), ==, 0x7bf0);
            qtest_writel(q, base + 4 + 4 * channel, 0x0180095f);
            qtest_writel(q, base + 12 + 4 * channel, 8);
            qtest_writel(q, base + 20 + 4 * channel, 9);
            qtest_writel(q, base + 0xa0, 1u << channel);
            uint32_t cfg = (1u << (14 + 4 * channel)) |
                           (down << (6 + 4 * channel));
            qtest_writel(q, base + 0x1c, cfg);
            qtest_writel(q, base + 12 + 4 * channel, 0x8008);
            qtest_writel(q, base + 0x1c, cfg | (1u << channel) |
                         (1u << (24 + channel)));
            qtest_clock_step(q, 999999);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
            uint32_t count = qtest_readl(q, base + 20 + 4 * channel) >> 16;
            g_assert_cmphex(count, ==, down ? 0 : 9);
            qtest_clock_step(q, 1);
            uint32_t pending = 1u << (6 + channel);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, pending);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(34)) & 1, ==, 0);
            qtest_writel(q, base + 0x80, 0x7bf0 & ~pending);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(34)) & 1, ==, 1);
            g_assert_cmphex(qtest_readl(q, base + 0x80) >> 16, ==, pending);
            qtest_writel(q, base + 0x84, pending << 16);
            g_assert_cmphex(qtest_readb(q, ECLIC_PENDING(34)) & 1, ==, 0);
            /* A gated module preserves a partially elapsed timer clock. */
            qtest_clock_step(q, 150000);
            qtest_writel(q, 0x43100028, 0);
            count = qtest_readl(q, base + 20 + 4 * channel);
            qtest_clock_step(q, 10000000);
            g_assert_cmphex(qtest_readl(q, base + 20 + 4 * channel), ==, count);
            qtest_writel(q, 0x43100028, 0x40);
            qtest_clock_step(q, 849999);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
            qtest_clock_step(q, 1);
            g_assert_cmphex(qtest_readl(q, base + 0x84), ==, pending);
        }
    }
    /* Two independent 8-bit timers share one channel clock and raw IRQ word. */
    qtest_writel(q, 0x43100014, 0x200);
    qtest_writel(q, base + 4, 0x0180095f);
    qtest_writel(q, base + 12, 0x10);
    qtest_writel(q, base + 20, 0x0301);
    qtest_writel(q, base + 0xa0, 1);
    qtest_writel(q, base + 12, 0x8010);
    qtest_writel(q, base + 0x1c, 0x05000001);
    qtest_clock_step(q, 200000);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x40);
    qtest_writel(q, base + 0x84, 0x00400000);
    qtest_clock_step(q, 200000);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x100);
    qtest_writel(q, base + 0x84, 0x01000000);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 20), ==, 0x03010301);
    /* Free-run uses the full counter width, independently of reload. */
    qtest_writel(q, 0x43100014, 0x200);
    qtest_writel(q, base + 4, 0x0180095f);
    qtest_writel(q, base + 12, 0x8008);
    qtest_writel(q, base + 20, 9);
    qtest_writel(q, base + 0xa0, 1);
    qtest_writel(q, base + 0x1c, 0x01008001);
    qtest_clock_step(q, INT64_C(6553600000) - 1);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
    g_assert_cmphex(qtest_readl(q, base + 20) >> 16, ==, 65535);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x40);
    /* Keepgo resumes the stopped count; an external T0 divider is latched. */
    qtest_writel(q, 0x43100014, 0x200);
    qtest_writel(q, 0x43100028, 0x62);
    qtest_writel(q, 0x43100028, 0x42);
    qtest_writel(q, base + 4, 0x0180095f);
    qtest_writel(q, base + 12, 0x8008);
    qtest_writel(q, base + 20, 9);
    qtest_writel(q, base + 0xa0, 1);
    qtest_writel(q, base + 0x1c, 0x0100c041);
    qtest_clock_step(q, 600000);
    g_assert_cmphex(qtest_readl(q, base + 20) >> 16, ==, 6);
    qtest_writel(q, base + 0x1c, 0x0100c044);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, base + 20) >> 16, ==, 6);
    qtest_writel(q, base + 0x1c, 0x0100c041);
    qtest_clock_step(q, 1399999);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, base + 0x84), ==, 0x40);
    qtest_quit(q);
}

static void otp_mapping(void)
{
    QTestState *q = start(false);
    for (unsigned word = 0; word < 16; word++) {
        g_assert_cmphex(qtest_readl(q, 0x45500080 + 4 * word), ==, 0x97530000 + word);
        g_assert_cmphex(qtest_readl(q, 0x45501000 + 4 * word), ==, 0x97530000 + word);
    }
    qtest_system_reset(q);
    g_assert_cmphex(qtest_readl(q, 0x45500090), ==, 0x97530004);
    qtest_quit(q);
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
    image("venusa-test-flash-XXXXXX", &flash_path, 16 * 1024 * 1024);
    image("venusa-test-sd-XXXXXX", &sd_path, 64 * 1024 * 1024);
    image("venusa-test-otp-XXXXXX", &otp_path, 64);
    FILE *otp = fopen(otp_path, "wb");
    g_assert_nonnull(otp);
    for (unsigned i = 0; i < 16; i++) {
        uint32_t value = cpu_to_le32(0x97530000 + i);
        g_assert_cmpuint(fwrite(&value, 4, 1, otp), ==, 1);
    }
    fclose(otp);
    g_setenv("LISEM_QEMU_OTP_IMAGE", otp_path, true);
    qtest_add_func("venusa/systimer", systimer);
    qtest_add_func("venusa/psram", psram);
    qtest_add_func("venusa/sd-transfers", sd_transfers);
    qtest_add_func("venusa/dma-channels", dma_channels);
    qtest_add_func("venusa/calendar", calendar);
    qtest_add_func("venusa/dual-timers", dual_timers);
    qtest_add_func("venusa/gpt-timers", gpt_timers);
    qtest_add_func("venusa/otp-mapping", otp_mapping);
    int result = g_test_run();
    unlink(flash_path); unlink(sd_path); unlink(otp_path);
    g_free(flash_path); g_free(sd_path); g_free(otp_path);
    return result;
}
