/*
 * QTest accelerator code
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "qemu/accel.h"
#include "accel/accel-ops.h"
#include "accel/accel-cpu-ops.h"
#include "system/qtest.h"
#include "system/cpus.h"
#include "qemu/guest-random.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"

/* A non-executing CPU needs one wait primitive. Switching between sigwait
 * and halt_cond can lose a coalesced kick during repeated pause/resume. All
 * work and waits here are serialized by BQL; no guest instructions execute. */
static void *qtest_cpu_thread(void *opaque)
{
    CPUState *cpu = opaque;
    rcu_register_thread();
    bql_lock();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);
    do {
        qemu_wait_io_event_common(cpu);
        if (!cpu->unplug) {
            qemu_cond_wait_bql(cpu->halt_cond);
        }
    } while (!cpu->unplug);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void qtest_start_cpu(CPUState *cpu)
{
    char name[VCPU_THREAD_NAME_SIZE];
    snprintf(name, sizeof(name), "CPU %d/QTEST", cpu->cpu_index);
    qemu_thread_create(cpu->thread, name, qtest_cpu_thread, cpu,
                       QEMU_THREAD_JOINABLE);
}

static void qtest_kick_cpu(CPUState *cpu)
{
    /* qemu_cpu_kick already broadcasts halt_cond while holding BQL. */
    assert(bql_locked());
}

static int64_t qtest_clock_counter;

static int64_t qtest_get_virtual_clock(void)
{
    return qatomic_read_i64(&qtest_clock_counter);
}

static void qtest_set_virtual_clock(int64_t count)
{
    qatomic_set_i64(&qtest_clock_counter, count);
}

static int qtest_init_accel(AccelState *as, MachineState *ms)
{
    return 0;
}

static void qtest_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "QTest";
    ac->init_machine = qtest_init_accel;
    ac->allowed = &qtest_allowed;
}

#define TYPE_QTEST_ACCEL ACCEL_CLASS_NAME("qtest")

static const TypeInfo qtest_accel_type = {
    .name = TYPE_QTEST_ACCEL,
    .parent = TYPE_ACCEL,
    .class_init = qtest_accel_class_init,
};
module_obj(TYPE_QTEST_ACCEL);

static void qtest_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = qtest_start_cpu;
    ops->kick_vcpu_thread = qtest_kick_cpu;
    ops->get_virtual_clock = qtest_get_virtual_clock;
    ops->set_virtual_clock = qtest_set_virtual_clock;
    ops->handle_interrupt = generic_handle_interrupt;
};

static const TypeInfo qtest_accel_ops_type = {
    .name = ACCEL_OPS_NAME("qtest"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = qtest_accel_ops_class_init,
    .abstract = true,
};
module_obj(ACCEL_OPS_NAME("qtest"));

static void qtest_type_init(void)
{
    type_register_static(&qtest_accel_type);
    type_register_static(&qtest_accel_ops_type);
}

type_init(qtest_type_init);
