/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#define I2C 0x43c00000
static char *flash;
static void command(QTestState *q, unsigned ctl)
{
    qtest_writel(q,I2C+0x18,0x3f8);
    qtest_writel(q,I2C+0x24,ctl);qtest_writel(q,I2C+0x28,1);
    qtest_clock_step(q,10000);
}
static void write_reg(QTestState *q,unsigned reg,unsigned value)
{
    qtest_writel(q,I2C+0x20,reg);qtest_writel(q,I2C+0x20,value);
    command(q,0x1e02);
    g_assert_cmphex(qtest_readl(q,I2C+0x18)&0x600,==,0x600);
}
static void read_regs(QTestState *q,unsigned reg,uint8_t *data,unsigned n)
{
    qtest_writel(q,I2C+0x20,reg);command(q,0x1c01);
    command(q,0x1f00|n);qtest_clock_step(q,100000);
    for(unsigned i=0;i<n;i++){data[i]=qtest_readl(q,I2C+0x20);qtest_clock_step(q,3000);}
}
static QTestState *start(void)
{
    g_autofree char *f=g_shell_quote(flash);
    QTestState *q=qtest_initf("-machine venusa-rd-evb -drive if=pflash,format=raw,file=%s -device venusa-ft6236-touch,id=touch,soc=/machine/soc",f);
    qtest_writel(q,0x43100034,2);
    qtest_writel(q,I2C+0x2c,5);qtest_writel(q,I2C+0x1c,0x38);
    qtest_irq_intercept_in(q,"/machine/soc");
    return q;
}
static void set(QTestState*q,unsigned slot,bool down,unsigned x,unsigned y)
{
    qtest_set_irq_in(q,"/machine/peripheral/touch","contact",slot,(down<<24)|(x<<12)|y);
}
static void contacts(void)
{
    QTestState*q=start();uint8_t b[14];
    read_regs(q,0xa3,b,1);g_assert_cmphex(b[0],==,0x36);
    write_reg(q,0xa4,0);
    set(q,0,true,123,456);read_regs(q,1,b,14);
    g_assert_cmpuint(b[1],==,1);g_assert_cmphex(b[2],==,0);
    g_assert_cmpuint(b[3],==,123);g_assert_cmphex(b[4],==,1);g_assert_cmpuint(b[5],==,200);
    set(q,1,true,479,799);read_regs(q,1,b,14);
    g_assert_cmpuint(b[1],==,2);g_assert_cmphex(b[2]&0xc0,==,0x80);
    g_assert_cmphex(b[8],==,1);g_assert_cmpuint(b[9],==,223);
    g_assert_cmphex(b[10],==,0x13);g_assert_cmpuint(b[11],==,31);
    set(q,0,false,0,0);read_regs(q,1,b,14);
    g_assert_cmpuint(b[1],==,1);g_assert_cmphex(b[4]>>4,==,1);
    set(q,1,false,0,0);read_regs(q,1,b,14);g_assert_cmpuint(b[1],==,0);
    /* Unsupported factory mode is NACKed, not a fake factory transition. */
    qtest_writel(q,I2C+0x20,0);qtest_writel(q,I2C+0x20,0x40);
    command(q,0x1e02);g_assert_cmphex(qtest_readl(q,I2C+0x18)&0x400,==,0);
    qtest_quit(q);
}
static void interrupt(void)
{
    QTestState*q=start();
    qtest_writel(q,0x44100004,1); /* PA1 GPIO, input. */
    qtest_writel(q,0x43600054,5<<4); /* Falling edge. */
    qtest_writel(q,0x43600050,2);
    write_reg(q,0xa4,1);
    set(q,0,true,10,20);
    g_assert_cmphex(qtest_readl(q,0x43600064)&2,==,2);
    g_assert_cmpuint(qtest_readb(q,0xe0021000+24*4),==,1);
    qtest_writel(q,0x43600064,2);
    g_assert_cmphex(qtest_readl(q,0x43600064)&2,==,0);
    g_assert_cmpuint(qtest_readb(q,0xe0021000+24*4),==,0);
    g_assert_cmphex(qtest_readl(q,0x43600020)&2,==,0);
    qtest_clock_step(q,99999);
    g_assert_cmphex(qtest_readl(q,0x43600020)&2,==,0);
    qtest_clock_step(q,1);
    g_assert_cmphex(qtest_readl(q,0x43600020)&2,==,2);
    set(q,0,false,0,0);
    g_assert_cmphex(qtest_readl(q,0x43600020)&2,==,0);
    qtest_clock_step(q,100000);
    g_assert_cmphex(qtest_readl(q,0x43600020)&2,==,2);
    qtest_quit(q);
}
int main(int argc,char**argv)
{
    g_test_init(&argc,&argv,NULL);
    int fd=g_file_open_tmp("venusa-touch-flash-XXXXXX",&flash,NULL);
    g_assert_cmpint(ftruncate(fd,16*1024*1024),==,0);close(fd);
    qtest_add_func("venusa/touch/contacts-i2c",contacts);
    qtest_add_func("venusa/touch/interrupt-pulse",interrupt);
    int r=g_test_run();unlink(flash);g_free(flash);return r;
}
