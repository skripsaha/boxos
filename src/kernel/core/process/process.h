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

/*
 * Lock ordering:
 *   1. scheduler_lock  (scheduler.c) — acquired BRIEFLY only to check current_process
 *   2. process_lock    (process.c)   — global list protection
 *   3. cleanup_queue.lock            — deferred cleanup queue
 *   4. process_t->state_lock         — per-process state
 *
 * process_destroy() releases scheduler_lock before any resource cleanup.
 * schedule() handles periodic cleanup of finished processes.
 * process_cleanup_deferred() runs without scheduler_lock or process_lock.
 */

#define PROCESS_MAX_COUNT MAX_PROCESSES
#define PROCESS_TAG_SIZE 256
#define PROCESS_INVALID_PID 0

// Fast O(1) role checks use proc->cabin->tag_bits AND g_well_known bitmasks from tagfs.h.

struct process_t;

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
    // Pointer to dynamically allocated FPU/SSE/AVX state buffer.
    // Buffer is allocated at process creation with fpu_alloc_size() bytes.
    // fpu_save/fpu_restore align this pointer to 64 bytes at runtime via fpu_align().
    uint8_t *fpu_state;
    bool fpu_initialized;
    uint8_t _pad_ssp[7];               /* align pl0_ssp to 8 bytes */

    /* CET — per-process supervisor SSP MSR snapshot (Intel SDM Vol 3D §17).
     *
     * Saved by task_save_context via RDMSR IA32_PL0_SSP (0x6A4); restored by
     * task_restore_context via WRMSR IA32_PL0_SSP. Both ops are gated on
     * `g_cet_supv_active` (1 byte global, set when S_CET.SH_STK_EN flips
     * to 1); on TCG (no SHSTK) and on real-HW before the activation, the
     * flag stays 0 and the asm path skips the MSR access — pl0_ssp is dead
     * weight then, no boot-time cost.
     *
     * For new processes, cet_process_create_kernel_ssp allocates a 4 KiB
     * supervisor SSP page, writes a supervisor token at the top, pre-pushes
     * the kernel-entry RIP one slot below, and stores SSP=top-8 here so
     * task_restore_context's first RET pops a matching shadow-stack entry.
     */
    uint64_t pl0_ssp;

    /* Per-process user FS base — the TLS thread pointer (System V x86-64
     * uses FS for thread-local storage). Userspace programs it via
     * WRFSBASE (CR4.FSGSBASE=1) or the kernel SET_FSBASE op on older
     * silicon; ISRs never touch FS, so the live value at context-switch
     * time is always the owning process's.
     *
     * Saved/restored by context_switch.asm: RDFSBASE/WRFSBASE when
     * g_fsgsbase_active=1, MSR 0xC0000100 when g_user_fsbase_used=1,
     * skipped entirely otherwise (zero cost until TLS is used).
     * CRITICAL restore order: the FS *selector* load (`mov fs, ax`)
     * zeroes the base on real silicon — WRFSBASE must come after it. */
    uint64_t user_fsbase;
} ProcessContext;

// These offsets must match context_switch.asm — if the struct layout changes,
// update the %define constants in context_switch.asm to match.
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

    /* Pointer to the shared cabin (address space + IPC rings + Bay/Brook/Touch).
     * NULL for the idle process.  All cabin-shared state lives in cabin_t. */
    cabin_t *cabin;

    int32_t score;
    uint64_t last_run_time;
    uint32_t consecutive_runs;
    uint64_t total_cpu_time;
    int8_t current_prio;            // Current scheduler priority level (set on enqueue)
    int8_t rq_prio;                 // Priority level in runqueue (-1 = not enqueued)
    int16_t rq_index;               // Index in queue (-1 = not enqueued)

    volatile process_state_t state;
    spinlock_t state_lock;

    atomic_u32_t ref_count;

    // Atomic flag: 1 = process is being destroyed, 0 = normal.
    // Set BEFORE releasing scheduler_lock in process_destroy().
    // Checked in scheduler_select_next() to prevent selecting doomed process.
    volatile uint8_t destroying;

    ProcessContext context;
    void *kernel_stack;
    void *kernel_stack_top;
    void *kernel_stack_guard_base;

    bool started;
    uint8_t home_core;              // App Core index for scheduling (RunQueue lives here)
    volatile uint8_t kcore_pending; // 1 = already in a K-Core queue, 0 = free (atomic CAS)

    volatile wait_reason_t wait_reason;
    uint64_t wait_start_time; // TSC timestamp when waiting (0 = not waiting)

    uint64_t         irq_stack_top;
    uint64_t         irq_rip;
    volatile uint8_t irq_active;
    uint8_t          _irq_pad[3];
    uint64_t         irq_saved_rip;
    uint64_t         irq_saved_rsp;
    uint64_t         irq_saved_rflags;
    void            *irq_pending_head;
    spinlock_t       irq_lock;

    /* set to 1 after TouchCleanupProcess runs once for THIS strand.
     * Per-strand (not per-cabin): with multi-strand cabins each strand
     * owns its own Touch subscriptions (keyed by sub->proc), so the guard
     * and the teardown are per-strand. */
    uint8_t           touch_cleaned;

    /* Head of this strand's Touch subscriptions after TouchCleanupProcess
     * splices them off the shared cabin list (linked via TouchSub.proc_next).
     * They are bucket-unlinked immediately but freed only in
     * TouchFinalizeProcess once ref_count hits 0, so an in-flight publisher
     * snapshot (which holds a proc ref) can never dereference a freed sub. */
    void             *touch_detached_subs;

    /* Phase 2K+ — CET shadow stack per-process state. */
    uintptr_t         user_ssp_phys;
    uintptr_t         user_ssp_va;
    uint32_t          user_ssp_size;

    uintptr_t         kernel_ssp_phys;
    uintptr_t         kernel_ssp_va_top;

    /* Strands (P4) — set only for a strand spawned via strand_spawn into an
     * EXISTING cabin.  hammock_base is the base VA of this strand's slot in
     * the cabin's hammock window (see cabin_layout.h); user_stack_phys is
     * the PMM allocation backing its user stack, freed on strand exit.
     * Both stay 0 for the main strand, whose stack lives at the top of the
     * address space and is reclaimed by vmm_destroy_context at cabin
     * teardown.  process_user_ssp_va_for derives the per-strand CET shadow
     * stack VA from hammock_base so sibling strands never collide. */
    uintptr_t         hammock_base;
    uintptr_t         user_stack_phys;

    /* Embedded addr-wait entry — one per strand, lifetime = process lifetime.
     * SysAddrPark reuses this rather than stack-allocating to avoid
     * use-after-return.  Zeroed by process_create's memset; linked=0 means
     * not in any bucket chain. */
    AddrWaitEntry     addr_wait_entry;

    struct process_t *hash_next;    // hash table collision chain
    struct process_t *next;         // global process list (forward)
    struct process_t *prev;         // global process list (backward) — O(1) unlink in process_destroy
    struct process_t *ready_next;   // intrusive link for ReadyQueue
    struct process_t *cleanup_next; // intrusive link for process cleanup queue
    volatile uint8_t  in_ready;     // CAS guard: 1 = currently enqueued in ReadyQueue
} process_t;

_Static_assert(sizeof(process_t) < 4096, "Process structure must fit in one page");

/* Phase 2K+ — CET SSP accessors. Get returns 0 when CET is dormant or
 * the process predates the SSP wire-up. Set is used by cet_process_create
 * / cet_process_destroy; not for general callers. user_ssp_va_for
 * returns the canonical user VA for the per-process SSP region (fixed
 * offset under the user stack). */
uintptr_t process_get_user_ssp_phys(struct process_t *proc);
uintptr_t process_get_user_ssp_va(struct process_t *proc);
uint32_t  process_get_user_ssp_size(struct process_t *proc);
void      process_set_user_ssp(struct process_t *proc, uintptr_t phys,
                                uintptr_t va, uint32_t size);
uintptr_t process_user_ssp_va_for(struct process_t *proc);
/* Guard-page VAs around the SSP region — see process.c for layout
 * diagram. Returned values are fixed (same for every process); they're
 * functions only to keep the constants out of the header. */
uintptr_t process_user_ssp_guard_hi_for(struct process_t *proc);
uintptr_t process_user_ssp_guard_lo_for(struct process_t *proc);

void process_init(void);

process_t *process_create(const char *tags);
void process_destroy(process_t *proc);

/* strand_spawn — create an additional strand (execution context) inside an
 * EXISTING cabin (shared address space / CR3 / rings / tags).  Allocates a
 * fresh process_t with its own pid, kernel stack, register frame, and a
 * user stack + CET shadow stack carved from the cabin's hammock window;
 * starts it at entry_va with `arg` in rdi and enqueues it.  Increments
 * cabin->strand_count so the cabin outlives the spawning strand.  Returns
 * the new strand or NULL on failure (fully unwound). */
process_t *strand_spawn(cabin_t *cabin, uintptr_t entry_va, uint64_t arg);

int process_load_binary(process_t *proc, const void *binary_data, size_t size);

process_t *process_find(uint32_t pid);
process_t *process_find_ref(uint32_t pid); // returns ref-counted pointer, caller must process_ref_dec()
process_t *process_get_first(void);
process_t *process_get_current(void);

// Lock/unlock the global process list for safe iteration.
// Caller must hold process_list_lock while iterating via proc->next.
// The spinlock disables IRQs, so this is safe from any context.
void process_list_lock(void);
void process_list_unlock(void);
bool spin_trylock_process_list(void);

uint32_t process_get_count(void);

/* Snapshot all live PIDs into out[] via hash-table iteration.
 * Returns number written. Used by Touch to bypass linked-list corruption
 * paths and remain robust against transient process_list races. */
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

uint64_t *process_active_memtags(process_t *proc);
void *process_get_cabin(process_t *proc);

#endif // PROCESS_H
