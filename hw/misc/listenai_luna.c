/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "lisem_luna_validated.h"
#include "hw/misc/listenai_luna.h"
#include "system/address-spaces.h"
#include "crypto/hash.h"
#include "qemu/error-report.h"

static bool read_memory(void *opaque, uint32_t address, void *data, size_t size)
{
    return address_space_read(&address_space_memory, address,
                              MEMTXATTRS_UNSPECIFIED, data, size) == MEMTX_OK;
}

static bool write_memory(void *opaque, uint32_t address, const void *data, size_t size)
{
    return address_space_write(&address_space_memory, address,
                               MEMTXATTRS_UNSPECIFIED, data, size) == MEMTX_OK;
}

static bool sha256(const void *data, size_t size, char hex[65])
{
    g_autofree char *digest = NULL;
    if (qcrypto_hash_digest(QCRYPTO_HASH_ALGO_SHA256, data, size, &digest, NULL) < 0) {
        return false;
    }
    memcpy(hex, digest, 65);
    return true;
}

static void schedule(void *opaque, uint64_t delay_ns)
{
    ListenAILUNA *s = opaque;
    s->pending = true;
    s->remaining = delay_ns;
    if (s->clock_enabled) {
        timer_mod(s->completion, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
    }
}

static void cancel(void *opaque)
{
    ListenAILUNA *s = opaque;
    timer_del(s->completion);
    s->pending = false;
    s->remaining = 0;
}

static G_NORETURN void fatal(void *opaque, const char *message)
{
    ListenAILUNA *s = opaque;
    s->fatal(s->opaque, message);
    abort();
}

static void update_irq(ListenAILUNA *s)
{
    if (s->irq) {
        s->irq(s->opaque, (lisem_luna_read(s->backend, 0xd8, 4) &
                          ~s->irq_mask) != 0);
    }
}

static void complete(void *opaque)
{
    ListenAILUNA *s = opaque;
    s->pending = false;
    lisem_luna_complete(s->backend);
    update_irq(s);
}

static uint64_t read_reg(void *opaque, hwaddr offset, unsigned size)
{
    ListenAILUNA *s = opaque;
    uint64_t value;
    if (size == 4 && s->read_extra &&
        s->read_extra(s->opaque, offset, &value)) { return value; }
    if (size == 4 && s->irq) {
        if (offset == 0xd4) { return s->irq_mask; }
        if (offset == 0xd8 || offset == 0xdc) {
            value = lisem_luna_read(s->backend, 0xd8, 4);
            return offset == 0xd8 ? value & ~s->irq_mask : value;
        }
    }
    value = lisem_luna_read(s->backend, offset, size);
    return offset == 0xc0 && size == 4 ? value & s->reset_mask : value;
}

static void write_reg(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    ListenAILUNA *s = opaque;
    if (size == 4 && s->write_extra &&
        s->write_extra(s->opaque, offset, value)) { return; }
    if (offset == 0xd4 && size == 4 && s->irq) {
        s->irq_mask = value & s->irq_mask_bits;
        update_irq(s);
        return;
    }
    if (offset == 0xc0 && size == 4 && s->reset_mask != UINT32_MAX) {
        /* Reset-domain masks are SoC wiring, not microprogram state.
         * Non-reset upper bits (including DONE_CLR) need separate semantics. */
        if (value && value != s->reset_mask) {
            fatal(s, "LUNA partial reset or completion-clear mask unsupported");
        }
        value = value ? UINT32_MAX : 0;
    }
    lisem_luna_write(s->backend, offset, value, size);
    if (offset == 0xc0 && size == 4 && value == 0) {
        s->irq_mask = 0;
        if (s->reset_extra) { s->reset_extra(s->opaque); }
    }
    update_irq(s);
}

static bool read_safe(void *opaque, hwaddr offset)
{
    ListenAILUNA *s = opaque;
    return s->safe_reads && lisem_luna_read_safe(offset);
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .icount_read_safe = read_safe,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

void listenai_luna_init(ListenAILUNA *s, Object *owner, MemoryRegion *memory,
                       hwaddr base, uint32_t reset_mask, bool safe_reads,
                       void *opaque,
                       void (*fatal_callback)(void *, const char *))
{
    s->reset_mask = reset_mask;
    s->irq_mask_bits = UINT32_MAX;
    s->opaque = opaque;
    s->fatal = fatal_callback;
    s->safe_reads = safe_reads;
    s->clock_enabled = true;
    s->completion = timer_new_ns(QEMU_CLOCK_VIRTUAL, complete, s);
    const LisemLunaHost host = {
        .abi = LISEM_LUNA_ABI, .opaque = s,
        .read = read_memory, .write = write_memory, .sha256 = sha256,
        .schedule = schedule, .cancel = cancel, .fatal = fatal,
    };
    s->backend = lisem_luna_create(&host);
    if (!s->backend) { fatal(s, "LUNA backend ABI mismatch"); }
    memory_region_init_io(&s->io, owner, &ops, s, "listenai-luna", 0x1000);
    memory_region_add_subregion(memory, base, &s->io);
}

void listenai_luna_reset(ListenAILUNA *s)
{
    cancel(s);
    lisem_luna_reset(s->backend);
    s->irq_mask = 0;
    if (s->reset_extra) { s->reset_extra(s->opaque); }
    update_irq(s);
}

void listenai_luna_clock(ListenAILUNA *s, bool enabled)
{
    if (s->clock_enabled == enabled) { return; }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->pending) {
        if (enabled) {
            timer_mod(s->completion, now + s->remaining);
        } else {
            s->remaining = MAX(0, timer_expire_time_ns(s->completion) - now);
            timer_del(s->completion);
        }
    }
    s->clock_enabled = enabled;
}
