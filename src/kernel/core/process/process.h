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
#include "touch_queue.h"
#include "boxos_crate.h"
#include "image_load.h"

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
#define PROCESS_INVALID_PID 0

/* quiesce_core sentinel: strand was never dispatched on any core, so no core
 * can be in its stack epilogue — it is immediately reapable. */
#define QUIESCE_CORE_NONE 0xFFu

struct process_t;

/* One Touch event the kernel accepted for a strand's ring but could not fit
 * in it. Defined in touch.c — nothing outside Touch takes it apart. */
struct TouchOwed;

/* One answer the kernel accepted for a strand's REPLY ring but could not fit
 * in it. Defined in kring.c — the same shape one ring over. */
struct ResultOwed;

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

/* One question a strand asked and the carrier it asked has not answered yet
 * (SysAsk). It hangs on the carrier, beside the gone list and under the same
 * lock, for the same reason: the one place a life ends is the one place that
 * settles what the life still owed — here with ERR_PROCESS_NOT_FOUND. The
 * asker's (pid, cookie) names it; the asker's staged crates are the question's
 * until it is settled, the answer going into the asker's answer box. */
typedef struct Question {
    struct process_t *asker;
    uint32_t          cookie;        /* the asker's submit token */
    uint16_t          crate_count;
    uint16_t          answer_box;    /* index of the answer crate, CRATE_INDEX_NONE if none */
    Crate            *crates;        /* the asker's staged crates, owned until settled */
    uint64_t          crates_uaddr;
    uint64_t          deposit;       /* where the carrier was lent the question's bytes: */
    uint32_t          deposit_length;/*   taken back with its answer (system_deck.c) */
    struct Question  *next;
} Question;

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

/* The answers a strand's sleep check notes before it looks at its whole
 * reply ring (process_t.answers_at). */
#define PROCESS_ANSWERS_NOTED 8

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
    /* Processor time this process has been given, in MICROSECONDS, summed at
     * every context switch away from it (scheduler.c). Whole slices only: the
     * slice in progress is not in it — process_cpu_time_us adds it, and is
     * what system.proc.cputime (std::clock()) and system.proc.crew read. */
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
    /* How long this strand holds a core each time it has one, in TSC cycles:
     * a mean of its holds, the latest weighing a quarter, taken as each ends
     * (scheduler.c). One that holds a least turn or more is worth a core of
     * its own when it wakes and its home is working; a short one takes its
     * turn at home. */
    uint64_t hold_mean;
    /* The Fare: the time this strand has held a core, in TSC cycles, on the
     * scale of one core's Waterline — fare_core's, -1 until it is first
     * placed. A strand whose cabin wears the Use Context pays a third of it
     * (scheduler.c). fare_tsc is when it was last charged, zero before. */
    uint64_t fare;
    uint64_t fare_tsc;
    int16_t fare_core;
    /* The Tip: the time K-Cores have spent serving this strand's pockets, at
     * the same price as the Fare, on the scale of one K-Core's Waterline —
     * tip_core's, -1 until its first pocket is held (kcore.c). Only the K-Core
     * holding or serving the strand touches the two. */
    uint64_t tip;
    int16_t tip_core;
    int32_t rq_index;               // Slot in its core's runqueue (-1 = not enqueued)
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

    /* Where the kernel published answers to this strand's submits it may not
     * have read yet (kring.c): a sleep is refused across any of them, and only
     * these are looked at — not every record waiting in its reply ring.
     * Spilled when more were published than there is room to note; the whole
     * unread ring is looked at then, and what it finds noted again. Under
     * state_lock. */
    uint64_t answers_at[PROCESS_ANSWERS_NOTED];
    uint8_t  answers_noted;
    uint8_t  answers_spilled;

    atomic_u32_t ref_count;

    // Atomic flag: 1 = process is being destroyed, 0 = normal.
    // Set BEFORE releasing scheduler_lock in process_destroy().
    // Checked in scheduler_select_next() to prevent selecting doomed process.
    volatile uint8_t destroying;

    ProcessContext context;
    void *kernel_stack;
    void *kernel_stack_top;
    void *kernel_stack_guard_base;
    pte_t kernel_stack_guard_leaf;  // the Pull Map leaf the guard replaced — restored verbatim at free

    bool started;
    uint8_t home_core;              // App Core index for scheduling (RunQueue lives here)
    volatile uint8_t kcore_pending; // 1 = already in a K-Core queue, 0 = free (atomic CAS)
#ifdef CONFIG_MEASURE
    /* When this strand's call came to the gate and when it was taken to be
     * served, TSC — the stopwatch its answer carries (kring.h). */
    uint64_t call_gate;
    uint64_t call_taken;
    uint64_t call_woke;      /* when the K-Core that took it last woke */
    uint64_t call_mark[5];   /* peeked, manifest, staged, executed, committed (KCallMark) */
    /* Its asks to sleep (SysTurnIn), as KCallTurnIn counts them. */
    uint32_t turnin_count[5];
#endif

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

    /* Owed, the reply ring's half — answers ACCEPTED for THIS strand's
     * ResultRing that did not fit in it (kring.c). Same mechanism, same word,
     * a second queue because it is a second ring with a second order.
     *
     * The one difference is the bound, and it is deliberate: there is none.
     * The Touch queue holds the event itself, so kernel memory grows with its
     * depth and a ring's worth is where that stops. Here the payload was
     * already deposited into the TARGET'S OWN cabin heap before the push was
     * refused (system_ops.c, ipc_copy_to_heap) — what is held is a 24-byte
     * Result descriptor, and its bound is kmalloc saying no. A depth number on
     * top of that would be a second, invented limit on a resource that already
     * has a real one. See the block comment in kring.c.
     *
     * Each node OWNS the cabin-heap deposit its Result names, so a queue that
     * dies with its strand takes those pages with it (ResultOwedRelease). */
    struct ResultOwed *result_owed_head;
    struct ResultOwed *result_owed_tail;
    spinlock_t        result_owed_lock;
    volatile uint32_t result_owed_count;
    volatile uint32_t result_owed_draining;

    /* Phase 2K+ — CET shadow stack per-process state. */
    uintptr_t         user_ssp_phys;
    uintptr_t         user_ssp_va;
    uint32_t          user_ssp_size;

    uintptr_t         kernel_ssp_phys;
    uintptr_t         kernel_ssp_va_top;

    /* Strands (P4) — set only for a strand spawned via strand_spawn into an
     * EXISTING cabin.  hammock_base is the base VA of this strand's slot in
     * the cabin's Hammock window (see cabin_layout.h); its user stack is
     * registered in the cabin's address space (vmm_stack_register) and comes
     * in as the strand touches it, and goes at strand exit.  hammock_base
     * stays 0 for the main strand, whose stack lives at the top of the
     * address space and is reclaimed by vmm_destroy_context at cabin
     * teardown.  process_user_ssp_va_for derives the per-strand CET shadow
     * stack VA from hammock_base so sibling strands never collide. */
    uintptr_t         hammock_base;

    /* Set by the fault handler when this strand's stack ran into the gap
     * below it. process_destroy records the death as PROC_EXIT_STACK_OVERFLOW
     * instead of PROC_EXIT_CRASHED. */
    volatile uint8_t  stack_overran;

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
    /* The slots the kernel made that ResultRing with — its own record, never
     * the header's slot_count_max, which lives in a page the cabin writes. */
    uint32_t          result_ring_slots;
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

    /* The questions asked of this process and not answered yet (Question,
     * above). Under gone_lock, settled with the gone list at its end. */
    struct Question   *questions;

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

    /* That wake's node, carried here so arming it cannot fail for want of
     * memory (touch_queue.h). One park at a time, so one node: a park armed
     * while the last one's wake still waits moves it to its own moment. */
    TouchQueueNode    park_wake;

    /* The strand's alarm (alarm_ops.c): the moment it is set for, in µs of
     * uptime, 0 when none — the one word that says whether an alarm is
     * pending, claimed by whoever delivers it. Its node waits in the touch
     * queue, carried here so setting an alarm cannot fail for want of memory,
     * and the baton is the pass from the fire to a K-Core, where the Touch is
     * delivered; alarm_passed is its gate, as deadline_passed is the park's. */
    volatile uint64_t alarm_moment;
    TouchQueueNode    alarm_node;
    Baton             alarm_baton;
    volatile uint8_t  alarm_passed;

    /* The newest change of each volume this strand's own calls made — the
     * place of their newest Ink, which tells a mount before this one apart
     * (tagfs.h, TagFSNoteInker): the home volume's here, every other one's in
     * the list after it. A strand that ends with one unsettled demands that
     * volume's next Settle cycle; a strand that only read owes nothing. */
    volatile uint64_t           volume_ink_place;
    struct VolumeInkDebt*       volume_ink_more;

    /* Nameplate — where this image's "address -> name" table is mapped, found
     * by the loader (nameplate_locate) at the one moment the whole file is in
     * kernel memory. 0 when the image carries none. Read only by the
     * user-mode fault dump, and only through get_user. */
    uintptr_t         nameplate_va;
    uint64_t          nameplate_bytes;
    /* What the loader added to every place of the image: the table names
     * addresses where the image was linked, so an address it is asked about
     * is taken back by this first. 0 for an image where it was linked. */
    uintptr_t         image_base;

    struct process_t *hash_next;    // hash table collision chain
    struct process_t *next;         // global process list (forward)
    struct process_t *prev;         // global process list (backward) — O(1) unlink in process_destroy
    struct process_t *ready_next;   // intrusive link for ReadyQueue
    struct process_t *cleanup_next; // intrusive link for process cleanup queue
    struct process_t *dead_next;    // intrusive link for the reaper's list of the dead
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

/* The program `source` gives made this process's: put into its cabin by
 * ImageLoad (image_load.h) — `name` is what every line about it calls it —
 * then given its stack, its heap and its CabinInfo. OK, or why not; a process
 * whose load failed is the caller's to destroy, never to start. */
error_t process_load_binary(process_t *proc, const ImageSource *source, const char *name);

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

void process_start_initial(process_t *proc);

bool process_has_tag(process_t *proc, const char *tag);
bool process_has_tag_id(process_t *proc, uint32_t tag_id);
/* OK, or why the process does not wear the tag: a text that does not spell
 * (tag_spelling.h), the volume's refusal of a number, no volume for an
 * ordinary tag, or ERR_TAG_OVERFLOW_FAILED. */
error_t process_add_tag(process_t *proc, const char *tag);
int process_remove_tag(process_t *proc, const char *tag);
size_t process_snapshot_tags(process_t *proc, char *buffer, size_t buffer_size);

void process_ref_inc(process_t *proc);
void process_ref_dec(process_t *proc);
uint32_t process_ref_count(process_t *proc);

/* Processor time `proc` has been given, in microseconds: its completed slices
 * and the one it is running now. What system.proc.cputime says of the caller
 * and system.proc.crew of each mate. */
uint64_t process_cpu_time_us(const process_t *proc);

/* Returns true when this call made the strand runnable AND enqueued it —
 * sched_enqueue rang its core, so the caller must not ring again. */
bool process_set_state(process_t *proc, process_state_t new_state);
/* An answer published for `proc` at reply-ring position `pos` — `head` the
 * ring's head as the publisher read it — noted where its sleep check looks,
 * and the strand made runnable, in one hold of its state lock: a sleep
 * committed before the note is ended by the move, one after it sees the
 * note. Whether it enqueued. */
bool process_answer_published(process_t *proc, uint64_t pos, uint64_t head);
/* A process let run as it was made: PROC_CREATED -> PROC_WORKING under the
 * state lock, enqueued as process_set_state enqueues it. False, the state left
 * as it stands, when it is no longer as made: a stop or an end decided over it
 * before it ever ran is not undone. */
bool ProcessStartCreated(process_t *proc);
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
 * churn would exhaust the process table. Every strand whose death is committed
 * while it is listed goes, at that moment, onto a list of the dead — both
 * spawned strands (hammock_base != 0) AND full processes / main strands
 * (hammock_base == 0, e.g. proc_exec children that called exit()) — and
 * process_reap_strands takes from that list alone the corpses no longer
 * current on any core and process_destroy's them (feeding the existing
 * deferred-cleanup queue; the last strand's reap also tears the cabin down via
 * cabin_ref_dec): a reap costs the dead, never the living. Called
 * periodically from the K-Core run loop.
 * Single-reaper-at-a-time (internal guard) so two cores never destroy the same
 * zombie. (Joinable std::thread strands stay zombies until join/detach clears
 * reap_blocked — their pid == thread::id must not recycle while a live handle
 * holds it; full processes carry no such handle, so they reap eagerly.) */
void process_reap_strands(void);

uint64_t *process_active_memtags(process_t *proc);
void *process_get_cabin(process_t *proc);

#endif // PROCESS_H
