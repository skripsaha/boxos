#ifndef PROCESS_H
#define PROCESS_H

#include "ktypes.h"
#include "kernel_config.h"
#include "vmm.h"
#include "boxos_magic.h"
#include "boxos_limits.h"
#include "atomics.h"

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

// Fast O(1) role checks use proc->tag_bits AND g_well_known bitmasks from tagfs.h.

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
} ProcessContext;

// These offsets must match context_switch.asm — if the struct layout changes,
// update the %define constants in context_switch.asm to match.
_Static_assert(offsetof(ProcessContext, rip) == 128, "ProcessContext.rip offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, cs) == 136, "ProcessContext.cs offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, rflags) == 152, "ProcessContext.rflags offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_state) == 168, "ProcessContext.fpu_state offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_initialized) == 176, "ProcessContext.fpu_initialized offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, pl0_ssp) == 184, "ProcessContext.pl0_ssp offset mismatch with asm");

typedef struct process_t
{
    uint32_t magic;
    uint32_t pid;
    vmm_context_t *cabin;
    uint64_t cabin_info_phys;
    uint64_t pocket_ring_phys;
    uint64_t result_ring_phys;
    uint64_t touch_ring_phys;             /* kernel→user Touch event channel */

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

    uint64_t tag_bits;
    uint16_t *tag_overflow_ids;
    uint16_t tag_overflow_count;
    uint16_t tag_overflow_capacity;

    /* MemTag capabilities — per-cabin bit-mask of HELD MemTag tag_ids.
     * Inline 1024 bits (covers tag_ids 0..1023). For larger tag_ids the
     * mask is sparse — currently capped at 1024 (production-typical tag
     * count is <500). Phase 2B PTE-bit enforcement reads this on every
     * tag-checked access; Phase 2A uses it for soft `mem_check_access`.
     *
     * Mutation is atomic per uint64_t word (single-bit set/clear via
     * __atomic_*). No spinlock needed for word-bounded ops. Cross-word
     * batch ops (grant_many) take memtag_lock briefly.
     *
     * Default: all zeros. Guard-flagged regions (MEMTAG_FLAG_GUARD on
     * tag_registry entry) require cabin to hold the tag bit to access. */
    uint64_t active_memtags[16];   /* 1024 bits total */

    uintptr_t code_start;
    size_t code_size;

    ProcessContext context;
    void *kernel_stack;
    void *kernel_stack_top;
    void *kernel_stack_guard_base;

    bool started;
    uint8_t home_core;              // App Core index for scheduling (RunQueue lives here)
    volatile uint8_t kcore_pending; // 1 = already in a K-Core queue, 0 = free (atomic CAS)
    uint32_t spawner_pid;
    uint64_t buf_heap_next; // next free virtual address for buffer mapping

    // ASLR: per-process randomized addresses (set at creation time)
    uint64_t aslr_heap_base;     // actual heap start (CABIN_HEAP_BASE + random)
    uint64_t aslr_stack_top;     // actual stack top (USER_STACK_TOP - random)
    uint64_t aslr_buf_heap_base; // actual buffer heap start

    volatile wait_reason_t wait_reason;
    uint64_t wait_start_time; // TSC timestamp when waiting (0 = not waiting)

    /* Touch subscriber list — head of doubly-linked TouchSub chain. Each
     * sub also lives on a TouchBucket's bucket list (publish-side index).
     * subs_lock orders link/unlink between TouchClaimSet/Clear and the
     * O(N_my_claims) walk in TouchCleanupProcess. */
    void       *subs_head;
    spinlock_t  subs_lock;

    /* Bay claim list — per-cabin head of BayClaim chain. Walked by
     * BayCleanupProcess at process_destroy to release every claim this
     * cabin holds before vmm_destroy_context tears down the page tables.
     * bay_va_next is a bump cursor inside CABIN_BAY_BASE..CABIN_BAY_END
     * used to assign user-VA windows to incoming Bay maps. */
    void       *bay_claims_head;
    spinlock_t  bay_lock;
    uint64_t    bay_va_next;

    /* Brook claim list — per-cabin head of BrookClaim chain. Same
     * lifecycle pattern as Bay: BrookCleanupProcess walks the list
     * during process_destroy, drops every BrookObject reference (peer
     * wakes with -ERR_BROKEN_PIPE), unmaps the per-claim VA windows.
     * brook_va_next bump-allocates inside CABIN_BROOK_BASE..CABIN_BROOK_END.
     * Held while linking/walking claims; no kernel allocation under this
     * lock (Brook ops do PMM/VMM work without holding it). */
    void       *brook_claims_head;
    spinlock_t  brook_lock;
    uint64_t    brook_va_next;

    uint64_t         irq_stack_top;
    uint64_t         irq_rip;
    volatile uint8_t irq_active;
    uint8_t          _irq_pad[3];
    uint64_t         irq_saved_rip;
    uint64_t         irq_saved_rsp;
    uint64_t         irq_saved_rflags;
    void            *irq_pending_head;
    spinlock_t       irq_lock;

    uint8_t           touch_cleaned; // set to 1 after TouchCleanupProcess runs once

    /* Phase 2K+ — CET shadow stack per-process state. Populated by
     * cet_process_create when g_cpu_caps.has_shstk + CR4.CET=1; left
     * zero on CPUs without SHSTK or when CET is dormant. The user SSP
     * page is mapped into the process's vmm_context with PTE bit 61
     * (Intel SDM Vol 3A §4.5.1 — user shadow stack). user_ssp_va is
     * the initial SSP value (top of the SSP region minus 8) — what
     * jump_to_userspace writes into IA32_PL3_SSP before the iretq to
     * Ring 3. After that first transition the kernel maintains SSP
     * via the XSAVE CET_U component (XCR0 bit 12) without touching
     * the MSR directly. */
    uintptr_t         user_ssp_phys;
    uintptr_t         user_ssp_va;
    uint32_t          user_ssp_size;

    /* Per-process supervisor shadow stack — backing for the process's
     * kernel CALL/RET tracking when S_CET.SH_STK_EN=1. Each process gets
     * its own 4 KiB kernel SSP page; context switches between processes
     * swap IA32_PL0_SSP so process A's kernel CALLs don't pollute process
     * B's shadow stack.
     *
     * Layout of the SSP page (kernel direct map, PTE bit 60 set):
     *   [top - 0]    supervisor SSP token (Intel SDM Vol 1 §17.2.3):
     *                 value = top | 0x1 (mode bit)
     *   [top - 8]    pre-pushed kernel entry RIP — what task_restore_context's
     *                 first RET pops off the shadow stack to match the regular-
     *                 stack push (otherwise the brand-new process's first RET
     *                 would #CP against an empty shadow stack).
     *
     * ProcessContext.pl0_ssp is initialised to (top - 8) so the very first
     * task_restore_context's WRMSR IA32_PL0_SSP lands on the pre-pushed
     * entry; subsequent CALL/RET pairs grow the stack inside the page.
     *
     * Zero on processes created before SHSTK activation, on CPUs without
     * SHSTK, and on K-Core threads (kernel threads use per-CPU PL0_SSP
     * from cet_lifecycle_init_supervisor_ssp). */
    uintptr_t         kernel_ssp_phys;
    uintptr_t         kernel_ssp_va_top;

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

#endif // PROCESS_H
