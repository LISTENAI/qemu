/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
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
static void codec_calibration(void)
{
    QTestState *q = start();
    const uint32_t codec = 0x44800000;
    g_assert_cmphex(qtest_readl(q, codec + 0x48), ==, 0x80);
    qtest_writel(q, codec, 3);
    /* Capacitance calibration precedes ADC channel sampling enable. */
    qtest_writel(q, codec + 0x1c, 0xfc0);
    qtest_writel(q, codec + 4, 0x1613);
    qtest_clock_step(q, 5333);
    g_assert_cmphex(qtest_readl(q, codec + 0x48), ==, 0);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, codec + 0x48), ==, 8);
    qtest_clock_step(q, 1000);
    qtest_writel(q, codec + 4, 0x613); /* GO low does not cancel. */
    qtest_writel(q, CLK, qtest_readl(q, CLK) & ~0x200u);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, codec + 0x48), ==, 8);
    qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x200);
    qtest_clock_step(q, 111000);
    g_assert_cmphex(qtest_readl(q, codec + 0x48), ==, 0xaa);
    qtest_writel(q, codec + 4, 0x1613);
    qtest_clock_step(q, 20000);
    qtest_writel(q, 0x43100014, 1u << 18);
    qtest_clock_step(q, 200000);
    g_assert_cmphex(qtest_readl(q, codec + 4), ==, 0x613);
    g_assert_cmphex(qtest_readl(q, codec + 0x48), ==, 0x80);
    qtest_quit(q);
}

static void codec_stereo(void)
{
    char *path;
    int fd = g_file_open_tmp("venusa-microphones-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    uint8_t pcm[32];
    for (unsigned i = 0; i < 8; i++) {
        stw_le_p(pcm + 4 * i, 100 + i);
        stw_le_p(pcm + 4 * i + 2, -(200 + i));
    }
    g_assert_cmpint(write(fd, pcm, sizeof(pcm)), ==, sizeof(pcm));
    close(fd);
    g_setenv("LISEM_QEMU_AUDIO_INPUT", path, true);
    g_setenv("LISEM_QEMU_AUDIO_FORMAT", "16000,2", true);
    QTestState *q = start();
    g_unsetenv("LISEM_QEMU_AUDIO_INPUT");
    g_unsetenv("LISEM_QEMU_AUDIO_FORMAT");
    const uint32_t codec = 0x44800000;
    qtest_writel(q, APC + 0x18, 0x02070007);
    qtest_writel(q, codec, 3);
    qtest_writel(q, codec + 0x1c, 0xfc3);
    qtest_writel(q, codec + 4, 0x793);
    qtest_clock_step(q, 62499);
    g_assert_cmphex(qtest_readl(q, APC + 0x18), ==, 0x02070007);
    qtest_clock_step(q, 1);
    g_assert_cmphex(qtest_readl(q, codec + 0x50), ==, 100u << 8);
    g_assert_cmphex(qtest_readl(q, codec + 0x54), ==, ((uint32_t)-200 << 8) & 0xffffff);
    g_assert_cmphex(qtest_readl(q, APC + 0x48), ==, 100u << 16);
    g_assert_cmphex(qtest_readl(q, APC + 0x48), ==, (uint32_t)-200 << 16);
    /* Left digital mute preserves the independent right microphone. */
    qtest_writel(q, codec + 8, (qtest_readl(q, codec + 8) & ~0x3f80u) | 0x80);
    qtest_clock_step(q, 62500);
    g_assert_cmphex(qtest_readl(q, APC + 0x48), ==, 0);
    g_assert_cmphex(qtest_readw(q, APC + 0x4a), ==, (uint16_t)-201);
    qtest_writel(q, CLK, qtest_readl(q, CLK) & ~0x200u);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(qtest_readl(q, APC + 0x18), ==, 0x02070007);
    qtest_writel(q, CLK, qtest_readl(q, CLK) | 0x200);
    qtest_clock_step(q, 62500);
    g_assert_cmphex(qtest_readl(q, APC + 0x48), ==, 0);
    g_assert_cmphex(qtest_readl(q, APC + 0x48), ==, (uint32_t)-202 << 16);
    qtest_quit(q);
    unlink(path);
    g_free(path);
}

static void codec_output(void)
{
    char *path;
    int fd = g_file_open_tmp("venusa-speaker-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_setenv("LISEM_QEMU_AUDIO_OUTPUT", path, true);
    QTestState *q = start();
    g_unsetenv("LISEM_QEMU_AUDIO_OUTPUT");
    const uint32_t codec = 0x44800000;
    qtest_writel(q, APC + 0x0c, 7);
    qtest_writel(q, APC + 0x1c, 0x10000001);
    qtest_writel(q, 0x20010000, 0xfc1803e8);
    qtest_writel(q, 0x20010004, 0xf83007d0);
    qtest_writel(q, 0x431000a0, 1u << 8);
    qtest_writel(q, 0x40000000, 0x20010000);
    qtest_writel(q, 0x40000008, APC + 0x36);
    qtest_writel(q, 0x40000018, 0x00100113);
    qtest_writel(q, 0x4000001c, 4);
    qtest_writel(q, 0x40000044, 8u << 11);
    qtest_writel(q, 0x40000398, 1);
    qtest_writel(q, 0x400003a0, 0x101);
    g_assert_cmphex(qtest_readl(q, 0x400002c0), ==, 1);
    qtest_writel(q, codec, 3);
    qtest_writel(q, codec + 0x34, 0);
    qtest_writel(q, codec + 0x38, 0x10);
    qtest_writel(q, codec + 0x44, 1);
    qtest_writel(q, codec + 0x2c, 0x63);
    qtest_clock_step(q, 62500);
    qtest_writel(q, codec + 0x34, 0x40);
    qtest_clock_step(q, 62500);
    /* Digital echo precedes the codec mute control. */
    g_assert_cmphex(qtest_readl(q, APC + 0x50), ==, 0xfc1803e8);
    qtest_writel(q, codec + 0x34, 0);
    qtest_clock_step(q, 62500);
    qtest_clock_step(q, 62500);
    qtest_quit(q);
    char *bytes;
    gsize length;
    g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    g_assert_cmpuint(length, ==, 52);
    g_assert_cmpuint(ldl_le_p(bytes + 24), ==, 16000);
    g_assert_cmpint((int16_t)lduw_le_p(bytes + 44), ==, 1000);
    g_assert_cmpint((int16_t)lduw_le_p(bytes + 46), ==, 0);
    g_assert_cmpint((int16_t)lduw_le_p(bytes + 48), ==, 2000);
    g_assert_cmpint((int16_t)lduw_le_p(bytes + 50), ==, -2000);
    g_free(bytes);
    unlink(path);
    g_free(path);
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

typedef struct RejectedAudio {
    const char *name;
    uint32_t address, value;
} RejectedAudio;
static const RejectedAudio rejected[] = {
    { "alc", 0x44800010, 0x63192b00 },
    { "noise-filter", 0x4480000c, 0x4000 },
    { "digital-microphone", 0x44800014, 0xc10146 },
    { "debug-capture", 0x44800060, 1 },
    { "unpowered-calibration", 0x44800004, 0x1613 },
    { "pll-codec-clock", 0x43100034, 0xd600 },
    { "rx-resampler", APC + 0x1c, 0x201 },
};
static void rejected_audio(const void *opaque)
{
    const RejectedAudio *r = opaque;
    if (g_test_subprocess()) {
        QTestState *q = start();
        qtest_set_expected_status(q, 1);
        qtest_writel(q, r->address, r->value);
        qtest_quit(q);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, 0);
    g_test_trap_assert_failed();
    g_autofree char *message = g_strdup_printf(
        "*VenusA unsupported write*address=0x%08x size=4*", r->address);
    g_test_trap_assert_stderr(message);
}

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
    qtest_add_func("venusa/audio/codec-stereo-input", codec_stereo);
    qtest_add_func("venusa/audio/codec-output-mute", codec_output);
    qtest_add_func("venusa/audio/codec-calibration", codec_calibration);
    qtest_add_func("venusa/audio/initial-fifo-wait", initial_fifo_wait);
    for (unsigned i = 0; i < ARRAY_SIZE(rejected); i++) {
        g_autofree char *name = g_strdup_printf("venusa/audio/reject/%s",
                                               rejected[i].name);
        qtest_add_data_func(name, &rejected[i], rejected_audio);
    }
    int result = g_test_run();
    unlink(flash);
    g_free(flash);
    return result;
}
