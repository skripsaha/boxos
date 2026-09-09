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

/*
 * Lock ordering:
 *   1. scheduler_lock  (scheduler.c) — acquired BRIEFLY only to check current_process
 *   2. process_lock    (process.c)   — global list protection
 *   3. cleanup_queue.lock            — deferred cleanup queue
 *   4. process_t->state_lock         — per-process state
 *   5. process_t->chit.lock          — the innermost leaf: taken under process_lock,
 *                                     an addr-wait bucket lock, gone_lock, or the PIT
 *                                     tick; takes nothing itself (chit.h)
 *
 * process_destroy() releases scheduler_lock before any resource cleanup.
 * schedule() handles periodic cleanup of finished processes.
 * process_cleanup_deferred() runs without scheduler_lock or process_lock.
 */

#define PROCESS_MAX_COUNT MAX_PROCESSES
#define PROCESS_TAG_SIZE 256
#define PROCESS_INVALID_PID 0

/* quiesce_core sentinel: strand was never dispatched on any core, so no core
 * can be in its stack epilogue — it is immediately reapable. */
#define QUIESCE_CORE_NONE 0xFFu

// Fast O(1) role checks use proc->cabin->tag_bits AND g_well_known bitmasks from tagfs.h.

struct process_t;

/* One Touch event the kernel accepted for a strand's ring but could not fit
 * in it. Defined in touch.c — nothing outside Touch takes it apart. */
struct TouchOwed;

/* One strand waiting for a named incarnation to be gone (SysProcessGone).
 * Allocated by the waiter, owned by the awaited process's gone list, freed by
 * whoever delivers the answer — the death that drains the list, or the waiter
 * itself when it wins the race and never parks. */
typedef struct GoneWaiter {
    struct process_t  *waiter;
    uint32_t           want_generation; /* the incarnation asked about */
    uint32_t           submit_cookie;   /* cloakroom token of its submit */
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

    /* PID-allocator generation for this pid slot, snapshotted at creation
     * (pid_generation(pid)). The (pid, generation) pair is the canonical
     * process identity: a recycled pid always carries a different generation
     * than it did in a previous life, so a supervisor that matches a death by
     * (pid, generation) never mistakes a recycled pid for the dead one. Rides
     * in process:died (TouchProcessDied.generation). Set once, never changes. */
    uint32_t generation;

    /* Pointer to the shared cabin (address space + IPC rings + Bay/Brook/Touch).
     * NULL for the idle process.  All cabin-shared state lives in cabin_t. */
    cabin_t *cabin;

    int32_t score;
    uint64_t last_run_time;
    uint32_t consecutive_runs;
    /* Processor time this process has been given, in MICROSECONDS, summed at
     * every context switch away from it (scheduler.c) and read by
     * system.proc.cputime, which backs std::clock(). Whole slices only: the
     * slice in progress is not in it. */
    uint64_t total_cpu_time;
    /* TSC at the moment this process was given the core. The pair above is
     * measured with the TSC and not with the scheduler tick, and that is not a
     * refinement: at tick granularity whoever is current when the tick fires is
     * credited the WHOLE tick, so two processes alternating every tick are each
     * credited 100% of the wall clock. That was measured on a one-core boot —
     * cxxtest and pid 1 ping-ponged every tick, both fully credited — and it is
     * what made std::clock() indistinguishable from wall time. Zero means "not
     * currently holding the core". */
    uint64_t cpu_tsc_stamp;
    int8_t current_prio;            // Current scheduler priority level (set on enqueue)
    int8_t rq_prio;                 // Priority level in runqueue (-1 = not enqueued)
    int16_t rq_index;               // Index in queue (-1 = not enqueued)
    /* Running-XOR-runqueue interlock: the core index this strand is currently
     * dispatched on, or -1 if on no core. schedule() CAS-claims it before
     * committing the strand as current_process and releases it on switch-out, so
     * the SAME process_t can never run on two cores at once (which would tear its
     * shared context via concurrent context_save/restore). Init -1 by
     * process_init_strand_fields / idle_setup. int16_t holds every MAX_CORES
     * (up to 256) index alongside the -1 sentinel — int8_t would alias core 255
     * onto -1. */
    volatile int16_t on_cpu;

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

    /* Owed — Touch events accepted for THIS strand's ring that did not fit in
     * it. A claim is a promise, so what the ring refuses is held here in the
     * order the ring would have carried it, and handed over at the next door
     * the strand comes to (the syscall gate, idt.c).
     *
     * The queue hangs on the RING's owner, never on a subscription: it is the
     * ring that is full, and one ring is one stream with one order. Its depth
     * is mirrored into the ring header's `owed` slip so a consumer asleep on
     * that cacheline learns the kernel is still holding its events.
     *
     * owed_lock is a LEAF lock: held only to link/unlink one node and publish
     * the count. KTouchPush is NEVER called under it — the drainer takes a node
     * off the queue, drops the lock, pushes, and re-links at the head on
     * refusal. owed_count is the queue length PLUS the at-most-one node a
     * drainer holds in flight, so it never reads 0 while an event is still
     * owed. owed_draining is the single-drainer token. See touch.c. */
    struct TouchOwed *owed_head;
    struct TouchOwed *owed_tail;
    spinlock_t        owed_lock;
    volatile uint32_t owed_count;
    volatile uint32_t owed_draining;

    /* Phase 2K+ — CET shadow stack per-process state. */
    uintptr_t         user_ssp_phys;
    uintptr_t         user_ssp_va;
    uint32_t          user_ssp_size;

    uintptr_t         kernel_ssp_phys;
    uintptr_t         kernel_ssp_va_top;

    /* Strands (P4) — set only for a strand spawned via strand_spawn into an
     * EXISTING cabin.  hammock_base is the base VA of this strand's slot in
     * the cabin's Hammock window (see cabin_layout.h); user_stack_phys is
     * the PMM allocation backing its user stack, freed on strand exit.
     * Both stay 0 for the main strand, whose stack lives at the top of the
     * address space and is reclaimed by vmm_destroy_context at cabin
     * teardown.  process_user_ssp_va_for derives the per-strand CET shadow
     * stack VA from hammock_base so sibling strands never collide. */
    uintptr_t         hammock_base;
    uintptr_t         user_stack_phys;

    /* Zombie-until-join (std::thread conformance). When a strand is spawned
     * JOINABLE (strand_spawn joinable=1, used by std::thread), this is 1 and the
     * P5b reaper will NOT reclaim it on exit — it lingers as a zombie holding its
     * pid, so thread::id (==pid) stays unique while the std::thread is joinable.
     * SYSTEM_OP_STRAND_RELEASE (join()/detach()) clears it (SEQ_CST) → the reaper
     * then reclaims it. 0 for raw strand_spawn workers (eager reap, as before)
     * and the main strand. Zeroed by the spawn memset. */
    volatile uint8_t  reap_blocked;

    /* Reap-vs-switch quiescence stamp. The scheduler runs on the dispatched
     * strand's OWN kernel stack, and a switch only finishes at the iretq AFTER
     * current_process has already moved to the incoming strand — so an exited
     * strand is briefly off the run state yet still has the outgoing core in its
     * stack epilogue. schedule() stamps quiesce_core (the core that last ran
     * this strand) + quiesce_seq (that core's quiesce_seq at eviction) whenever
     * it switches the strand out; the reaper must not free the strand until
     * scheduler_get_core(quiesce_core)->quiesce_seq has advanced past quiesce_seq
     * (that core has since dispatched again, hence left this stack). quiesce_core
     * == QUIESCE_CORE_NONE means the strand was never dispatched → immediately
     * reapable. Set to QUIESCE_CORE_NONE by process_init_strand_fields. */
    uint8_t           quiesce_core;
    uint64_t          quiesce_seq;

    /* Per-strand IPC rings (P5a).  kring.c / touch_ring.c route by THESE
     * (not by cabin->*_ring_phys), so concurrent multi-strand syscalls never
     * share ring storage — the P4→P5 data-race fix.
     *   Main strand : aliases cabin->{pocket,result,touch}_ring_phys (the
     *                 fixed-VA rings) and strandinfo_phys == 0 (FS base 0).
     *   Spawned     : its own header pages mapped into the Hammock slot by
     *                 strand_rings_create, plus a StrandInfo TLS block whose
     *                 VA becomes the strand's FS base.
     * strandinfo_phys is the StrandInfo block's first physical page (0 for
     * the main strand); strand_rings_destroy frees the rings/slots/StrandInfo. */
    uint64_t          pocket_ring_phys;
    uint64_t          result_ring_phys;
    uint64_t          touch_ring_phys;
    uint64_t          strandinfo_phys;

    /* Ф20e — crash-orphan stamp target for this strand's boxlib StrandPool.
     * Set when the strand binds its slab slot (SYSTEM_OP_STRAND_POOL_BIND):
     * strand_pool_va is the user virtual address of its StrandPool node and
     * strand_pool_gen the generation it bound at. We store the VA, NOT a phys:
     * process_destroy re-resolves it through the live cabin page tables at death,
     * so a page unmapped/recycled between bind and death can never make us stamp a
     * phys that now belongs to another cabin. The CAS marks the node's GenState
     * (gen<<8|LIVE)→(gen<<8|ORPHANED) so a surviving strand reclaims the cached
     * blocks. An orderly flush bumps the generation first, so the CAS misses and
     * the stamp is a no-op. Both 0 for the main strand and any strand that never
     * claimed a pool. */
    uint64_t          strand_pool_va;
    uint32_t          strand_pool_gen;
    uint64_t          strand_pool_orphan_va;

    /* Embedded addr-wait entry — one per strand, lifetime = process lifetime.
     * SysAddrPark reuses this rather than stack-allocating to avoid
     * use-after-return.  Zeroed by process_create's memset; linked=0 means
     * not in any bucket chain. */
    AddrWaitEntry     addr_wait_entry;

    /* The kernel's half of the cloakroom token: the answer this strand was
     * promised, by whom, and whether the event that makes it has come. One
     * slot — a strand holds out for one token at a time. See chit.h. */
    Chit              chit;

    /* The deadline's baton. A timed park's ERR_TIMEOUT is delivered by a
     * K-Core (KResultPush touches the cabin VMM, never allowed in the tick),
     * and the node that carries that delivery lives HERE, so the tick's pass
     * cannot fail for want of a slot — irq_defer, which used to carry it,
     * drops when its chunk has no successor, and a dropped deadline was a
     * park that never returned (boxlib covered it with a +100 ms clock of
     * its own). One slot, one gate: `deadline_passed` is 1 while the baton
     * is queued; a deadline that fires while it is still queued is folded
     * into that pass, which judges by the park's state when it runs, not by
     * which fire woke it. The pass holds a ref on this process until run. */
    Baton             deadline_baton;
    volatile uint8_t  deadline_passed;

    /* Who is waiting for THIS process to be gone.
     *
     * The list hangs on the awaited process, not in a side registry, because
     * the thing being waited for is exactly this record's existence: while a
     * waiter is attached the record is held (a deposit is not abandoned), and
     * the one place that ends a life is the one place that drains the list —
     * no lookup, no hashing, no window between "it died" and "someone finds
     * out". Guarded by gone_lock, which nests INSIDE nothing: it is taken
     * alone, and the wake it performs happens after it is dropped. */
    struct GoneWaiter *gone_waiters;
    spinlock_t        gone_lock;

    /* Which sleep a wake belongs to.
     *
     * Every timed wait — addr_park and touch_await alike — arms a wake in the
     * touch queue and then parks. A wait that ends EARLY leaves its wake
     * armed: taking it back would mean walking the queue under the tick's
     * lock, and it was believed harmless because the Result the wake carries
     * is already refused when it no longer belongs to anyone. It was not
     * harmless. The reschedule half of that same wake consulted nothing, so a
     * wake owed to a finished sleep pulled the strand out of the NEXT one —
     * awake, with no Result coming, spinning out the rest of a timeout that
     * was never its own. Measured: a 300 ms sleep cost 250 ms of processor
     * time whenever an earlier wait had left a wake behind.
     *
     * Bumped once on entry to every timed park; the wake carries the value it
     * was armed with; the reschedule happens only when the two still agree.
     * A stale wake then finds a strand that has moved on and does nothing,
     * which costs one comparison in the tick and needs no cancellation. */
    uint32_t          park_seq;

    /* Nameplate — where this image's "address -> name" table is mapped, found
     * by the loader (nameplate_locate) at the one moment the whole file is in
     * kernel memory. 0 when the image carries none. Read only by the
     * user-mode fault dump, and only through get_user. */
    uintptr_t         nameplate_va;
    uint64_t          nameplate_bytes;

    struct process_t *hash_next;    // hash table collision chain
    struct process_t *next;         // global process list (forward)
    struct process_t *prev;         // global process list (backward) — O(1) unlink in process_destroy
    struct process_t *ready_next;   // intrusive link for ReadyQueue
    struct process_t *cleanup_next; // intrusive link for process cleanup queue
    atomic_u32_t      cleanup_enqueued; // one-shot 0->1 CAS guard: gates cleanup-queue enqueue (double-free defense). Zeroed by process_create's memset.
    volatile uint8_t  in_ready;     // CAS guard: 1 = currently enqueued in ReadyQueue

    /*
     * ‼ A TLS BASE THAT HAS BEEN ASKED FOR AND NOT YET INSTALLED.
     *
     * ProcessContext.user_fsbase means "what the hardware had when this
     * strand was switched out" — it is written by the SAVE, from the live
     * MSR. That is right for a strand that installed its own base with
     * WRFSBASE, and it is the only writer on a CPU that has FSGSBASE.
     *
     * A CPU without FSGSBASE cannot write the base from ring 3 at all, so the
     * base arrives through SYSTEM_OP_TLS_FSBASE — and that op runs on a K-Core
     * in the guide loop, which is not the caller's CPU and cannot program its
     * MSR. Writing the value into user_fsbase from there put a SECOND writer
     * on the field, and the two disagreed: the very next save read the
     * hardware (which had never been told) and overwrote the request with the
     * zero that was still in the register. The restore then put that zero
     * back, and the first fs-relative instruction in the program faulted at
     * address 0.
     *
     * Measured: every C++ binary on a Braswell laptop, and reproduced exactly
     * under QEMU with -cpu qemu64 (no fsgsbase). Intermittent on the board
     * only because a timer tick landing in the two-instruction window installs
     * the base first, after which the save reads the right value.
     *
     * So the request lives here, where nothing reads the hardware. The save
     * declines to run while one is owed — the register is stale by definition
     * — and the restore installs it and clears the debt.
     */
    volatile uint8_t  fsbase_owed;  // 1 = fsbase_wanted has not reached the CPU yet
    uint64_t          fsbase_wanted;
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
 * the new strand or NULL on failure (fully unwound).
 *
 * joinable=true (std::thread) sets reap_blocked so the strand becomes a zombie
 * on exit (pid held) until SYSTEM_OP_STRAND_RELEASE; joinable=false (raw worker)
 * keeps the eager-reap behavior. */
process_t *strand_spawn(cabin_t *cabin, uintptr_t entry_va, uint64_t arg, bool joinable);

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

/* Shutdown-only: reclaim process-subsystem locks after all AP cores are
 * halted, so a dead lock-holder (e.g. an AP stopped mid strand-reaper) can't
 * wedge the BSP's shutdown walk. See process.c — do NOT call in normal run. */
void process_force_release_locks_for_shutdown(void);

/* P5b strand reaper — runtime reclamation of exited strands AND processes.
 *
 * A strand that exits (strand_exit → PROC_DONE) or crashes (PROC_CRASHED)
 * cannot destroy itself while running, so it lingers as a zombie holding its
 * pid + process_count slot + per-strand rings. Without this, std::thread-style
 * churn would exhaust the process table. process_reap_strands scans for exited
 * corpses — both spawned strands (hammock_base != 0) AND full processes / main
 * strands (hammock_base == 0, e.g. proc_exec children that called exit()) — that
 * are no longer current on any core and process_destroy's them (feeding the
 * existing deferred-cleanup queue; the last strand's reap also tears the cabin
 * down via cabin_ref_dec). Called periodically from the K-Core run loop.
 * Single-reaper-at-a-time (internal guard) so two cores never destroy the same
 * zombie. (Joinable std::thread strands stay zombies until join/detach clears
 * reap_blocked — their pid == thread::id must not recycle while a live handle
 * holds it; full processes carry no such handle, so they reap eagerly.) */
void process_reap_strands(void);

uint64_t *process_active_memtags(process_t *proc);
void *process_get_cabin(process_t *proc);

#endif // PROCESS_H
