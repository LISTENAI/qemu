/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Nuclei cluster interrupt distribution, claim arbitration and semaphores. */
#include "qemu/osdep.h"
#include "hw/intc/nuclei_cidu.h"
#include "system/address-spaces.h"

static void route(NucleiCIDU *s, unsigned n)
{
    uint32_t enabled = s->targets[n] & s->claims[n];
    for (unsigned core = 0; core < s->cores; core++) {
        s->irq(s->opaque, core, s->irq_offset + n,
               s->levels[n] && (enabled & (1u << core)));
    }
}

void nuclei_cidu_redrive(NucleiCIDU *s)
{
    for (unsigned core = 0; core < s->cores; core++) {
        s->irq(s->opaque, core, s->ici_irq, (s->status[core] & 0xffffff) != 0);
    }
    for (unsigned n = 0; n < s->interrupts; n++) { route(s, n); }
}

void nuclei_cidu_input(NucleiCIDU *s, unsigned n, bool level)
{
    assert(n < s->interrupts);
    s->levels[n] = level;
    route(s, n);
}

static G_NORETURN void fault(NucleiCIDU *s, hwaddr off, unsigned size,
                             bool write, uint64_t value)
{
    s->fail(s->opaque, s->base + off, size, write, value);
    abort();
}

static uint64_t read_register(void *opaque, hwaddr off, unsigned size)
{
    NucleiCIDU *s = opaque;
    if (size != 4 || (off & 3)) { fault(s, off, size, false, 0); }
    if (off < s->cores * 4) { return s->status[off / 4]; }
    if (off >= 0x80 && off < 0x80 + s->semaphores * 4) {
        return s->semaphore[(off - 0x80) / 4];
    }
    if (off >= 0x4000 && off < 0x4000 + s->interrupts * 4) {
        return s->targets[(off - 0x4000) / 4];
    }
    if (off >= 0x8000 && off < 0x8000 + s->interrupts * 4) {
        return s->claims[(off - 0x8000) / 4];
    }
    if (off >= 0x180 && off <= 0x188) { return 0; }
    if (off == 0xc084) { return s->cores; }
    if (off == 0xc090) { return s->interrupts; }
    fault(s, off, size, false, 0);
}

static void write_register(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    NucleiCIDU *s = opaque;
    uint32_t all = (1u << s->cores) - 1;
    if (size != 4 || (off & 3)) { fault(s, off, size, true, value); }
    if (off < s->cores * 4) {
        unsigned core = off / 4;
        s->status[core] &= ~(value & 0xffffff);
        s->irq(s->opaque, core, s->ici_irq, (s->status[core] & 0xffffff) != 0);
        return;
    }
    if (off >= 0x80 && off < 0x80 + s->semaphores * 4) {
        uint32_t *sem = &s->semaphore[(off - 0x80) / 4];
        if ((value & 0x3ff) == 0x3ff) { *sem = 0x3ff; }
        else if (value >= s->cores) { fault(s, off, size, true, value); }
        else if (*sem == 0x3ff) { *sem = value; }
        return;
    }
    if (off == 0x3ffc) {
        unsigned from = (value >> 16) & 0xff, to = value & 0xffff;
        if (from >= s->cores || to >= s->cores) {
            fault(s, off, size, true, value);
        }
        s->status[to] = (value & 0xff000000) |
                        (s->status[to] & 0xffffff) | (1u << from);
        s->irq(s->opaque, to, s->ici_irq, true);
        return;
    }
    if (off >= 0x4000 && off < 0x4000 + s->interrupts * 4) {
        unsigned n = (off - 0x4000) / 4;
        s->targets[n] = value & all;
        route(s, n);
        return;
    }
    if (off >= 0x8000 && off < 0x8000 + s->interrupts * 4) {
        unsigned n = (off - 0x8000) / 4;
        if ((value & all) == all) { s->claims[n] = all; }
        else if (s->claims[n] == all) { s->claims[n] = value & all; }
        route(s, n);
        return;
    }
    /* Event delivery is distinct from IRQ distribution and not implemented. */
    if (off >= 0x180 && off <= 0x188 && !value) { return; }
    fault(s, off, size, true, value);
}

static const MemoryRegionOps ops = {
    .read = read_register, .write = write_register,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};
void nuclei_cidu_init(NucleiCIDU *s, Object *owner, const char *name,
                     hwaddr base, unsigned cores, unsigned interrupts,
                     unsigned semaphores, unsigned irq_offset, unsigned ici_irq)
{
    assert(cores && cores <= 2 && interrupts <= 80 && semaphores <= 32);
    s->base = base; s->cores = cores; s->interrupts = interrupts;
    s->semaphores = semaphores; s->irq_offset = irq_offset;
    s->ici_irq = ici_irq;
    memory_region_init_io(&s->io, owner, &ops, s, name, 0x10000);
    memory_region_add_subregion(get_system_memory(), base, &s->io);
}

void nuclei_cidu_reset(NucleiCIDU *s)
{
    memset(s->status, 0, sizeof(s->status));
    for (unsigned i = 0; i < s->semaphores; i++) { s->semaphore[i] = 0x3ff; }
    for (unsigned i = 0; i < s->interrupts; i++) {
        s->targets[i] = s->claims[i] = (1u << s->cores) - 1;
    }
    nuclei_cidu_redrive(s);
}
