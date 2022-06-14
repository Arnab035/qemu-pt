/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/config-file.h"
#include "migration/vmstate.h"
#include "monitor/monitor.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qapi-events-run-state.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "qemu/qemu-print.h"
#include "sysemu/tcg.h"
#include "sysemu/block-backend.h"
#include "exec/gdbstub.h"
#include "sysemu/dma.h"
#include "sysemu/hw_accel.h"
#include "sysemu/kvm.h"
#include "sysemu/hax.h"
#include "sysemu/hvf.h"
#include "sysemu/whpx.h"
#include "exec/exec-all.h"

#include "qemu/thread.h"
#include "qemu/plugin.h"
#include "sysemu/cpus.h"
#include "sysemu/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/bitmap.h"
#include "qemu/seqlock.h"
#include "qemu/guest-random.h"
#include "tcg/tcg.h"
#include "hw/nmi.h"
#include "sysemu/replay.h"
#include "replay/replay-internal.h"
#include "sysemu/runstate.h"
#include "hw/boards.h"

#include "hw/i386/pc.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-blk.h"
#include "index_array_header.h"
#include "hw/hw.h"

#ifdef CONFIG_LINUX

#include <sys/prctl.h>

#ifndef PR_MCE_KILL
#define PR_MCE_KILL 33
#endif

#ifndef PR_MCE_KILL_SET
#define PR_MCE_KILL_SET 1
#endif

#ifndef PR_MCE_KILL_EARLY
#define PR_MCE_KILL_EARLY 1
#endif

#endif /* CONFIG_LINUX */

static QemuMutex qemu_global_mutex;

int64_t max_delay;
int64_t max_advance;

static bool network_replay_done_at_init = false;
static bool disk_replay_done_at_init = false;

/* vcpu throttling controls */
static QEMUTimer *throttle_timer;
static unsigned int throttle_percentage;


#define CPU_THROTTLE_PCT_MIN 1
#define CPU_THROTTLE_PCT_MAX 99
#define CPU_THROTTLE_TIMESLICE_NS 10000000


/* FULL path of Intel PT trace file location */
const char *intel_pt_trace_file_prefix = "/home/arnabjyoti/linux-4.14.3/tools/perf/linux_05may21";

bool cpu_is_stopped(CPUState *cpu)
{
    return cpu->stopped || !runstate_is_running();
}

static bool cpu_thread_is_idle(CPUState *cpu)
{
    if (cpu->stop || cpu->queued_work_first) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return true;
    }
    if (!cpu->halted || cpu_has_work(cpu) ||
        kvm_halt_in_kernel()) {
        return false;
    }
    return true;
}

static bool all_cpu_threads_idle(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!cpu_thread_is_idle(cpu)) {
            return false;
        }
    }
    return true;
}

/***********************************************************/
/* guest cycle counter */

/* Protected by TimersState seqlock */

static bool icount_sleep = true;
/* Arbitrarily pick 1MIPS as the minimum allowable speed.  */
#define MAX_ICOUNT_SHIFT 10

typedef struct TimersState {
    /* Protected by BQL.  */
    int64_t cpu_ticks_prev;
    int64_t cpu_ticks_offset;

    /* Protect fields that can be respectively read outside the
     * BQL, and written from multiple threads.
     */
    QemuSeqLock vm_clock_seqlock;
    QemuSpin vm_clock_lock;

    int16_t cpu_ticks_enabled;

    /* Conversion factor from emulated instructions to virtual clock ticks.  */
    int16_t icount_time_shift;

    /* Compensate for varying guest execution speed.  */
    int64_t qemu_icount_bias;

    int64_t vm_clock_warp_start;
    int64_t cpu_clock_offset;

    /* Only written by TCG thread */
    int64_t qemu_icount;

    /* for adjusting icount */
    QEMUTimer *icount_rt_timer;
    QEMUTimer *icount_vm_timer;
    QEMUTimer *icount_warp_timer;
} TimersState;

static TimersState timers_state;
bool mttcg_enabled;


/* The current number of executed instructions is based on what we
 * originally budgeted minus the current state of the decrementing
 * icount counters in extra/u16.low.
 */
static int64_t cpu_get_icount_executed(CPUState *cpu)
{
    return (cpu->icount_budget -
            (cpu_neg(cpu)->icount_decr.u16.low + cpu->icount_extra));
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
static void cpu_update_icount_locked(CPUState *cpu)
{
    int64_t executed = cpu_get_icount_executed(cpu);
    cpu->icount_budget -= executed;

    atomic_set_i64(&timers_state.qemu_icount,
                   timers_state.qemu_icount + executed);
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
void cpu_update_icount(CPUState *cpu)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    cpu_update_icount_locked(cpu);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static int64_t cpu_get_icount_raw_locked(void)
{
    CPUState *cpu = current_cpu;

    if (cpu && cpu->running) {
        if (!cpu->can_do_io) {
            error_report("Bad icount read");
            exit(1);
        }
        /* Take into account what has run */
        cpu_update_icount_locked(cpu);
    }
    /* The read is protected by the seqlock, but needs atomic64 to avoid UB */
    return atomic_read_i64(&timers_state.qemu_icount);
}

static int64_t cpu_get_icount_locked(void)
{
    int64_t icount = cpu_get_icount_raw_locked();
    return atomic_read_i64(&timers_state.qemu_icount_bias) +
        cpu_icount_to_ns(icount);
}

int64_t cpu_get_icount_raw(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = cpu_get_icount_raw_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

/* Return the virtual CPU time, based on the instruction counter.  */
int64_t cpu_get_icount(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = cpu_get_icount_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

int64_t cpu_icount_to_ns(int64_t icount)
{
    return icount << atomic_read(&timers_state.icount_time_shift);
}

static int64_t cpu_get_ticks_locked(void)
{
    int64_t ticks = timers_state.cpu_ticks_offset;
    if (timers_state.cpu_ticks_enabled) {
        ticks += cpu_get_host_ticks();
    }

    if (timers_state.cpu_ticks_prev > ticks) {
        /* Non increasing ticks may happen if the host uses software suspend.  */
        timers_state.cpu_ticks_offset += timers_state.cpu_ticks_prev - ticks;
        ticks = timers_state.cpu_ticks_prev;
    }

    timers_state.cpu_ticks_prev = ticks;
    return ticks;
}

/* return the time elapsed in VM between vm_start and vm_stop.  Unless
 * icount is active, cpu_get_ticks() uses units of the host CPU cycle
 * counter.
 */
int64_t cpu_get_ticks(void)
{
    int64_t ticks;

    if (use_icount) {
        return cpu_get_icount();
    }

    qemu_spin_lock(&timers_state.vm_clock_lock);
    ticks = cpu_get_ticks_locked();
    qemu_spin_unlock(&timers_state.vm_clock_lock);
    return ticks;
}

static int64_t cpu_get_clock_locked(void)
{
    int64_t time;

    time = timers_state.cpu_clock_offset;
    if (timers_state.cpu_ticks_enabled) {
        time += get_clock();
    }

    return time;
}

/* Return the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
int64_t cpu_get_clock(void)
{
    int64_t ti;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        ti = cpu_get_clock_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return ti;
}

/* enable cpu_get_ticks()
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_enable_ticks(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (!timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset -= cpu_get_host_ticks();
        timers_state.cpu_clock_offset -= get_clock();
        timers_state.cpu_ticks_enabled = 1;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
}

/* disable cpu_get_ticks() : the clock is stopped. You must not call
 * cpu_get_ticks() after that.
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_disable_ticks(void)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset += cpu_get_host_ticks();
        timers_state.cpu_clock_offset = cpu_get_clock_locked();
        timers_state.cpu_ticks_enabled = 0;
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

/* Correlation between real and virtual time is always going to be
   fairly approximate, so ignore small variation.
   When the guest is idle real and virtual time will be aligned in
   the IO wait loop.  */
#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 10)

static void icount_adjust(void)
{
    int64_t cur_time;
    int64_t cur_icount;
    int64_t delta;

    /* Protected by TimersState mutex.  */
    static int64_t last_delta;

    /* If the VM is not running, then do nothing.  */
    if (!runstate_is_running()) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    cur_time = cpu_get_clock_locked();
    cur_icount = cpu_get_icount_locked();

    delta = cur_icount - cur_time;
    /* FIXME: This is a very crude algorithm, somewhat prone to oscillation.  */
    if (delta > 0
        && last_delta + ICOUNT_WOBBLE < delta * 2
        && timers_state.icount_time_shift > 0) {
        /* The guest is getting too far ahead.  Slow time down.  */
        atomic_set(&timers_state.icount_time_shift,
                   timers_state.icount_time_shift - 1);
    }
    if (delta < 0
        && last_delta - ICOUNT_WOBBLE > delta * 2
        && timers_state.icount_time_shift < MAX_ICOUNT_SHIFT) {
        /* The guest is getting too far behind.  Speed time up.  */
        atomic_set(&timers_state.icount_time_shift,
                   timers_state.icount_time_shift + 1);
    }
    last_delta = delta;
    atomic_set_i64(&timers_state.qemu_icount_bias,
                   cur_icount - (timers_state.qemu_icount
                                 << timers_state.icount_time_shift));
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static void icount_adjust_rt(void *opaque)
{
    timer_mod(timers_state.icount_rt_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    icount_adjust();
}

static void icount_adjust_vm(void *opaque)
{
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    icount_adjust();
}

static int64_t qemu_icount_round(int64_t count)
{
    int shift = atomic_read(&timers_state.icount_time_shift);
    return (count + (1 << shift) - 1) >> shift;
}

static void icount_warp_rt(void)
{
    unsigned seq;
    int64_t warp_start;

    /* The icount_warp_timer is rescheduled soon after vm_clock_warp_start
     * changes from -1 to another value, so the race here is okay.
     */
    do {
        seq = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        warp_start = timers_state.vm_clock_warp_start;
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, seq));

    if (warp_start == -1) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (runstate_is_running()) {
        int64_t clock = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                            cpu_get_clock_locked());
        int64_t warp_delta;

        warp_delta = clock - timers_state.vm_clock_warp_start;
        if (use_icount == 2) {
            /*
             * In adaptive mode, do not let QEMU_CLOCK_VIRTUAL run too
             * far ahead of real time.
             */
            int64_t cur_icount = cpu_get_icount_locked();
            int64_t delta = clock - cur_icount;
            warp_delta = MIN(warp_delta, delta);
        }
        atomic_set_i64(&timers_state.qemu_icount_bias,
                       timers_state.qemu_icount_bias + warp_delta);
    }
    timers_state.vm_clock_warp_start = -1;
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);

    if (qemu_clock_expired(QEMU_CLOCK_VIRTUAL)) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

static void icount_timer_cb(void *opaque)
{
    /* No need for a checkpoint because the timer already synchronizes
     * with CHECKPOINT_CLOCK_VIRTUAL_RT.
     */
    icount_warp_rt();
}

void qtest_clock_warp(int64_t dest)
{
    int64_t clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    AioContext *aio_context;
    assert(qtest_enabled());
    aio_context = qemu_get_aio_context();
    while (clock < dest) {
        int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                                      QEMU_TIMER_ATTR_ALL);
        int64_t warp = qemu_soonest_timeout(dest - clock, deadline);

        seqlock_write_lock(&timers_state.vm_clock_seqlock,
                           &timers_state.vm_clock_lock);
        atomic_set_i64(&timers_state.qemu_icount_bias,
                       timers_state.qemu_icount_bias + warp);
        seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                             &timers_state.vm_clock_lock);

        qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
        timerlist_run_timers(aio_context->tlg.tl[QEMU_CLOCK_VIRTUAL]);
        clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
}

void qemu_start_warp_timer(void)
{
    int64_t clock;
    int64_t deadline;

    if (!use_icount) {
        return;
    }

    /* Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    /* warp clock deterministically in record/replay mode */
    /*
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_START)) {
        return;
    }*/
    if (replay_mode != REPLAY_MODE_PLAY) {
        if (!all_cpu_threads_idle()) {
            return;
        }

        if (qtest_enabled()) {
            /* When testing, qtest commands advance icount.  */
            return;
        }

        replay_checkpoint(CHECKPOINT_CLOCK_WARP_START);
    } else {
        /* warp clock deterministically in record/replay mode */
        if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_START)) {
            /* vCPU is sleeping and warp can't be started.
               It is probably a race condition: notification sent
               to vCPU was processed in advance and vCPU went to sleep.
               Therefore we have to wake it up for doing someting. */
            if (replay_has_checkpoint()) {
                qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            }
            return;
        }
    }

    /* We want to use the earliest deadline from ALL vm_clocks */
    clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    if (deadline < 0) {
        static bool notified;
        if (!icount_sleep && !notified) {
            warn_report("icount sleep disabled and no active timers");
            notified = true;
        }
        return;
    }

    if (deadline > 0) {
        /*
         * Ensure QEMU_CLOCK_VIRTUAL proceeds even when the virtual CPU goes to
         * sleep.  Otherwise, the CPU might be waiting for a future timer
         * interrupt to wake it up, but the interrupt never comes because
         * the vCPU isn't running any insns and thus doesn't advance the
         * QEMU_CLOCK_VIRTUAL.
         */
        if (!icount_sleep) {
            /*
             * We never let VCPUs sleep in no sleep icount mode.
             * If there is a pending QEMU_CLOCK_VIRTUAL timer we just advance
             * to the next QEMU_CLOCK_VIRTUAL event and notify it.
             * It is useful when we want a deterministic execution time,
             * isolated from host latencies.
             */
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            atomic_set_i64(&timers_state.qemu_icount_bias,
                           timers_state.qemu_icount_bias + deadline);
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        } else {
            /*
             * We do stop VCPUs and only advance QEMU_CLOCK_VIRTUAL after some
             * "real" time, (related to the time left until the next event) has
             * passed. The QEMU_CLOCK_VIRTUAL_RT clock will do this.
             * This avoids that the warps are visible externally; for example,
             * you will not be sending network packets continuously instead of
             * every 100ms.
             */
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            if (timers_state.vm_clock_warp_start == -1
                || timers_state.vm_clock_warp_start > clock) {
                timers_state.vm_clock_warp_start = clock;
            }
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            timer_mod_anticipate(timers_state.icount_warp_timer,
                                 clock + deadline);
        }
    } else if (deadline == 0) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

static void qemu_account_warp_timer(void)
{
    if (!use_icount || !icount_sleep) {
        return;
    }

    /* Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    /* expect these checkpoints to return FALSE anyway, so this would escape */

    /* warp clock deterministically in record/replay mode */
    /*
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_ACCOUNT)) {
        return;
    }*/

    timer_del(timers_state.icount_warp_timer);
    icount_warp_rt();
}

static bool icount_state_needed(void *opaque)
{
    return use_icount;
}

static bool warp_timer_state_needed(void *opaque)
{
    TimersState *s = opaque;
    return s->icount_warp_timer != NULL;
}

static bool adjust_timers_state_needed(void *opaque)
{
    TimersState *s = opaque;
    return s->icount_rt_timer != NULL;
}

/*
 * Subsection for warp timer migration is optional, because may not be created
 */
static const VMStateDescription icount_vmstate_warp_timer = {
    .name = "timer/icount/warp_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = warp_timer_state_needed,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(vm_clock_warp_start, TimersState),
        VMSTATE_TIMER_PTR(icount_warp_timer, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription icount_vmstate_adjust_timers = {
    .name = "timer/icount/timers",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = adjust_timers_state_needed,
    .fields = (VMStateField[]) {
        VMSTATE_TIMER_PTR(icount_rt_timer, TimersState),
        VMSTATE_TIMER_PTR(icount_vm_timer, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * This is a subsection for icount migration.
 */
static const VMStateDescription icount_vmstate_timers = {
    .name = "timer/icount",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = icount_state_needed,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(qemu_icount_bias, TimersState),
        VMSTATE_INT64(qemu_icount, TimersState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
        &icount_vmstate_warp_timer,
        &icount_vmstate_adjust_timers,
        NULL
    }
};

static const VMStateDescription vmstate_timers = {
    .name = "timer",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(cpu_ticks_offset, TimersState),
        VMSTATE_UNUSED(8),
        VMSTATE_INT64_V(cpu_clock_offset, TimersState, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
        &icount_vmstate_timers,
        NULL
    }
};

static void cpu_throttle_thread(CPUState *cpu, run_on_cpu_data opaque)
{
    double pct;
    double throttle_ratio;
    int64_t sleeptime_ns, endtime_ns;

    if (!cpu_throttle_get_percentage()) {
        return;
    }

    pct = (double)cpu_throttle_get_percentage()/100;
    throttle_ratio = pct / (1 - pct);
    /* Add 1ns to fix double's rounding error (like 0.9999999...) */
    sleeptime_ns = (int64_t)(throttle_ratio * CPU_THROTTLE_TIMESLICE_NS + 1);
    endtime_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + sleeptime_ns;
    while (sleeptime_ns > 0 && !cpu->stop) {
        if (sleeptime_ns > SCALE_MS) {
            qemu_cond_timedwait(cpu->halt_cond, &qemu_global_mutex,
                                sleeptime_ns / SCALE_MS);
        } else {
            qemu_mutex_unlock_iothread();
            g_usleep(sleeptime_ns / SCALE_US);
            qemu_mutex_lock_iothread();
        }
        sleeptime_ns = endtime_ns - qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
    atomic_set(&cpu->throttle_thread_scheduled, 0);
}

static void cpu_throttle_timer_tick(void *opaque)
{
    CPUState *cpu;
    double pct;

    /* Stop the timer if needed */
    if (!cpu_throttle_get_percentage()) {
        return;
    }
    CPU_FOREACH(cpu) {
        if (!atomic_xchg(&cpu->throttle_thread_scheduled, 1)) {
            async_run_on_cpu(cpu, cpu_throttle_thread,
                             RUN_ON_CPU_NULL);
        }
    }

    pct = (double)cpu_throttle_get_percentage()/100;
    timer_mod(throttle_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) +
                                   CPU_THROTTLE_TIMESLICE_NS / (1-pct));
}

void cpu_throttle_set(int new_throttle_pct)
{
    /* Ensure throttle percentage is within valid range */
    new_throttle_pct = MIN(new_throttle_pct, CPU_THROTTLE_PCT_MAX);
    new_throttle_pct = MAX(new_throttle_pct, CPU_THROTTLE_PCT_MIN);

    atomic_set(&throttle_percentage, new_throttle_pct);

    timer_mod(throttle_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) +
                                       CPU_THROTTLE_TIMESLICE_NS);
}

void cpu_throttle_stop(void)
{
    atomic_set(&throttle_percentage, 0);
}

bool cpu_throttle_active(void)
{
    return (cpu_throttle_get_percentage() != 0);
}

int cpu_throttle_get_percentage(void)
{
    return atomic_read(&throttle_percentage);
}

void cpu_ticks_init(void)
{
    seqlock_init(&timers_state.vm_clock_seqlock);
    qemu_spin_init(&timers_state.vm_clock_lock);
    vmstate_register(NULL, 0, &vmstate_timers, &timers_state);
    throttle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                           cpu_throttle_timer_tick, NULL);
}

void configure_icount(QemuOpts *opts, Error **errp)
{
    const char *option;
    char *rem_str = NULL;

    option = qemu_opt_get(opts, "shift");
    if (!option) {
        if (qemu_opt_get(opts, "align") != NULL) {
            error_setg(errp, "Please specify shift option when using align");
        }
        return;
    }

    icount_sleep = qemu_opt_get_bool(opts, "sleep", true);
    if (icount_sleep) {
        timers_state.icount_warp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                         icount_timer_cb, NULL);
    }

    icount_align_option = qemu_opt_get_bool(opts, "align", false);

    if (icount_align_option && !icount_sleep) {
        error_setg(errp, "align=on and sleep=off are incompatible");
    }
    if (strcmp(option, "auto") != 0) {
        errno = 0;
        timers_state.icount_time_shift = strtol(option, &rem_str, 0);
        if (errno != 0 || *rem_str != '\0' || !strlen(option)) {
            error_setg(errp, "icount: Invalid shift value");
        }
        use_icount = 1;
        return;
    } else if (icount_align_option) {
        error_setg(errp, "shift=auto and align=on are incompatible");
    } else if (!icount_sleep) {
        error_setg(errp, "shift=auto and sleep=off are incompatible");
    }

    use_icount = 2;

    /* 125MIPS seems a reasonable initial guess at the guest speed.
       It will be corrected fairly quickly anyway.  */
    timers_state.icount_time_shift = 3;

    /* Have both realtime and virtual time triggers for speed adjustment.
       The realtime trigger catches emulated time passing too slowly,
       the virtual time trigger catches emulated time passing too fast.
       Realtime triggers occur even when idle, so use them less frequently
       than VM triggers.  */
    timers_state.vm_clock_warp_start = -1;
    timers_state.icount_rt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL_RT,
                                   icount_adjust_rt, NULL);
    timer_mod(timers_state.icount_rt_timer,
                   qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    timers_state.icount_vm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        icount_adjust_vm, NULL);
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
}

/***********************************************************/
/* TCG vCPU kick timer
 *
 * The kick timer is responsible for moving single threaded vCPU
 * emulation on to the next vCPU. If more than one vCPU is running a
 * timer event with force a cpu->exit so the next vCPU can get
 * scheduled.
 *
 * The timer is removed if all vCPUs are idle and restarted again once
 * idleness is complete.
 */

static QEMUTimer *tcg_kick_vcpu_timer;
static CPUState *tcg_current_rr_cpu;

#define TCG_KICK_PERIOD (NANOSECONDS_PER_SECOND / 10)

static inline int64_t qemu_tcg_next_kick(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TCG_KICK_PERIOD;
}

/* Kick the currently round-robin scheduled vCPU to next */
static void qemu_cpu_kick_rr_next_cpu(void)
{
    CPUState *cpu;
    do {
        cpu = atomic_mb_read(&tcg_current_rr_cpu);
        if (cpu) {
            cpu_exit(cpu);
        }
    } while (cpu != atomic_mb_read(&tcg_current_rr_cpu));
}

/* Kick all RR vCPUs */
static void qemu_cpu_kick_rr_cpus(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_exit(cpu);
    };
}

static void do_nothing(CPUState *cpu, run_on_cpu_data unused)
{
}

void qemu_timer_notify_cb(void *opaque, QEMUClockType type)
{
    if (!use_icount || type != QEMU_CLOCK_VIRTUAL) {
        qemu_notify_event();
        return;
    }

    if (qemu_in_vcpu_thread()) {
        /* A CPU is currently running; kick it back out to the
         * tcg_cpu_exec() loop so it will recalculate its
         * icount deadline immediately.
         */
        qemu_cpu_kick(current_cpu);
    } else if (first_cpu) {
        /* qemu_cpu_kick is not enough to kick a halted CPU out of
         * qemu_tcg_wait_io_event.  async_run_on_cpu, instead,
         * causes cpu_thread_is_idle to return false.  This way,
         * handle_icount_deadline can run.
         * If we have no CPUs at all for some reason, we don't
         * need to do anything.
         */
        async_run_on_cpu(first_cpu, do_nothing, RUN_ON_CPU_NULL);
    }
}

static void kick_tcg_thread(void *opaque)
{
    timer_mod(tcg_kick_vcpu_timer, qemu_tcg_next_kick());
    qemu_cpu_kick_rr_next_cpu();
}

static void start_tcg_kick_timer(void)
{
    assert(!mttcg_enabled);
    if (!tcg_kick_vcpu_timer && CPU_NEXT(first_cpu)) {
        tcg_kick_vcpu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           kick_tcg_thread, NULL);
    }
    if (tcg_kick_vcpu_timer && !timer_pending(tcg_kick_vcpu_timer)) {
        timer_mod(tcg_kick_vcpu_timer, qemu_tcg_next_kick());
    }
}

static void stop_tcg_kick_timer(void)
{
    assert(!mttcg_enabled);
    if (tcg_kick_vcpu_timer && timer_pending(tcg_kick_vcpu_timer)) {
        timer_del(tcg_kick_vcpu_timer);
    }
}

/***********************************************************/
void hw_error(const char *fmt, ...)
{
    va_list ap;
    CPUState *cpu;

    va_start(ap, fmt);
    fprintf(stderr, "qemu: hardware error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    CPU_FOREACH(cpu) {
        fprintf(stderr, "CPU #%d:\n", cpu->cpu_index);
        cpu_dump_state(cpu, stderr, CPU_DUMP_FPU);
    }
    va_end(ap);
    abort();
}

void cpu_synchronize_all_states(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_state(cpu);
        /* TODO: move to cpu_synchronize_state() */
        if (hvf_enabled()) {
            hvf_cpu_synchronize_state(cpu);
        }
    }
}

void cpu_synchronize_all_post_reset(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_reset(cpu);
        /* TODO: move to cpu_synchronize_post_reset() */
        if (hvf_enabled()) {
            hvf_cpu_synchronize_post_reset(cpu);
        }
    }
}

void cpu_synchronize_all_post_init(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_init(cpu);
        /* TODO: move to cpu_synchronize_post_init() */
        if (hvf_enabled()) {
            hvf_cpu_synchronize_post_init(cpu);
        }
    }
}

void cpu_synchronize_all_pre_loadvm(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_pre_loadvm(cpu);
    }
}

static int do_vm_stop(RunState state, bool send_stop)
{
    int ret = 0;

    if (runstate_is_running()) {
        runstate_set(state);
        cpu_disable_ticks();
        pause_all_vcpus();
        vm_state_notify(0, state);
        if (send_stop) {
            qapi_event_send_stop();
        }
    }

    bdrv_drain_all();
    ret = bdrv_flush_all();

    return ret;
}

/* Special vm_stop() variant for terminating the process.  Historically clients
 * did not expect a QMP STOP event and so we need to retain compatibility.
 */
int vm_shutdown(void)
{
    return do_vm_stop(RUN_STATE_SHUTDOWN, false);
}

static bool cpu_can_run(CPUState *cpu)
{
    if (cpu->stop) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return false;
    }
    return true;
}

static void cpu_handle_guest_debug(CPUState *cpu)
{
    gdb_set_stop_cpu(cpu);
    qemu_system_debug_request();
    cpu->stopped = true;
}

#ifdef CONFIG_LINUX
static void sigbus_reraise(void)
{
    sigset_t set;
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    if (!sigaction(SIGBUS, &action, NULL)) {
        raise(SIGBUS);
        sigemptyset(&set);
        sigaddset(&set, SIGBUS);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    }
    perror("Failed to re-raise SIGBUS!\n");
    abort();
}

static void sigbus_handler(int n, siginfo_t *siginfo, void *ctx)
{
    if (siginfo->si_code != BUS_MCEERR_AO && siginfo->si_code != BUS_MCEERR_AR) {
        sigbus_reraise();
    }

    if (current_cpu) {
        /* Called asynchronously in VCPU thread.  */
        if (kvm_on_sigbus_vcpu(current_cpu, siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    } else {
        /* Called synchronously (via signalfd) in main thread.  */
        if (kvm_on_sigbus(siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    }
}

static void qemu_init_sigbus(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = sigbus_handler;
    sigaction(SIGBUS, &action, NULL);

    prctl(PR_MCE_KILL, PR_MCE_KILL_SET, PR_MCE_KILL_EARLY, 0, 0);
}
#else /* !CONFIG_LINUX */
static void qemu_init_sigbus(void)
{
}
#endif /* !CONFIG_LINUX */

static QemuThread io_thread;

/* cpu creation */
static QemuCond qemu_cpu_cond;
/* system init */
static QemuCond qemu_pause_cond;

void qemu_init_cpu_loop(void)
{
    qemu_init_sigbus();
    qemu_cond_init(&qemu_cpu_cond);
    qemu_cond_init(&qemu_pause_cond);
    qemu_mutex_init(&qemu_global_mutex);

    qemu_thread_get_self(&io_thread);
}

void run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    do_run_on_cpu(cpu, func, data, &qemu_global_mutex);
}

static void qemu_kvm_destroy_vcpu(CPUState *cpu)
{
    if (kvm_destroy_vcpu(cpu) < 0) {
        error_report("kvm_destroy_vcpu failed");
        exit(EXIT_FAILURE);
    }
}

static void qemu_tcg_destroy_vcpu(CPUState *cpu)
{
}

static void qemu_cpu_stop(CPUState *cpu, bool exit)
{
    g_assert(qemu_cpu_is_self(cpu));
    cpu->stop = false;
    cpu->stopped = true;
    if (exit) {
        cpu_exit(cpu);
    }
    qemu_cond_broadcast(&qemu_pause_cond);
}

static void qemu_wait_io_event_common(CPUState *cpu)
{
    atomic_mb_set(&cpu->thread_kicked, false);
    if (cpu->stop) {
        qemu_cpu_stop(cpu, false);
    }
    process_queued_cpu_work(cpu);
}

static void qemu_tcg_rr_wait_io_event(void)
{
    CPUState *cpu;

    while (all_cpu_threads_idle()) {
        if (arnab_replay_mode != REPLAY_MODE_PLAY)
            stop_tcg_kick_timer();
        qemu_cond_wait(first_cpu->halt_cond, &qemu_global_mutex);
    }

    if (arnab_replay_mode != REPLAY_MODE_PLAY)
        start_tcg_kick_timer();

    CPU_FOREACH(cpu) {
        qemu_wait_io_event_common(cpu);
    }
}

static void qemu_wait_io_event(CPUState *cpu)
{
    bool slept = false;

    while (cpu_thread_is_idle(cpu)) {
        if (!slept) {
            slept = true;
            qemu_plugin_vcpu_idle_cb(cpu);
        }
        qemu_cond_wait(cpu->halt_cond, &qemu_global_mutex);
    }
    if (slept) {
        qemu_plugin_vcpu_resume_cb(cpu);
    }

#ifdef _WIN32
    /* Eat dummy APC queued by qemu_cpu_kick_thread.  */
    if (!tcg_enabled()) {
        SleepEx(0, TRUE);
    }
#endif
    qemu_wait_io_event_common(cpu);
}

static void *qemu_kvm_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    current_cpu = cpu;

    r = kvm_init_vcpu(cpu);
    if (r < 0) {
        error_report("kvm_init_vcpu failed: %s", strerror(-r));
        exit(1);
    }

    kvm_init_cpu_signals(cpu);

    /* signal CPU creation */
    cpu->created = true;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        if (cpu_can_run(cpu)) {
            r = kvm_cpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
        qemu_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    qemu_kvm_destroy_vcpu(cpu);
    cpu->created = false;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_mutex_unlock_iothread();
    rcu_unregister_thread();
    return NULL;
}

static void *qemu_dummy_cpu_thread_fn(void *arg)
{
#ifdef _WIN32
    error_report("qtest is not supported under Windows");
    exit(1);
#else
    CPUState *cpu = arg;
    sigset_t waitset;
    int r;

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    current_cpu = cpu;

    sigemptyset(&waitset);
    sigaddset(&waitset, SIG_IPI);

    /* signal CPU creation */
    cpu->created = true;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        qemu_mutex_unlock_iothread();
        do {
            int sig;
            r = sigwait(&waitset, &sig);
        } while (r == -1 && (errno == EAGAIN || errno == EINTR));
        if (r == -1) {
            perror("sigwait");
            exit(1);
        }
        qemu_mutex_lock_iothread();
        qemu_wait_io_event(cpu);
    } while (!cpu->unplug);

    qemu_mutex_unlock_iothread();
    rcu_unregister_thread();
    return NULL;
#endif
}

static int64_t tcg_get_icount_limit(void)
{
    int64_t deadline;

    //if (replay_mode != REPLAY_MODE_PLAY) {
        /*
         * Include all the timers, because they may need an attention.
         * Too long CPU execution may create unnecessary delay in UI.
         */
        deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                              QEMU_TIMER_ATTR_ALL);
        /* Check realtime timers, because they help with input processing */
        deadline = qemu_soonest_timeout(deadline,
                qemu_clock_deadline_ns_all(QEMU_CLOCK_REALTIME,
                                           QEMU_TIMER_ATTR_ALL));

        /* Maintain prior (possibly buggy) behaviour where if no deadline
         * was set (as there is no QEMU_CLOCK_VIRTUAL timer) or it is more than
         * INT32_MAX nanoseconds ahead, we still use INT32_MAX
         * nanoseconds. */

    if ((deadline < 0) || (deadline > INT32_MAX)) {
        deadline = INT32_MAX;
    }

    return qemu_icount_round(deadline);
    // else {
       // return replay_get_instructions();   // expected to always return 0, comment it out
    //}
}


static void handle_icount_deadline(void)
{
    assert(qemu_in_vcpu_thread());
    if (use_icount) {
        int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                                      QEMU_TIMER_ATTR_ALL);

        if (deadline == 0) {
            /* Wake up other AioContexts.  */
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
        }
    }
}

static void prepare_icount_for_run(CPUState *cpu)
{
    if (use_icount) {
        int insns_left;

        /* These should always be cleared by process_icount_data after
         * each vCPU execution. However u16.high can be raised
         * asynchronously by cpu_exit/cpu_interrupt/tcg_handle_interrupt
         */
        g_assert(cpu_neg(cpu)->icount_decr.u16.low == 0);
        g_assert(cpu->icount_extra == 0);

        cpu->icount_budget = tcg_get_icount_limit();
        insns_left = MIN(0xffff, cpu->icount_budget);
        cpu_neg(cpu)->icount_decr.u16.low = insns_left;
        cpu->icount_extra = cpu->icount_budget - insns_left;

        replay_mutex_lock();
    }
}

static void process_icount_data(CPUState *cpu)
{
    if (use_icount) {
        /* Account for executed instructions */
        cpu_update_icount(cpu);

        /* Reset the counters */
        cpu_neg(cpu)->icount_decr.u16.low = 0;
        cpu->icount_extra = 0;
        cpu->icount_budget = 0;

        replay_account_executed_instructions();

        replay_mutex_unlock();
    }
}

/* preprocess_tip_array - preprocesses the tip array so that truncated addresses contain the fully-qualified address */

static void construct_fully_qualified_address(CPUState *cpu, int i, char *reference_address) {
    int j, chars_to_copy = 0;
    int short_length = 0;

    if(cpu->tip_addresses[i].ip_bytes==4) {   // some other value
      //chars_to_copy=12-strlen(tip_addresses[i].address);
        chars_to_copy=strlen(reference_address)-strlen(cpu->tip_addresses[i].address);
        if(chars_to_copy < 0) {
            chars_to_copy=0;
        }

        //short_length=8-strlen(tip_addresses[i].address);
        cpu->tip_addresses[i].address=realloc(cpu->tip_addresses[i].address,13);
        for(j=strlen(cpu->tip_addresses[i].address)-1; j>=0; j--) {
            cpu->tip_addresses[i].address[j+chars_to_copy]=cpu->tip_addresses[i].address[j];
        }
        if (chars_to_copy > 4) {
            for(j=0; j<chars_to_copy-4; j++) {
                cpu->tip_addresses[i].address[j+4] = '0';
            }
            chars_to_copy = 4;
        }
        for(j=0;j<chars_to_copy;j++) {
            cpu->tip_addresses[i].address[j] = reference_address[j];
        }
        cpu->tip_addresses[i].address[12]='\0';
    }
    else if(cpu->tip_addresses[i].ip_bytes==2) {
        if(strlen(reference_address)==6) {
            if(strlen(cpu->tip_addresses[i].address) < 4) {
                short_length = 4-strlen(cpu->tip_addresses[i].address);
                chars_to_copy=strlen(reference_address)-strlen(cpu->tip_addresses[i].address)-short_length;
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
            else if(strlen(cpu->tip_addresses[i].address)==4) {
                short_length=0;
                chars_to_copy=strlen(reference_address)-strlen(cpu->tip_addresses[i].address);
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
        }
        else {
            if(strlen(cpu->tip_addresses[i].address) < 4) {
                short_length = 4-strlen(cpu->tip_addresses[i].address);
                chars_to_copy = strlen(reference_address)-strlen(cpu->tip_addresses[i].address)-short_length;
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
            else if(strlen(cpu->tip_addresses[i].address)==4) {
                short_length = 0;
                chars_to_copy=strlen(reference_address)-strlen(cpu->tip_addresses[i].address);
                if(chars_to_copy<0) {
                    chars_to_copy=0;
                }
            }
        }
        cpu->tip_addresses[i].address=realloc(cpu->tip_addresses[i].address,13);

        for(j=strlen(cpu->tip_addresses[i].address)-1; j>=0; j--) {
            cpu->tip_addresses[i].address[j+chars_to_copy+short_length]=cpu->tip_addresses[i].address[j];
        }

        for(j=0;j<chars_to_copy;j++) {
            cpu->tip_addresses[i].address[j]=reference_address[j];
        }
        if(short_length) {
            for(j=0;j<short_length;j++) {
                cpu->tip_addresses[i].address[chars_to_copy+j]='0';
            }
        }
        cpu->tip_addresses[i].address[12]='\0';
    }
}

static void preprocess_tip_array(CPUState *cpu, int size) {

    if (cpu->last_tip_address) {
        construct_fully_qualified_address(cpu, 0, cpu->last_tip_address);
    }
    int i;
    for(i=1;i<=size;i++) {
        construct_fully_qualified_address(cpu, i, cpu->tip_addresses[i-1].address);
    }
}

/* find_newline_and_copy(char *buffer, int pos, int end, char *copy)
 *    - copies characters into copy till a newline
 *    - returns number of characters copied
 */

int find_newline_and_copy(char *buffer, int pos, int end, char *copy) {
    int i = 0;
    int count = 0;
    while(buffer[pos+i] != '\n' && pos+i <= end) {
        copy[i] = buffer[pos+i];
        count++;
        i++;
    }
    if(pos+i > end) return -1;
    copy[i] = '\0'; return count;
}

static int find_first_tsc_index(CPUState *cpu, int count) {
    int i = 0;
    for( i=0; i < count; i++) {
        if (cpu->tnt_array[i] == 'S')
            return i;
    }
    return -1;
}

/*
 * this function pre-computes all of the TSC values from oher clock sources
 * it is possible that some of the values calculated are not very accurate
 * we will use MTC packets in IntelPT and use the algorithm to compute
 * described in the Intel Software Developer Manual(Volume III).
 */

static void precompute_tsc_values(CPUState *cpu, int count, unsigned long last_tsc_value,
                                    bool last_is_last_tsc,
                                    int last_tma_ctc_value,
				    int last_mtc_payload) {
    /* first find TSC and get TMA packet */
    int i = 0;
    int j = 0;
    int tsc_index = 0;
    int tma_ctc_value = 0;
    unsigned long tsc_value = 0;
    bool is_last_tsc = false;
    int number_of_crystal_clocks_passed = 0; // we expect it to be bounded
    int computed_tsc_index = 0;
    /* http://halobates.de/blog/p/432 */
    int mtcfreq = 3;
    int mtc_index = 0;
    int mtc_next;
    int mtc_payload = 0, prev_mtc_payload = 0;

    if (!cpu->computed_tsc_values) {
        if (!last_tsc_value) {
            cpu->computed_tsc_values = malloc(1 * sizeof(unsigned long));
            if (!cpu->computed_tsc_values) {
                printf("Running out of memory while "
                      "allocating computed TSC values from TMA/MTC\n");
                exit(EXIT_FAILURE);
            }
            i = find_first_tsc_index(cpu, count);
            if (i == -1) {
                /* most likely there is no TSC packet, exit */
                return;
            }
            tsc_value = do_strtoul(cpu->tsc_values[tsc_index].tsc_value);
            cpu->computed_tsc_values[computed_tsc_index] = tsc_value;
            computed_tsc_index += 1;
            tma_ctc_value = do_strtoul(cpu->tsc_values[tsc_index].tma_ctc_value);
            is_last_tsc = true;
            tsc_index += 1;
        } else {
            /*
	     * we need to maintain this because IntelPT processing happens
	     * in stages now
	     */
            is_last_tsc = last_is_last_tsc;
            tma_ctc_value = last_tma_ctc_value;
        }
    }
    for (j = i; j < count; j++) {
        /* found MTC */
        if (cpu->tnt_array[j] == 'M') {
            /* next MTC after a TSC */
            cpu->computed_tsc_values = realloc(cpu->computed_tsc_values, computed_tsc_index+1);
            if (!cpu->computed_tsc_values) {
                printf("Running out of memory while "
			"allocating computed TSC values array");
                exit(EXIT_FAILURE);
            }
            if (is_last_tsc) {
                tma_ctc_value &= 0xffff;
                /*
		 * number of crystal clocks passed since next MTC
		 * is given by
		 * CTC[15:0] = CTC[15:0](next) - TMA.CTC[15:0]
		 * where CTC[15:0](next) = MTC(payload) << MTCFrequency
                 */
                mtc_payload = do_strtoul(cpu->mtc_values[mtc_index].mtc_value);
                mtc_next = mtc_payload << mtcfreq;
                number_of_crystal_clocks_passed = (mtc_next & 0xffff) - tma_ctc_value;
                is_last_tsc = false;
            }
            /*
	     * between two MTC packets
	     * A and B, number of crystal clock cycles
	     * passed is calculated from the 8-bit payloads
	     * of respective MTC packets
	     * (CTCB - CTCA) where CTCi = MTCi[15:8] << MTCFrequency
	     * and i = A, B.
	     */
            else {
                mtc_payload = do_strtoul(cpu->mtc_values[mtc_index].mtc_value);
                if (mtc_index > 0) {
                    prev_mtc_payload = do_strtoul(cpu->mtc_values[mtc_index - 1].mtc_value);
                } else {
                    prev_mtc_payload = last_mtc_payload;
                }
                number_of_crystal_clocks_passed = ((mtc_payload & 0xff00) -
				              (prev_mtc_payload & 0xff00)) << mtcfreq;
            }
            tsc_value = cpu->computed_tsc_values[computed_tsc_index-1] +
                          (number_of_crystal_clocks_passed * (150/2)); // 150/2 is ratio of frequencies
                                                                      // we got this doing cpuid
            mtc_index += 1;
            cpu->computed_tsc_values[computed_tsc_index] = tsc_value;
            computed_tsc_index += 1;
        }
        // if you found a TSC value, put it into the array anyway
	else if (cpu->tnt_array[j] == 'S') {
            is_last_tsc = true;
            tsc_value = do_strtoul(cpu->tsc_values[tsc_index].tsc_value);
            cpu->computed_tsc_values[computed_tsc_index] = tsc_value;
            computed_tsc_index += 1;
            tsc_index += 1;
        }
    }
    cpu->last_tsc_value = tsc_value;
    cpu->last_mtc_payload = mtc_payload;
    cpu->last_is_last_tsc = is_last_tsc;
    cpu->last_tma_ctc_value = tma_ctc_value;
}

/*
 * TMA pkt looks like this - TMA CTC 0x8618 FC 0x48
 * we find the position of 'F', return it
 *
 */

static int location_of_fc(char *copy_str) {
    int i = 0;
    int count_of_spaces = 0;

    if (!copy_str)
        return i;

    while(copy_str[i] != '\n') {
        if (copy_str[i] == ' ')
            count_of_spaces++;
        if (count_of_spaces == 3)
            break;
        i++;
    }

    return i+1;
}

/*  get_array_of_tnt_bits()
 *  parameters : none
 *  returns : the array containing the TNT bits
 *  also maintains 2 arrays - one having the TIP addresses with some metadata
 *  the other being the FUP addresses with metadata
 *  use the gzlib standard library
 */

void get_array_of_tnt_bits(CPUState *cpu) {
    char *pch;
    char *pch_pip;
    bool stop_parsing_due_to_heartbeat = false;
    bool stop_parsing_due_to_overflow = false;
    //int len;

    int is_ignore_tip = 0;
    int is_ignore_pip = 0;
    int count_fup_after_ovf = 0;
    unsigned long long k, prev_count;
    unsigned long long j;
    int max_lines_read = 3000000, curr_lines_read = 0;
    int cpu_index = cpu->cpu_index;

    char filename[100] = {'\0'};

    sprintf(filename, "%s_%d.txt.gz", intel_pt_trace_file_prefix, cpu_index);
    if (!cpu->tnt_array) {
        cpu->tnt_array = malloc(1);
    }

    //tnt_array[0] = 'P';
    if (!cpu->intel_pt_file) {
        cpu->intel_pt_file = gzopen(filename, "r");
    }
    cpu->tnt_index_limit = 0;

    int count = 0;

    int count_tip = 0;
    int count_fup = 0;
    int count_tsc = 0;
    int count_mtc = 0;

    cpu->tip_addresses = malloc(1 * sizeof(struct tip_address_info));
    cpu->fup_addresses = malloc(1 * sizeof(struct fup_address_info));
    cpu->tsc_values = malloc(1 * sizeof(struct tsc_counter_info));
    /* we are going to store values of MTC now */
    cpu->mtc_values = malloc(1 * sizeof(struct mtc_timer_info));

    if(!cpu->intel_pt_file) {
        fprintf(stderr, "gzopen of %s failed.\n", filename);
        exit(EXIT_FAILURE);
    }

    char copy[50];
    while(1) {
        if(gzgets(cpu->intel_pt_file, copy, 50) != 0) {
            copy[strcspn(copy, "\n")] = 0;
            curr_lines_read += 1;
        } else {
            printf("Incorrect read from gz file. Simulation probably finished...\n");
            cpu->is_core_simulation_finished = true;
            break;
        }
        //pos = find_newline_and_copy(buffer, start, bytes_read, copy+remainder);
        if (strncmp(copy, "PSBEND", 6) == 0) {
            stop_parsing_due_to_heartbeat = false;
            continue;
        }
        else if (strncmp(copy, "PSB", 3) == 0) {
            stop_parsing_due_to_heartbeat = true;
            continue;
        }
        else if (strncmp(copy, "OVF", 3) == 0) {
            stop_parsing_due_to_overflow = true;
            continue;
        }
        if (stop_parsing_due_to_overflow) {
            if (strncmp(copy, "FUP", 3) == 0) {
                if (strncmp(copy+6, "ffffc", 5) == 0) {
                    count_fup_after_ovf += 1;
                    if (stop_parsing_due_to_heartbeat) {
                        stop_parsing_due_to_heartbeat = false;
                    }
                } else {
                    /* this is a useful TIP, not FUP packet */
                    cpu->tnt_array = realloc(cpu->tnt_array, count+1);
                    if (!cpu->tnt_array) {
                        printf("Running out of memory while allocating TNT array\n");
                        exit(EXIT_FAILURE);
                    }
                    cpu->tnt_array[count] = 'P';
                    count++;
                    cpu->tip_addresses = realloc(cpu->tip_addresses, (count_tip+1)*sizeof(struct tip_address_info));
                    cpu->tip_addresses[count_tip].address = malloc(strlen(copy+6)-3 * sizeof(char));
                    if (!cpu->tip_addresses[count_tip].address) {
                        printf("Running out of memory. We allocated %d packets\n", count_tip);
                        exit(EXIT_FAILURE);
                    }
                    memcpy(cpu->tip_addresses[count_tip].address, copy+6, strlen(copy+6)-3);
                    cpu->tip_addresses[count_tip].address[strlen(copy+6)-3] = '\0';
                    cpu->tip_addresses[count_tip].is_useful=1;
	                /* ip bytes appear in the trace as "TIP 0x40184c 6d" here 6 is the IP Bytes */
                    cpu->tip_addresses[count_tip].ip_bytes=6;
                    count_tip++;
                    stop_parsing_due_to_overflow = false;
                }
            }
            if (count_fup_after_ovf == 2) {
                count_fup_after_ovf = 0;
                stop_parsing_due_to_overflow = false;
            }
            continue;
        }
        if (!stop_parsing_due_to_heartbeat) {
	    if (strncmp(copy, "TNT", 3) == 0) {
                if(is_ignore_tip == 1) {
	            is_ignore_tip = 0;
	        }
	        pch = strchr(copy, '(');
	        prev_count = count;
	        count += ((*++pch) - '0');
	        cpu->tnt_array = realloc(cpu->tnt_array, count);
                if (!cpu->tnt_array) {
                    printf("Running out of memory while allocating TNT array\n");
                    exit(EXIT_FAILURE);
                }
	        for(j=prev_count,k=0; j<count; j++, k++) {
	            cpu->tnt_array[j]=copy[4+k];
	        }
            }
            else if(strncmp(copy, "PIP", 3) == 0) {
                pch_pip = strchr(copy, '=');
	  /* VMEXIT */
	        if((*++pch_pip - '0') == 0) {
	    // the next PIP bit should be (NR = 1) - you need to ignore those PIP bits
	    // only stray PIP (NR=1) packets should be considered and stored
	    // these stray PIP packets indicate context switch events
	            is_ignore_pip = 1;
            // the FUP preceding this PIP will represent the source address for a VMEXIT
                    cpu->fup_addresses[count_fup-1].type = 'V';
	        }
	    /* VMENTRY */
	        else {
                    if (is_ignore_pip == 0 && curr_lines_read >= 5) {
                        continue;
                    }
                    is_ignore_tip = 1;
	            if(is_ignore_pip == 1) {
	                is_ignore_pip = 0;
	            }
	        }
            }
            else {
                if(strncmp(copy, "TIP", 3) == 0) {
	            if(is_ignore_tip == 0) {
	                cpu->tnt_array = realloc(cpu->tnt_array, count+1);
                        if (!cpu->tnt_array) {
                            printf("Running out of memory while allocating TNT array\n");
                            exit(EXIT_FAILURE);
                        }
	                cpu->tnt_array[count] = 'P';
	                count++;
	                // enter TIP addresses into global tip_address_array //
	                cpu->tip_addresses = realloc(cpu->tip_addresses, (count_tip+1)*sizeof(struct tip_address_info));
                        if (!cpu->tip_addresses) {
                            printf("Running out of memory while storing TIP addresses\n");
                            exit(EXIT_FAILURE);
                        }
	                cpu->tip_addresses[count_tip].address = malloc(strlen(copy+6)-3 * sizeof(char));
                        if (!cpu->tip_addresses[count_tip].address) {
                            printf("Running out of memory while storing address of TIP packet\n");
                            exit(EXIT_FAILURE);
                        }
	                memcpy(cpu->tip_addresses[count_tip].address, copy+6, strlen(copy+6)-3);
	                cpu->tip_addresses[count_tip].address[strlen(copy+6)-3] = '\0';
	                cpu->tip_addresses[count_tip].is_useful=1;
	                /* ip bytes appear in the trace as "TIP 0x40184c 6d" here 6 is the IP Bytes */
	                cpu->tip_addresses[count_tip].ip_bytes=copy[strlen(copy)-2]-'0';
	                count_tip++;
	            }
	            else {
	                cpu->tip_addresses = realloc(cpu->tip_addresses, (count_tip+1)*sizeof(struct tip_address_info));
                        if (!cpu->tip_addresses) {
                            printf("Running out of memory while stroing TIP addresses\n");
                            exit(EXIT_FAILURE);
                        }
	                cpu->tip_addresses[count_tip].address = malloc(strlen(copy+6)*sizeof(char));
                        if (!cpu->tip_addresses[count_tip].address) {
                            printf("Running out of memory while storing address of TIP packet\n");
                            exit(EXIT_FAILURE);
                        }
	                memcpy(cpu->tip_addresses[count_tip].address,copy+6,strlen(copy+6)-3);
	                cpu->tip_addresses[count_tip].address[strlen(copy+6)-3] = '\0';
	                cpu->tip_addresses[count_tip].is_useful=0;
	                cpu->tip_addresses[count_tip].ip_bytes=copy[strlen(copy)-2]-'0';
	                count_tip++;
                        //printf("count: %llu\n", count);
	                is_ignore_tip=0;
                    }
	        }
	        else if(strncmp(copy, "FUP", 3) == 0) {
                    cpu->tnt_array = realloc(cpu->tnt_array, count+1);
                    if (!cpu->tnt_array) {
                        printf("Running out of memory while storing TNT packets\n");
                        exit(EXIT_FAILURE);
                    }
                    cpu->tnt_array[count] = 'F';
                    count++;
                    cpu->fup_addresses = realloc(cpu->fup_addresses, (count_fup+1)*sizeof(struct fup_address_info));
                    if (!cpu->fup_addresses) {
                        printf("Running out of memory while storing FUP addresses\n");
                        exit(EXIT_FAILURE);
                    }
                    cpu->fup_addresses[count_fup].address = malloc(strlen(copy+6)-3 * sizeof(char));
                    if (!cpu->fup_addresses[count_fup].address) {
                        printf("Running out of memory while storing address of FUP packet\n");
                        exit(EXIT_FAILURE);
                    }
                    memcpy(cpu->fup_addresses[count_fup].address, copy+6, strlen(copy+6)-3);
                    cpu->fup_addresses[count_fup].address[strlen(copy+6)-3] = '\0';
                    cpu->fup_addresses[count_fup].type = 'I';
                    count_fup++;
                }
                else if (strncmp(copy, "MTC", 3) == 0) {
                    /* store MTC values */
                    cpu->tnt_array = realloc(cpu->tnt_array, count+1);
                    if (!cpu->tnt_array) {
                        printf("Running out of memory while storing TNT packets\n");
                        exit(EXIT_FAILURE);
                    }
                    cpu->tnt_array[count] = 'M';
                    count++;
                    cpu->mtc_values = realloc(cpu->mtc_values, (count_mtc+1)*sizeof(struct mtc_timer_info));
                    if (!cpu->mtc_values) {
                        printf("Running out of memory while storing MTC packets\n");
                        exit(EXIT_FAILURE);
                    }
                    cpu->mtc_values[count_mtc].mtc_value = malloc(strlen(copy+6) * sizeof(char));
                    if (!cpu->mtc_values[count_mtc].mtc_value) {
                        printf("Running out of memory while storing counter values of MTC\n");
                        exit(EXIT_FAILURE);
                    }
                    memcpy(cpu->mtc_values[count_mtc].mtc_value, copy+6, strlen(copy+6));
                    cpu->mtc_values[count_mtc].mtc_value[strlen(copy+6)] = '\0';
                    count_mtc++;
                }
            }
	} else {
            /* TSC packets are present between a PSB and PSBEND */
            if (strncmp(copy, "TSC", 3) == 0) {
                cpu->tnt_array = realloc(cpu->tnt_array, count+1);
                if (!cpu->tnt_array) {
                    printf("Running out of memory while storing packets in TNT array\n");
                    exit(EXIT_FAILURE);
                }
                cpu->tnt_array[count] = 'S';
                count++;
                cpu->tsc_values = realloc(cpu->tsc_values, (count_tsc+1) * sizeof(struct tsc_counter_info));
                if (!cpu->tsc_values) {
                    printf("Running out of memory while storing TSC packets\n");
                }
                cpu->tsc_values[count_tsc].tsc_value = malloc(strlen(copy+6) * sizeof(char));
                if (!cpu->tsc_values[count_tsc].tsc_value) {
                    printf("Running out of memory while stroing TSC values\n");
                    exit(EXIT_FAILURE);
                }
                memcpy(cpu->tsc_values[count_tsc].tsc_value, copy+6, strlen(copy+6));
                cpu->tsc_values[count_tsc].tsc_value[strlen(copy+6)] = '\0';
            }
            /*
	     * TMA packets are involved in timestamp calculation.
	     * So, we associate the CTC and FC values with TSC packet for now.
	     */
            else if (strncmp(copy, "TMA", 3) == 0) {
                int loc_of_fc = location_of_fc(copy);
                int len_of_ctc = loc_of_fc -  1 - 10;
                int len_of_fc = 2;  // FC packets are of 8 bits, so 2 HEX characters
                cpu->tsc_values[count_tsc].tma_ctc_value = malloc((len_of_ctc+1) * sizeof(char));
                if (!cpu->tsc_values[count_tsc].tma_ctc_value) {
                    printf("Running out of memory while storing TMA CTC packets\n");
                    exit(EXIT_FAILURE);
                }
                memcpy(cpu->tsc_values[count_tsc].tma_ctc_value, copy+10, len_of_ctc);
                cpu->tsc_values[count_tsc].tma_ctc_value[len_of_ctc] = '\0';
                cpu->tsc_values[count_tsc].tma_fc_value = malloc((len_of_fc + 1) * sizeof(char));
                if (!cpu->tsc_values[count_tsc].tma_fc_value) {
                    printf("Running out of memory while storing TMA FC packets\n");
                    exit(EXIT_FAILURE);
                }
                memcpy(cpu->tsc_values[count_tsc].tma_fc_value, copy+loc_of_fc+5, 2);
                cpu->tsc_values[count_tsc].tma_fc_value[2] = '\0';
                count_tsc++;
            }
        }
        //start += pos+1;
        if (curr_lines_read >= max_lines_read) {
            if (strncmp(copy, "TNT", 3) == 0) {
                break;
            }
        }
    }
    cpu->tnt_index_limit = count;
    cpu->number_of_lines_consumed += curr_lines_read;
    printf("Number of lines consumed: %llu\n for cpu %d\n", cpu->number_of_lines_consumed, cpu->cpu_index);

#if 0
    printf("TNT array: %d\n", count);
    printf("FUP array: %d\n", count_fup);
    printf("TIP array: %d\n", count_tip);
# endif
   // preprocess the tip addresses //
    preprocess_tip_array(cpu, count_tip);
    precompute_tsc_values(cpu, count, cpu->last_tsc_value,
		    cpu->last_is_last_tsc, cpu->last_tma_ctc_value,
                    cpu->last_mtc_payload); // pass number of 'general' intelpt packets

    cpu->last_tip_address = malloc(sizeof(cpu->tip_addresses[count_tip-1].address));
    strcpy(cpu->last_tip_address, cpu->tip_addresses[count_tip-1].address);

#if 0
    printf("final count : %d\n", count);
#endif
}


static int tcg_cpu_exec(CPUState *cpu)
{
    int ret;
#ifdef CONFIG_PROFILER
    int64_t ti;
#endif
    /* create tnt_array here */
    // static char *tnt_array = NULL;

    if(cpu->tnt_array == NULL) {
        get_array_of_tnt_bits(cpu);
    }

    if(!cpu->tnt_array) {
      printf("get_array_of_tnt_bits returns NULL, for CPU index: %d\n",
		      cpu->cpu_index);
    }

    assert(tcg_enabled());
#ifdef CONFIG_PROFILER
    ti = profile_getclock();
#endif
    cpu_exec_start(cpu);
    ret = cpu_exec(cpu);
    cpu_exec_end(cpu);
#ifdef CONFIG_PROFILER
    atomic_set(&tcg_ctx->prof.cpu_exec_time,
               tcg_ctx->prof.cpu_exec_time + profile_getclock() - ti);
#endif
    return ret;
}

/* Destroy any remaining vCPUs which have been unplugged and have
 * finished running
 */
static void deal_with_unplugged_cpus(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (cpu->unplug && !cpu_can_run(cpu)) {
            qemu_tcg_destroy_vcpu(cpu);
            cpu->created = false;
            qemu_cond_signal(&qemu_cpu_cond);
            break;
        }
    }
}

/* Single-threaded TCG
 *
 * In the single-threaded case each vCPU is simulated in turn. If
 * there is more than a single vCPU we create a simple timer to kick
 * the vCPU and ensure we don't get stuck in a tight loop in one vCPU.
 * This is done explicitly rather than relying on side-effects
 * elsewhere.
 */

static void *qemu_tcg_rr_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    struct iovec iov[VIRTQUEUE_MAX_SIZE];
    hwaddr addr[VIRTQUEUE_MAX_SIZE];

    assert(tcg_enabled());
    rcu_register_thread();
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();

    cpu->created = true;
    cpu->can_do_io = 1;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    if (timer_cpuid_sequence_array[timer_index_array] == '0') {
        /* cpu0 is scheduled, cpu1 is stalled */
        is_cpu0_stalled = false;
        is_cpu1_stalled = true;
    } else if (timer_cpuid_sequence_array[timer_index_array] == '1') {
        is_cpu0_stalled = true;
        is_cpu1_stalled = false;
    }

    /* wait for initial kick-off after machine start */
    while (first_cpu->stopped) {
        qemu_cond_wait(first_cpu->halt_cond, &qemu_global_mutex);

        /* process any pending work */
        CPU_FOREACH(cpu) {
            current_cpu = cpu;
            qemu_wait_io_event_common(cpu);
        }
    }
    if (arnab_replay_mode != REPLAY_MODE_PLAY)
        start_tcg_kick_timer();

    cpu = first_cpu;

    /* process any pending work */
    cpu->exit_request = 1;

    while (1) {
        qemu_mutex_unlock_iothread();
        replay_mutex_lock();
        qemu_mutex_lock_iothread();
        /* Account partial waits to QEMU_CLOCK_VIRTUAL.  */
        if (arnab_replay_mode != REPLAY_MODE_PLAY) {
            qemu_account_warp_timer();

            /* Run the timers here.  This is much more efficient than
             * waking up the I/O thread and waiting for completion.
             */
            handle_icount_deadline();
	}

        replay_mutex_unlock();

        if (!cpu) {
            cpu = first_cpu;
        }

        while (cpu && !cpu->queued_work_first && !cpu->exit_request) {
            atomic_mb_set(&tcg_current_rr_cpu, cpu);
            current_cpu = cpu;
            if (arnab_replay_mode != REPLAY_MODE_PLAY) {
                qemu_clock_enable(QEMU_CLOCK_VIRTUAL,
                              (cpu->singlestep_enabled & SSTEP_NOTIMER) == 0);
            }

            if (cpu_can_run(cpu)) {
                int r;
                qemu_mutex_unlock_iothread();
                if (arnab_replay_mode != REPLAY_MODE_PLAY) {
                    prepare_icount_for_run(cpu);
                }

                if(!network_replay_done_at_init && arnab_replay_mode == REPLAY_MODE_PLAY) {
                    while(true) {
                        network_replay_done_at_init = true;
                        ReplayIOEvent *event;
                        event = g_malloc0(sizeof(ReplayIOEvent));
                        event->event_kind = REPLAY_ASYNC_EVENT_NET;
                        event->opaque = arnab_replay_event_net_load();
                        if (!event->opaque) {
                            g_free(event);
                            break;
                        }
                        replay_event_net_run(event->opaque);
                        g_free(event);
                    }
                }

                if(!disk_replay_done_at_init && arnab_replay_mode == REPLAY_MODE_PLAY) {
                    disk_replay_done_at_init = true;
                    unsigned int index;
                    size_t len;
                    size_t in_len;
                    int i;
                    while (true) {
                        index = arnab_replay_get_qword("disk", -1); // disk replay is independent of cpu
                        if (index == EVENT_BLK_INTERRUPT) {
                            break;
                        }
                        VirtQueueElement *vqe;
                        vqe = g_malloc(sizeof(VirtQueueElement));
                        vqe->index = index;
                        vqe->len = arnab_replay_get_qword("disk", -1);
                        vqe->ndescs = arnab_replay_get_qword("disk", -1);
                        vqe->out_num = arnab_replay_get_qword("disk", -1);
                        vqe->in_num = arnab_replay_get_qword("disk", -1);
                        in_len = arnab_replay_get_qword("disk", -1);
                        vqe->in_sg = g_malloc(sizeof(struct iovec) * vqe->in_num);
                        vqe->in_addr = g_malloc(sizeof(hwaddr) * vqe->in_num);
                        vqe->out_sg = g_malloc(sizeof(struct iovec) * vqe->out_num);
                        vqe->out_addr = g_malloc(sizeof(hwaddr) * vqe->out_num);
                        for (i = 0; i < vqe->in_num; i++) {
                            hwaddr rep_addr = arnab_replay_get_qword("disk", -1);
                            uint8_t *data;
                            arnab_replay_get_array_alloc(&data, &len, "disk", -1);
                            iov[i].iov_base = address_space_map(
                                            global_vdev->dma_as, rep_addr, &len, 1,
                                            MEMTXATTRS_UNSPECIFIED);
                            iov[i].iov_len = len;
                            addr[i] = rep_addr;
                            memcpy(iov[i].iov_base, data, len);
                            vqe->in_sg[i] = iov[i];
                            vqe->in_addr[i] = addr[i];
                            g_free((void *)data);
                        }
                        for (i = 0; i < vqe->out_num; i++) {
                            hwaddr rep_addr = arnab_replay_get_qword("disk", -1);
                            uint8_t *data;
                            arnab_replay_get_array_alloc(&data, &len, "disk", -1);
                            iov[i].iov_base = address_space_map(
                                            global_vdev->dma_as, rep_addr, &len, 0,
                                            MEMTXATTRS_UNSPECIFIED);
                            iov[i].iov_len = len;
                            addr[i] = rep_addr;
                            memcpy(iov[i].iov_base, data, len);
                            vqe->out_sg[i] = iov[i];
                            vqe->out_addr[i] = addr[i];
                            g_free((void *)data);
                        }
                        virtqueue_increment_inuse(global_vdev);
                        virtqueue_push_first_vq(global_vdev, vqe, in_len);
                        g_free(vqe->in_sg);
                        g_free(vqe->in_addr);
                        g_free(vqe->out_sg);
                        g_free(vqe->out_addr);
                    }
                }
                r = tcg_cpu_exec(cpu);

                if (arnab_replay_mode != REPLAY_MODE_PLAY) {
                    process_icount_data(cpu);
                }
                qemu_mutex_lock_iothread();

                if (r == EXCP_DEBUG) {
                    cpu_handle_guest_debug(cpu);
                    break;
                } else if (r == EXCP_ATOMIC) {
                    qemu_mutex_unlock_iothread();
                    cpu_exec_step_atomic(cpu);
                    qemu_mutex_lock_iothread();
                    break;
                } else if (arnab_replay_mode == REPLAY_MODE_PLAY &&
                            (r == EXCP_HALTED || r == EXCP_INTERRUPT)) {
                    continue;
                }
            } else if (cpu->stop) {
                if (cpu->unplug) {
                    cpu = CPU_NEXT(cpu);
                }
                break;
            }
            /* do not swap CPUs if one of them is stalled */
            if ((cpu->cpu_index == 1 && is_cpu0_stalled) ||
                    (cpu->cpu_index == 0 && is_cpu1_stalled)) {
                continue;
            }
            cpu = CPU_NEXT(cpu);
        } /* while (cpu && !cpu->exit_request).. */

        /* Does not need atomic_mb_set because a spurious wakeup is okay.  */
        atomic_set(&tcg_current_rr_cpu, NULL);

        if (cpu && cpu->exit_request) {
            atomic_mb_set(&cpu->exit_request, 0);
        }
        if (arnab_replay_mode != REPLAY_MODE_PLAY) {
            if (use_icount && all_cpu_threads_idle()) {
                /*
                 * When all cpus are sleeping (e.g in WFI), to avoid a deadlock
                 * in the main_loop, wake it up in order to start the warp timer.
                 */
                qemu_notify_event();
            }
        }
        qemu_tcg_rr_wait_io_event();
        deal_with_unplugged_cpus();
    }

    rcu_unregister_thread();
    return NULL;
}

static void *qemu_hax_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();
    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->created = true;
    current_cpu = cpu;

    hax_init_vcpu(cpu);
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        if (cpu_can_run(cpu)) {
            r = hax_smp_cpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }

        qemu_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));
    rcu_unregister_thread();
    return NULL;
}

/* The HVF-specific vCPU thread function. This one should only run when the host
 * CPU supports the VMX "unrestricted guest" feature. */
static void *qemu_hvf_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;

    int r;

    assert(hvf_enabled());

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    current_cpu = cpu;

    hvf_init_vcpu(cpu);

    /* signal CPU creation */
    cpu->created = true;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        if (cpu_can_run(cpu)) {
            r = hvf_vcpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
        qemu_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    hvf_vcpu_destroy(cpu);
    cpu->created = false;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_mutex_unlock_iothread();
    rcu_unregister_thread();
    return NULL;
}

static void *qemu_whpx_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;

    r = whpx_init_vcpu(cpu);
    if (r < 0) {
        fprintf(stderr, "whpx_init_vcpu failed: %s\n", strerror(-r));
        exit(1);
    }

    /* signal CPU creation */
    cpu->created = true;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        if (cpu_can_run(cpu)) {
            r = whpx_vcpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
        while (cpu_thread_is_idle(cpu)) {
            qemu_cond_wait(cpu->halt_cond, &qemu_global_mutex);
        }
        qemu_wait_io_event_common(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    whpx_destroy_vcpu(cpu);
    cpu->created = false;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_mutex_unlock_iothread();
    rcu_unregister_thread();
    return NULL;
}

#ifdef _WIN32
static void CALLBACK dummy_apc_func(ULONG_PTR unused)
{
}
#endif

/* Multi-threaded TCG
 *
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

static void *qemu_tcg_cpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;

    assert(tcg_enabled());
    g_assert(!use_icount);

    rcu_register_thread();
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->created = true;
    cpu->can_do_io = 1;
    current_cpu = cpu;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    /* process any pending work */
    cpu->exit_request = 1;

    do {
        if (cpu_can_run(cpu)) {
            int r;
            qemu_mutex_unlock_iothread();
            r = tcg_cpu_exec(cpu);
            qemu_mutex_lock_iothread();
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /* during start-up the vCPU is reset and the thread is
                 * kicked several times. If we don't ensure we go back
                 * to sleep in the halted state we won't cleanly
                 * start-up when the vCPU is enabled.
                 *
                 * cpu->halted should ensure we sleep in wait_io_event
                 */
                g_assert(cpu->halted);
                break;
            case EXCP_ATOMIC:
                qemu_mutex_unlock_iothread();
                cpu_exec_step_atomic(cpu);
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        atomic_mb_set(&cpu->exit_request, 0);
        qemu_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    qemu_tcg_destroy_vcpu(cpu);
    cpu->created = false;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_mutex_unlock_iothread();
    rcu_unregister_thread();
    return NULL;
}

static void qemu_cpu_kick_thread(CPUState *cpu)
{
#ifndef _WIN32
    int err;

    if (cpu->thread_kicked) {
        return;
    }
    cpu->thread_kicked = true;
    err = pthread_kill(cpu->thread->thread, SIG_IPI);
    if (err && err != ESRCH) {
        fprintf(stderr, "qemu:%s: %s", __func__, strerror(err));
        exit(1);
    }
#else /* _WIN32 */
    if (!qemu_cpu_is_self(cpu)) {
        if (whpx_enabled()) {
            whpx_vcpu_kick(cpu);
        } else if (!QueueUserAPC(dummy_apc_func, cpu->hThread, 0)) {
            fprintf(stderr, "%s: QueueUserAPC failed with error %lu\n",
                    __func__, GetLastError());
            exit(1);
        }
    }
#endif
}

void qemu_cpu_kick(CPUState *cpu)
{
    qemu_cond_broadcast(cpu->halt_cond);
    if (tcg_enabled()) {
        if (qemu_tcg_mttcg_enabled()) {
            cpu_exit(cpu);
        } else {
            qemu_cpu_kick_rr_cpus();
        }
    } else {
        if (hax_enabled()) {
            /*
             * FIXME: race condition with the exit_request check in
             * hax_vcpu_hax_exec
             */
            cpu->exit_request = 1;
        }
        qemu_cpu_kick_thread(cpu);
    }
}

void qemu_cpu_kick_self(void)
{
    assert(current_cpu);
    qemu_cpu_kick_thread(current_cpu);
}

bool qemu_cpu_is_self(CPUState *cpu)
{
    return qemu_thread_is_self(cpu->thread);
}

bool qemu_in_vcpu_thread(void)
{
    return current_cpu && qemu_cpu_is_self(current_cpu);
}

static __thread bool iothread_locked = false;

bool qemu_mutex_iothread_locked(void)
{
    return iothread_locked;
}

/*
 * The BQL is taken from so many places that it is worth profiling the
 * callers directly, instead of funneling them all through a single function.
 */
void qemu_mutex_lock_iothread_impl(const char *file, int line)
{
    QemuMutexLockFunc bql_lock = atomic_read(&qemu_bql_mutex_lock_func);

    g_assert(!qemu_mutex_iothread_locked());
    bql_lock(&qemu_global_mutex, file, line);
    iothread_locked = true;
}

void qemu_mutex_unlock_iothread(void)
{
    g_assert(qemu_mutex_iothread_locked());
    iothread_locked = false;
    qemu_mutex_unlock(&qemu_global_mutex);
}

void qemu_cond_wait_iothread(QemuCond *cond)
{
    qemu_cond_wait(cond, &qemu_global_mutex);
}

static bool all_vcpus_paused(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!cpu->stopped) {
            return false;
        }
    }

    return true;
}

void pause_all_vcpus(void)
{
    CPUState *cpu;

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, false);
    CPU_FOREACH(cpu) {
        if (qemu_cpu_is_self(cpu)) {
            qemu_cpu_stop(cpu, true);
        } else {
            cpu->stop = true;
            qemu_cpu_kick(cpu);
        }
    }

    /* We need to drop the replay_lock so any vCPU threads woken up
     * can finish their replay tasks
     */
    replay_mutex_unlock();

    while (!all_vcpus_paused()) {
        qemu_cond_wait(&qemu_pause_cond, &qemu_global_mutex);
        CPU_FOREACH(cpu) {
            qemu_cpu_kick(cpu);
        }
    }

    qemu_mutex_unlock_iothread();
    replay_mutex_lock();
    qemu_mutex_lock_iothread();
}

void cpu_resume(CPUState *cpu)
{
    cpu->stop = false;
    cpu->stopped = false;
    qemu_cpu_kick(cpu);
}

void resume_all_vcpus(void)
{
    CPUState *cpu;

    if (!runstate_is_running()) {
        return;
    }

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    CPU_FOREACH(cpu) {
        cpu_resume(cpu);
    }
}

void cpu_remove_sync(CPUState *cpu)
{
    cpu->stop = true;
    cpu->unplug = true;
    qemu_cpu_kick(cpu);
    qemu_mutex_unlock_iothread();
    qemu_thread_join(cpu->thread);
    qemu_mutex_lock_iothread();
}

/* For temporary buffers for forming a name */
#define VCPU_THREAD_NAME_SIZE 16

static void qemu_tcg_init_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    static QemuCond *single_tcg_halt_cond;
    static QemuThread *single_tcg_cpu_thread;
    static int tcg_region_inited;

    assert(tcg_enabled());
    /*
     * Initialize TCG regions--once. Now is a good time, because:
     * (1) TCG's init context, prologue and target globals have been set up.
     * (2) qemu_tcg_mttcg_enabled() works now (TCG init code runs before the
     *     -accel flag is processed, so the check doesn't work then).
     */
    if (!tcg_region_inited) {
        tcg_region_inited = 1;
        tcg_region_init();
    }

    if (qemu_tcg_mttcg_enabled() || !single_tcg_cpu_thread) {
        cpu->thread = g_malloc0(sizeof(QemuThread));
        cpu->halt_cond = g_malloc0(sizeof(QemuCond));
        qemu_cond_init(cpu->halt_cond);

        if (qemu_tcg_mttcg_enabled()) {
            /* create a thread per vCPU with TCG (MTTCG) */
            parallel_cpus = true;
            snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
                 cpu->cpu_index);

            qemu_thread_create(cpu->thread, thread_name, qemu_tcg_cpu_thread_fn,
                               cpu, QEMU_THREAD_JOINABLE);

        } else {
            /* share a single thread for all cpus with TCG */
            snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "ALL CPUs/TCG");
            qemu_thread_create(cpu->thread, thread_name,
                               qemu_tcg_rr_cpu_thread_fn,
                               cpu, QEMU_THREAD_JOINABLE);

            single_tcg_halt_cond = cpu->halt_cond;
            single_tcg_cpu_thread = cpu->thread;
        }
#ifdef _WIN32
        cpu->hThread = qemu_thread_get_handle(cpu->thread);
#endif
    } else {
        /* For non-MTTCG cases we share the thread */
        cpu->thread = single_tcg_cpu_thread;
        cpu->halt_cond = single_tcg_halt_cond;
        cpu->thread_id = first_cpu->thread_id;
        cpu->can_do_io = 1;
        cpu->created = true;
    }
}

static void qemu_hax_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/HAX",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_hax_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
#ifdef _WIN32
    cpu->hThread = qemu_thread_get_handle(cpu->thread);
#endif
}

static void qemu_kvm_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/KVM",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_kvm_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void qemu_hvf_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    /* HVF currently does not support TCG, and only runs in
     * unrestricted-guest mode. */
    assert(hvf_enabled());

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/HVF",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_hvf_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void qemu_whpx_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/WHPX",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_whpx_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
#ifdef _WIN32
    cpu->hThread = qemu_thread_get_handle(cpu->thread);
#endif
}

static void qemu_dummy_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/DUMMY",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_dummy_cpu_thread_fn, cpu,
                       QEMU_THREAD_JOINABLE);
}

void qemu_init_vcpu(CPUState *cpu)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    cpu->nr_cores = ms->smp.cores;
    cpu->nr_threads =  ms->smp.threads;
    cpu->stopped = true;
    cpu->random_seed = qemu_guest_random_seed_thread_part1();

    if (!cpu->as) {
        /* If the target cpu hasn't set up any address spaces itself,
         * give it the default one.
         */
        cpu->num_ases = 1;
        cpu_address_space_init(cpu, 0, "cpu-memory", cpu->memory);
    }

    if (kvm_enabled()) {
        qemu_kvm_start_vcpu(cpu);
    } else if (hax_enabled()) {
        qemu_hax_start_vcpu(cpu);
    } else if (hvf_enabled()) {
        qemu_hvf_start_vcpu(cpu);
    } else if (tcg_enabled()) {
        qemu_tcg_init_vcpu(cpu);
    } else if (whpx_enabled()) {
        qemu_whpx_start_vcpu(cpu);
    } else {
        qemu_dummy_start_vcpu(cpu);
    }

    while (!cpu->created) {
        qemu_cond_wait(&qemu_cpu_cond, &qemu_global_mutex);
    }
}

void cpu_stop_current(void)
{
    if (current_cpu) {
        current_cpu->stop = true;
        cpu_exit(current_cpu);
    }
}

int vm_stop(RunState state)
{
    if (qemu_in_vcpu_thread()) {
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(state);
        /*
         * FIXME: should not return to device code in case
         * vm_stop() has been requested.
         */
        cpu_stop_current();
        return 0;
    }

    return do_vm_stop(state, true);
}

/**
 * Prepare for (re)starting the VM.
 * Returns -1 if the vCPUs are not to be restarted (e.g. if they are already
 * running or in case of an error condition), 0 otherwise.
 */
int vm_prepare_start(void)
{
    RunState requested;

    qemu_vmstop_requested(&requested);
    if (runstate_is_running() && requested == RUN_STATE__MAX) {
        return -1;
    }

    /* Ensure that a STOP/RESUME pair of events is emitted if a
     * vmstop request was pending.  The BLOCK_IO_ERROR event, for
     * example, according to documentation is always followed by
     * the STOP event.
     */
    if (runstate_is_running()) {
        qapi_event_send_stop();
        qapi_event_send_resume();
        return -1;
    }

    /* We are sending this now, but the CPUs will be resumed shortly later */
    qapi_event_send_resume();

    cpu_enable_ticks();
    runstate_set(RUN_STATE_RUNNING);
    vm_state_notify(1, RUN_STATE_RUNNING);
    return 0;
}

void vm_start(void)
{
    if (!vm_prepare_start()) {
        resume_all_vcpus();
    }
}

/* does a state transition even if the VM is already stopped,
   current state is forgotten forever */
int vm_stop_force_state(RunState state)
{
    if (runstate_is_running()) {
        return vm_stop(state);
    } else {
        runstate_set(state);

        bdrv_drain_all();
        /* Make sure to return an error if the flush in a previous vm_stop()
         * failed. */
        return bdrv_flush_all();
    }
}

void list_cpus(const char *optarg)
{
    /* XXX: implement xxx_cpu_list for targets that still miss it */
#if defined(cpu_list)
    cpu_list();
#endif
}

void qmp_memsave(int64_t addr, int64_t size, const char *filename,
                 bool has_cpu, int64_t cpu_index, Error **errp)
{
    FILE *f;
    uint32_t l;
    CPUState *cpu;
    uint8_t buf[1024];
    int64_t orig_addr = addr, orig_size = size;

    if (!has_cpu) {
        cpu_index = 0;
    }

    cpu = qemu_get_cpu(cpu_index);
    if (cpu == NULL) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "cpu-index",
                   "a CPU number");
        return;
    }

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        if (cpu_memory_rw_debug(cpu, addr, buf, l, 0) != 0) {
            error_setg(errp, "Invalid addr 0x%016" PRIx64 "/size %" PRId64
                             " specified", orig_addr, orig_size);
            goto exit;
        }
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, QERR_IO_ERROR);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_pmemsave(int64_t addr, int64_t size, const char *filename,
                  Error **errp)
{
    FILE *f;
    uint32_t l;
    uint8_t buf[1024];

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        cpu_physical_memory_read(addr, buf, l);
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, QERR_IO_ERROR);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_inject_nmi(Error **errp)
{
    nmi_monitor_handle(monitor_get_cpu_index(), errp);
}

void dump_drift_info(void)
{
    if (!use_icount) {
        return;
    }

    qemu_printf("Host - Guest clock  %"PRIi64" ms\n",
                (cpu_get_clock() - cpu_get_icount())/SCALE_MS);
    if (icount_align_option) {
        qemu_printf("Max guest delay     %"PRIi64" ms\n",
                    -max_delay / SCALE_MS);
        qemu_printf("Max guest advance   %"PRIi64" ms\n",
                    max_advance / SCALE_MS);
    } else {
        qemu_printf("Max guest delay     NA\n");
        qemu_printf("Max guest advance   NA\n");
    }
}
