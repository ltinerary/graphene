/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <stddef.h> /* linux/signal.h misses this dependency (for size_t), at least on Ubuntu 16.04.
                     * We must include it ourselves before including linux/signal.h.
                     */

#include <asm/errno.h>
#include <asm/prctl.h>
#include <asm/signal.h>
#include <linux/futex.h>
#include <linux/signal.h>

#include "assert.h"
#include "gdb_integration/sgx_gdb.h"
#include "pal_internal.h"
#include "pal_security.h"
#include "sgx_enclave.h"
#include "sgx_internal.h"
#include "sgx_log.h"
#include "spinlock.h"

struct thread_map {
    unsigned int    tid;
    sgx_arch_tcs_t* tcs;
};

static sgx_arch_tcs_t* g_enclave_tcs;
static int g_enclave_thread_num;
static struct thread_map* g_enclave_thread_map;

bool g_sgx_enable_stats = false;

/* this function is called only on thread/process exit (never in the middle of thread exec) */
void update_and_print_stats(bool process_wide) {
    static atomic_ulong g_eenter_cnt       = 0;
    static atomic_ulong g_eexit_cnt        = 0;
    static atomic_ulong g_aex_cnt          = 0;
    static atomic_ulong g_sync_signal_cnt  = 0;
    static atomic_ulong g_async_signal_cnt = 0;

    if (!g_sgx_enable_stats)
        return;

    PAL_TCB_URTS* tcb = get_tcb_urts();

    int tid = INLINE_SYSCALL(gettid, 0);
    assert(tid > 0);
    urts_log_always("----- SGX stats for thread %d -----\n"
                    "  # of EENTERs:        %lu\n"
                    "  # of EEXITs:         %lu\n"
                    "  # of AEXs:           %lu\n"
                    "  # of sync signals:   %lu\n"
                    "  # of async signals:  %lu\n",
                    tid, tcb->eenter_cnt, tcb->eexit_cnt, tcb->aex_cnt,
                    tcb->sync_signal_cnt, tcb->async_signal_cnt);

    g_eenter_cnt       += tcb->eenter_cnt;
    g_eexit_cnt        += tcb->eexit_cnt;
    g_aex_cnt          += tcb->aex_cnt;
    g_sync_signal_cnt  += tcb->sync_signal_cnt;
    g_async_signal_cnt += tcb->async_signal_cnt;

    if (process_wide) {
        int pid = INLINE_SYSCALL(getpid, 0);
        assert(pid > 0);
        urts_log_always("----- Total SGX stats for process %d -----\n"
                        "  # of EENTERs:        %lu\n"
                        "  # of EEXITs:         %lu\n"
                        "  # of AEXs:           %lu\n"
                        "  # of sync signals:   %lu\n"
                        "  # of async signals:  %lu\n",
                        pid, g_eenter_cnt, g_eexit_cnt, g_aex_cnt,
                        g_sync_signal_cnt, g_async_signal_cnt);
    }
}

void pal_tcb_urts_init(PAL_TCB_URTS* tcb, void* stack, void* alt_stack) {
    tcb->self = tcb;
    tcb->tcs = NULL;    /* initialized by child thread */
    tcb->stack = stack;
    tcb->alt_stack = alt_stack;

    tcb->eenter_cnt       = 0;
    tcb->eexit_cnt        = 0;
    tcb->aex_cnt          = 0;
    tcb->sync_signal_cnt  = 0;
    tcb->async_signal_cnt = 0;

    tcb->profile_sample_time = 0;
}

static spinlock_t tcs_lock = INIT_SPINLOCK_UNLOCKED;

void create_tcs_mapper(void* tcs_base, unsigned int thread_num) {
    size_t thread_map_size = ALIGN_UP_POW2(sizeof(struct thread_map) * thread_num, PRESET_PAGESIZE);

    g_enclave_tcs = tcs_base;
    g_enclave_thread_num = thread_num;
    g_enclave_thread_map = (struct thread_map*)INLINE_SYSCALL(mmap, 6, NULL, thread_map_size,
                                                              PROT_READ | PROT_WRITE,
                                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    for (uint32_t i = 0; i < thread_num; i++) {
        g_enclave_thread_map[i].tid = 0;
        g_enclave_thread_map[i].tcs = &g_enclave_tcs[i];
    }
}

void map_tcs(unsigned int tid) {
    spinlock_lock(&tcs_lock);
    for (int i = 0; i < g_enclave_thread_num; i++)
        if (!g_enclave_thread_map[i].tid) {
            g_enclave_thread_map[i].tid = tid;
            get_tcb_urts()->tcs = g_enclave_thread_map[i].tcs;
            ((struct enclave_dbginfo*)DBGINFO_ADDR)->thread_tids[i] = tid;
            break;
        }
    spinlock_unlock(&tcs_lock);
}

void unmap_tcs(void) {
    spinlock_lock(&tcs_lock);

    int index = get_tcb_urts()->tcs - g_enclave_tcs;
    struct thread_map* map = &g_enclave_thread_map[index];

    assert(index < g_enclave_thread_num);

    get_tcb_urts()->tcs = NULL;
    ((struct enclave_dbginfo*)DBGINFO_ADDR)->thread_tids[index] = 0;
    map->tid = 0;
    spinlock_unlock(&tcs_lock);
}

int current_enclave_thread_cnt(void) {
    int ret = 0;
    spinlock_lock(&tcs_lock);
    for (int i = 0; i < g_enclave_thread_num; i++)
        if (g_enclave_thread_map[i].tid)
            ret++;
    spinlock_unlock(&tcs_lock);
    return ret;
}

/*
 * pal_thread_init(): An initialization wrapper of a newly-created thread (including
 * the first thread). This function accepts a TCB pointer to be set to the GS register
 * of the thread. The rest of the TCB is used as the alternative stack for signal
 * handling. Notice that this sets up the untrusted thread -- an enclave thread is set
 * up by other means (e.g., the GS register is set by an SGX-enforced TCS.OGSBASGX).
 */
int pal_thread_init(void* tcbptr) {
    PAL_TCB_URTS* tcb = tcbptr;
    int ret;

    /* set GS reg of this thread to thread's TCB; after this point, can use get_tcb_urts() */
    ret = INLINE_SYSCALL(arch_prctl, 2, ARCH_SET_GS, tcb);
    if (ret < 0) {
        ret = -EPERM;
        goto out;
    }

    if (tcb->alt_stack) {
        stack_t ss = {
            .ss_sp    = tcb->alt_stack,
            .ss_flags = 0,
            .ss_size  = ALT_STACK_SIZE - sizeof(*tcb)
        };
        ret = INLINE_SYSCALL(sigaltstack, 2, &ss, NULL);
        if (ret < 0) {
            ret = -EPERM;
            goto out;
        }
    }

    int tid = INLINE_SYSCALL(gettid, 0);
    map_tcs(tid); /* updates tcb->tcs */

    if (!tcb->tcs) {
        urts_log_error(
            "There are no available TCS pages left for a new thread!\n"
            "Please try to increase sgx.thread_num in the manifest.\n"
            "The current value is %d\n",
            g_enclave_thread_num);
        ret = -ENOMEM;
        goto out;
    }

    if (!tcb->stack) {
        /* only first thread doesn't have a stack (it uses the one provided by Linux); first
         * thread calls ecall_enclave_start() instead of ecall_thread_start() so just exit */
        return 0;
    }

    /* not-first (child) thread, start it */
    ecall_thread_start();

    unmap_tcs();
    ret = 0;
out:
    INLINE_SYSCALL(munmap, 2, tcb->stack, THREAD_STACK_SIZE + ALT_STACK_SIZE);
    return ret;
}

noreturn void thread_exit(int status) {
    PAL_TCB_URTS* tcb = get_tcb_urts();

    /* technically, async signals were already blocked before calling this function
     * (by sgx_ocall_exit()) but we keep it here for future proof */
    block_async_signals(true);

    update_and_print_stats(/*process_wide=*/false);

    if (tcb->alt_stack) {
        stack_t ss;
        ss.ss_sp    = NULL;
        ss.ss_flags = SS_DISABLE;
        ss.ss_size  = 0;

        /* take precautions to unset the TCB and alternative stack first */
        INLINE_SYSCALL(arch_prctl, 2, ARCH_SET_GS, 0);
        INLINE_SYSCALL(sigaltstack, 2, &ss, NULL);
    }

    /* free the thread stack (via munmap) and exit; note that exit() needs a "status" arg
     * but it could be allocated on a stack, so we must put it in register and do asm */
    __asm__ volatile("cmpq $0, %%rdi \n"        /* check if tcb->stack != NULL */
                     "je 1f \n"
                     "syscall \n"               /* all args are already prepared, call munmap */
                     "1: \n"
                     "mov %[nr_exit], %%rax \n"
                     "mov %[exit_code], %%edi \n"
                     "syscall \n"               /* all args are prepared, call exit  */
                     "ud2 \n"
                     "jmp 1b \n"
                     :
                     : "a" (__NR_munmap), "D" (tcb->stack), "S" (THREAD_STACK_SIZE + ALT_STACK_SIZE),
                       [nr_exit] "i" (__NR_exit), [exit_code] "r" (status)
                     : "memory", "rcx", "r11"
    );
    __builtin_unreachable();
}

int clone_thread(void) {
    int ret = 0;

    void* stack = (void*)INLINE_SYSCALL(mmap, 6, NULL, THREAD_STACK_SIZE + ALT_STACK_SIZE,
                                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (IS_ERR_P(stack))
        return -ENOMEM;

    /* Stack layout for the new thread looks like this (recall that stacks grow towards lower
     * addresses on Linux on x86-64):
     *
     *       stack +--> +-------------------+
     *                  |  child stack      | THREAD_STACK_SIZE
     * child_stack +--> +-------------------+
     *                  |  alternate stack  | ALT_STACK_SIZE - sizeof(PAL_TCB_URTS)
     *         tcb +--> +-------------------+
     *                  |  PAL TCB          | sizeof(PAL_TCB_URTS)
     *                  +-------------------+
     *
     * Note that this whole memory region is zeroed out because we use mmap(). */

    void* child_stack_top = stack + THREAD_STACK_SIZE;

    /* initialize TCB at the top of the alternative stack */
    PAL_TCB_URTS* tcb = child_stack_top + ALT_STACK_SIZE - sizeof(PAL_TCB_URTS);
    pal_tcb_urts_init(tcb, stack, child_stack_top);

    /* align child_stack to 16 */
    child_stack_top = ALIGN_DOWN_PTR(child_stack_top, 16);

    int dummy_parent_tid_field = 0;
    // TODO: pal_thread_init() may fail during initialization (e.g. on TCS exhaustion), we should
    // check its result (but this happens asynchronously, so it's not trivial to do).
    ret = clone(pal_thread_init, child_stack_top,
                CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM | CLONE_THREAD | CLONE_SIGHAND |
                    CLONE_PARENT_SETTID,
                (void*)tcb, &dummy_parent_tid_field, NULL);

    if (ret < 0) {
        INLINE_SYSCALL(munmap, 2, stack, THREAD_STACK_SIZE + ALT_STACK_SIZE);
        return ret;
    }
    return 0;
}

int get_tid_from_tcs(void* tcs) {
    int index = (sgx_arch_tcs_t*)tcs - g_enclave_tcs;
    struct thread_map* map = &g_enclave_thread_map[index];
    if (index >= g_enclave_thread_num)
        return -EINVAL;
    if (!map->tid)
        return -EINVAL;

    return map->tid;
}
