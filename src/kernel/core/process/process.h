#ifndef PROCESS_H
#define PROCESS_H

#include "ktypes.h"
#include "kernel_config.h"
#include "vmm.h"
#include "boxos_magic.h"
#include "boxos_limits.h"
#include "atomics.h"
#include "cabin.h"
#include "addr_wait.h"
#include "baton.h"
#include "chit.h"


#define PROCESS_MAX_COUNT MAX_PROCESSES
#define PROCESS_TAG_SIZE 256
#define PROCESS_INVALID_PID 0

#define QUIESCE_CORE_NONE 0xFFu


struct process_t;

struct TouchOwed;

typedef struct GoneWaiter {
    struct process_t  *waiter;
    uint32_t           want_generation;
    uint32_t           submit_cookie;
    struct GoneWaiter *next;
} GoneWaiter;

typedef enum
{
    WAIT_NONE = 0,
    WAIT_RESULT,
    WAIT_IO
} wait_reason_t;

typedef enum
{
    PROC_CREATED = 0,
    PROC_WORKING,
    PROC_WAITING,
    PROC_STOPPED,
    PROC_DONE,
    PROC_CRASHED
} process_state_t;

typedef struct
{
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    uint16_t cs, ds, es, fs, gs, ss;
    uint64_t rflags;
    uint64_t cr3;
    uint8_t *fpu_state;
    bool fpu_initialized;
    uint8_t _pad_ssp[7];

    uint64_t pl0_ssp;

    uint64_t user_fsbase;
} ProcessContext;

_Static_assert(offsetof(ProcessContext, rip) == 128, "ProcessContext.rip offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, cs) == 136, "ProcessContext.cs offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, rflags) == 152, "ProcessContext.rflags offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_state) == 168, "ProcessContext.fpu_state offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_initialized) == 176, "ProcessContext.fpu_initialized offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, pl0_ssp) == 184, "ProcessContext.pl0_ssp offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, user_fsbase) == 192, "ProcessContext.user_fsbase offset mismatch with asm");

typedef struct process_t
{
    uint32_t magic;
    uint32_t pid;

    uint32_t generation;

    cabin_t *cabin;

    int32_t score;
    uint64_t last_run_time;
    uint32_t consecutive_runs;
    uint64_t total_cpu_time;
    uint64_t cpu_tsc_stamp;
    int8_t current_prio;
    int8_t rq_prio;
    int16_t rq_index;
    volatile int16_t on_cpu;

    volatile process_state_t state;
    spinlock_t state_lock;

    atomic_u32_t ref_count;

    volatile uint8_t destroying;

    ProcessContext context;
    void *kernel_stack;
    void *kernel_stack_top;
    void *kernel_stack_guard_base;

    bool started;
    uint8_t home_core;
    volatile uint8_t kcore_pending;

    volatile wait_reason_t wait_reason;
    uint64_t wait_start_time;

    uint64_t         irq_stack_top;
    uint64_t         irq_rip;
    volatile uint8_t irq_active;
    uint8_t          _irq_pad[3];
    uint64_t         irq_saved_rip;
    uint64_t         irq_saved_rsp;
    uint64_t         irq_saved_rflags;
    void            *irq_pending_head;
    spinlock_t       irq_lock;

    uint8_t           touch_cleaned;

    struct TouchOwed *owed_head;
    struct TouchOwed *owed_tail;
    spinlock_t        owed_lock;
    volatile uint32_t owed_count;
    volatile uint32_t owed_draining;

    uintptr_t         user_ssp_phys;
    uintptr_t         user_ssp_va;
    uint32_t          user_ssp_size;

    uintptr_t         kernel_ssp_phys;
    uintptr_t         kernel_ssp_va_top;

    uintptr_t         hammock_base;
    uintptr_t         user_stack_phys;

    volatile uint8_t  reap_blocked;

    uint8_t           quiesce_core;
    uint64_t          quiesce_seq;

    uint64_t          pocket_ring_phys;
    uint64_t          result_ring_phys;
    uint64_t          touch_ring_phys;
    uint64_t          strandinfo_phys;

    uint64_t          strand_pool_va;
    uint32_t          strand_pool_gen;
    uint64_t          strand_pool_orphan_va;

    AddrWaitEntry     addr_wait_entry;

    Chit              chit;

    Baton             deadline_baton;
    volatile uint8_t  deadline_passed;

    struct GoneWaiter *gone_waiters;
    spinlock_t        gone_lock;

    uint32_t          park_seq;

    uintptr_t         nameplate_va;
    uint64_t          nameplate_bytes;

    struct process_t *hash_next;
    struct process_t *next;
    struct process_t *prev;
    struct process_t *ready_next;
    struct process_t *cleanup_next;
    atomic_u32_t      cleanup_enqueued;
    volatile uint8_t  in_ready;

    volatile uint8_t  fsbase_owed;
    uint64_t          fsbase_wanted;
} process_t;

_Static_assert(sizeof(process_t) < 4096, "Process structure must fit in one page");

uintptr_t process_get_user_ssp_phys(struct process_t *proc);
uintptr_t process_get_user_ssp_va(struct process_t *proc);
uint32_t  process_get_user_ssp_size(struct process_t *proc);
void      process_set_user_ssp(struct process_t *proc, uintptr_t phys,
                                uintptr_t va, uint32_t size);
uintptr_t process_user_ssp_va_for(struct process_t *proc);
uintptr_t process_user_ssp_guard_hi_for(struct process_t *proc);
uintptr_t process_user_ssp_guard_lo_for(struct process_t *proc);

void process_init(void);

process_t *process_create(const char *tags);
void process_destroy(process_t *proc);

process_t *strand_spawn(cabin_t *cabin, uintptr_t entry_va, uint64_t arg, bool joinable);

int process_load_binary(process_t *proc, const void *binary_data, size_t size);

process_t *process_find(uint32_t pid);
process_t *process_find_ref(uint32_t pid);
process_t *process_get_first(void);
process_t *process_get_current(void);

void process_list_lock(void);
void process_list_unlock(void);
bool spin_trylock_process_list(void);

uint32_t process_get_count(void);

uint32_t process_snapshot_pids(uint32_t *out, uint32_t max);

#ifdef CONFIG_KERNEL_TESTS
void process_test(void);
#endif

void process_start_initial(process_t *proc);

bool process_has_tag(process_t *proc, const char *tag);
bool process_has_tag_id(process_t *proc, uint16_t tag_id);
int process_add_tag(process_t *proc, const char *tag);
int process_remove_tag(process_t *proc, const char *tag);
size_t process_snapshot_tags(process_t *proc, char *buffer, size_t buffer_size);

void process_ref_inc(process_t *proc);
void process_ref_dec(process_t *proc);
uint32_t process_ref_count(process_t *proc);

void process_set_state(process_t *proc, process_state_t new_state);
process_state_t process_get_state(process_t *proc);

int process_destroy_safe(process_t *proc);

void process_list_validate(const char *caller);

void process_cleanup_deferred(void);
uint32_t process_cleanup_queue_size(void);
void process_cleanup_queue_flush(void);

void process_force_release_locks_for_shutdown(void);

void process_reap_strands(void);

uint64_t *process_active_memtags(process_t *proc);
void *process_get_cabin(process_t *proc);

#endif