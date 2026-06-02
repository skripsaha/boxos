#include "process.h"
#include "klib.h"
#include "kernel_config.h"
#include "pmm.h"
#include "pmtag.h"
#include "cabin_layout.h"
#include "kring.h"
#include "touch_ring.h"
#include "vmm.h"
#include "scheduler.h"
#include "gdt.h"
#include "tss.h"
#include "userspace.h"
#include "atomics.h"
#include "async_io.h"
#include "pid_allocator.h"
#include "cpu_caps_page.h"
#include "fpu.h"
#include "aslr.h"
#include "cabin_info.h"
#include "notify.h"
#include "touch.h"
#include "bay.h"
#include "brook.h"
#include "tagfs.h"
#include "per_core.h"
#include "amp.h"

typedef struct
{
    struct process_t *head;
    struct process_t *tail;
    uint32_t          count;
    spinlock_t        lock;
} process_cleanup_queue_t;

static process_t *process_list_head = NULL;
static volatile uint32_t process_count = 0;
static spinlock_t process_lock;
static process_cleanup_queue_t g_cleanup_queue;

// Round-robin counter for App Core assignment (multi-core only).
static volatile uint32_t g_appcore_rr_counter = 0;

// Tag overflow reallocation with lock-free read protection.
//
// Readers (scheduler_matches_use_context) use this pattern:
//   1. Read overflow_ids pointer atomically
//   2. Read overflow_count atomically
//   3. Loop using these stable values
//
// Writers (this function) use publish-before-free:
//   1. Allocate new buffer
//   2. Copy data to new buffer
//   3. Atomic store-release of new pointer (publishes to readers)
//   4. Atomic store-release of new capacity
//   5. Memory barrier (ensures all writes visible before kfree)
//   6. Free old buffer (safe — readers see either old or new pointer)
//
// Lock ordering: caller must hold process_lock when modifying tags.
static int process_set_tag_bit(process_t *proc, uint16_t tag_id)
{
    if (tag_id < 64)
    {
        __atomic_or_fetch(&proc->tag_bits, ((uint64_t)1 << tag_id), __ATOMIC_RELAXED);
        return 0;
    }

    // Check if already exists (caller holds process_lock, no concurrent modification)
    for (uint16_t i = 0; i < proc->tag_overflow_count; i++)
    {
        if (proc->tag_overflow_ids[i] == tag_id)
            return 0;
    }

    // Need to realloc?
    if (proc->tag_overflow_count >= proc->tag_overflow_capacity)
    {
        uint16_t new_cap = proc->tag_overflow_capacity == 0 ? 8 : proc->tag_overflow_capacity * 2;
        uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids)
            return -1;

        uint16_t *old_ids = proc->tag_overflow_ids;
        if (old_ids)
        {
            memcpy(new_ids, old_ids, sizeof(uint16_t) * proc->tag_overflow_count);
        }

        // CRITICAL: Atomic publish with release semantics.
        // Readers using atomic load-acquire will see:
        //   - Either old pointer + old count (before publish)
        //   - Or new pointer + new count (after publish)
        // Never: new pointer + old count (would read garbage)
        __atomic_store_n(&proc->tag_overflow_ids, new_ids, __ATOMIC_RELEASE);
        __atomic_store_n(&proc->tag_overflow_capacity, new_cap, __ATOMIC_RELEASE);

        // Full memory barrier before kfree — ensures old_ids is not freed
        // until all readers have completed their load-acquire.
        mfence();

        // Safe to free: any reader that saw old pointer is done,
        // any reader that sees new pointer never touches old_ids.
        if (old_ids)
        {
            kfree(old_ids);
        }
    }

    // Append new tag_id (caller holds process_lock, safe to modify count)
    proc->tag_overflow_ids[proc->tag_overflow_count++] = tag_id;
    return 0;
}

static int process_clear_tag_bit(process_t *proc, uint16_t tag_id)
{
    if (tag_id < 64)
    {
        proc->tag_bits &= ~((uint64_t)1 << tag_id);
        return 0;
    }
    for (uint16_t i = 0; i < proc->tag_overflow_count; i++)
    {
        if (proc->tag_overflow_ids[i] == tag_id)
        {
            proc->tag_overflow_ids[i] = proc->tag_overflow_ids[proc->tag_overflow_count - 1];
            proc->tag_overflow_count--;
            return 0;
        }
    }
    return -1;
}

/* Hash table for O(1) process_find(pid). Size sourced from kernel_config.h
 * (CONFIG_PROCESS_HASH_SIZE) — must be a power of two for the mask hash. */
#define PROCESS_HASH_SIZE CONFIG_PROCESS_HASH_SIZE
_Static_assert((PROCESS_HASH_SIZE & (PROCESS_HASH_SIZE - 1)) == 0,
               "CONFIG_PROCESS_HASH_SIZE must be a power of two");
static process_t *process_hash_table[PROCESS_HASH_SIZE];

static inline uint32_t process_hash(uint32_t pid)
{
    return pid & (PROCESS_HASH_SIZE - 1);
}

static void process_hash_insert(process_t *proc)
{
    uint32_t idx = process_hash(proc->pid);
    proc->hash_next = process_hash_table[idx];
    process_hash_table[idx] = proc;
}

static void process_hash_remove(process_t *proc)
{
    uint32_t idx = process_hash(proc->pid);
    process_t **pp = &process_hash_table[idx];
    while (*pp)
    {
        if (*pp == proc)
        {
            *pp = proc->hash_next;
            proc->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static bool cleanup_queue_enqueue(process_t *proc);
static process_t *cleanup_queue_dequeue(void);
static void process_cleanup_immediate(process_t *proc);

void process_init(void)
{
    aslr_init();
    pid_allocator_init();

    process_list_head = NULL;
    process_count = 0;
    spinlock_init(&process_lock);

    g_cleanup_queue.head = NULL;
    g_cleanup_queue.tail = NULL;
    g_cleanup_queue.count = 0;
    spinlock_init(&g_cleanup_queue.lock);

    memset(process_hash_table, 0, sizeof(process_hash_table));

    debug_printf("[PROCESS] Process management initialized\n");
    debug_printf("[PROCESS] Max processes: %u\n", PROCESS_MAX_COUNT);
    debug_printf("[PROCESS] Cabin layout: 0x%lx (NULL trap), 0x%lx (CabinInfo), 0x%lx (PocketRing), 0x%lx (ResultRing), 0x%lx+ (Code)\n",
                 CABIN_NULL_TRAP_START,
                 CABIN_INFO_ADDR, CABIN_POCKET_RING_ADDR, CABIN_RESULT_RING_ADDR, CABIN_CODE_START_ADDR);
    debug_printf("[PROCESS] Hash table size: %u buckets\n", PROCESS_HASH_SIZE);
    debug_printf("[PROCESS] Deferred cleanup queue initialized (intrusive, unbounded)\n");
}

process_t *process_create(const char *tags)
{
    // use atomic load for the early check; the authoritative check happens
    // under process_lock when we actually insert into the list
    if (atomic_load_u32(&process_count) >= PROCESS_MAX_COUNT)
    {
        debug_printf("[PROCESS] ERROR: Process limit reached (%u)\n", PROCESS_MAX_COUNT);
        return NULL;
    }

    process_t *proc = kmalloc(sizeof(process_t));
    if (!proc)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate process structure\n");
        return NULL;
    }

    memset(proc, 0, sizeof(process_t));
    proc->magic = PROCESS_MAGIC;
    proc->rq_prio = -1;
    proc->rq_index = -1;

    uint64_t info_phys = 0;
    uint64_t pocket_phys = 0;
    uint64_t result_phys = 0;
    uint64_t touch_phys = 0;
    vmm_context_t *cabin = vmm_create_cabin(&info_phys, &pocket_phys, &result_phys,
                                            &touch_phys);
    if (!cabin)
    {
        debug_printf("[PROCESS] ERROR: Failed to create cabin\n");
        kfree(proc);
        return NULL;
    }

    proc->pid = pid_alloc();
    if (proc->pid == PID_INVALID)
    {
        debug_printf("[PROCESS] ERROR: PID allocation failed (exhaustion at %u/%u)\n",
                     pid_allocated_count(), PID_MAX_COUNT);
        vmm_destroy_context(cabin);
        kfree(proc);
        return NULL;
    }

    proc->cabin = cabin;
    proc->cabin_info_phys  = info_phys;
    proc->pocket_ring_phys = pocket_phys;
    proc->result_ring_phys = result_phys;
    proc->touch_ring_phys  = touch_phys;

    /* Phase 11: ring header initialization. The pages were zeroed in
     * vmm_create_cabin; we now write head=tail=0, slots_base, slot_size and
     * slot_count_max so userspace can locate slots without further help. */
    KRingPocketInit((PocketRing *)vmm_phys_to_virt(pocket_phys));
    KRingResultInit((ResultRing *)vmm_phys_to_virt(result_phys));
    KTouchRingInit((TouchRing *)vmm_phys_to_virt(touch_phys));

    // Tag IPC ring header pages as shared so PMT tracks the kernel↔userspace boundary.
    // The slot regions are mapped lazily via demand paging and are NOT tagged
    // shared — each slot page belongs to exactly one cabin once allocated.
    PhysTagSet(pocket_phys,
               pocket_phys + CABIN_POCKET_RING_PAGES * PMM_PAGE_SIZE,
               PHYS_TAG_SHARED);
    PhysTagSet(result_phys,
               result_phys + CABIN_RESULT_RING_PAGES * PMM_PAGE_SIZE,
               PHYS_TAG_SHARED);
    PhysTagSet(touch_phys,
               touch_phys + CABIN_TOUCH_RING_PAGES * PMM_PAGE_SIZE,
               PHYS_TAG_SHARED);
    proc->score = 0;
    proc->last_run_time = 0;
    proc->consecutive_runs = 0;
    proc->total_cpu_time = 0;
    proc->state = PROC_CREATED;
    spinlock_init(&proc->state_lock);
    atomic_store_u32(&proc->ref_count, 1);  // "alive" reference — released in process_destroy
    proc->destroying = 0; // Not being destroyed
    proc->code_start = VMM_CABIN_CODE_START;
    proc->code_size = 0;
    proc->started = false;
    proc->kcore_pending  = 0;
    proc->touch_cleaned  = 0;

    proc->subs_head             = NULL;
    spinlock_init(&proc->subs_lock);
    proc->irq_stack_top         = 0;
    proc->irq_rip               = 0;
    proc->irq_active            = 0;
    proc->irq_saved_rip         = 0;
    proc->irq_saved_rsp         = 0;
    proc->irq_saved_rflags      = 0;
    proc->irq_pending_head      = NULL;
    spinlock_init(&proc->irq_lock);

    // Assign home_core: round-robin across App Cores (multi-core) or BSP (single-core).
    if (g_amp.app_count > 0)
    {
        uint32_t rr = atomic_fetch_add_u32(&g_appcore_rr_counter, 1);
        uint32_t target = rr % g_amp.app_count;
        uint32_t app_seen = 0;
        for (uint8_t c = 0; c < g_amp.total_cores; c++)
        {
            if (!g_amp.cores[c].is_kcore)
            {
                if (app_seen == target)
                {
                    proc->home_core = c;
                    break;
                }
                app_seen++;
            }
        }
    }
    else
    {
        proc->home_core = g_amp.bsp_index;
    }

    // CRITICAL: Validate home_core is within bounds before using.
    // This prevents out-of-bounds access in sched_enqueue/sched_dequeue.
    if (proc->home_core >= g_amp.total_cores)
    {
        debug_printf("[PROCESS] ERROR: Invalid home_core %u assigned (max %u), using BSP\n",
                     proc->home_core, g_amp.total_cores);
        proc->home_core = g_amp.bsp_index;
    }

    // ASLR: generate per-process random offsets
    aslr_offsets_t aslr = aslr_generate();
    proc->aslr_stack_top = VMM_USER_STACK_TOP - aslr.stack_offset;
    proc->aslr_heap_base = CABIN_HEAP_BASE + aslr.heap_offset;
    proc->aslr_buf_heap_base = CABIN_BUF_HEAP_START + aslr.buf_heap_offset;
    proc->buf_heap_next = proc->aslr_buf_heap_base;
    proc->bay_claims_head = NULL;
    spinlock_init(&proc->bay_lock);
    proc->bay_va_next = CABIN_BAY_BASE;
    proc->brook_claims_head = NULL;
    spinlock_init(&proc->brook_lock);
    proc->brook_va_next = CABIN_BROOK_BASE;
    proc->next         = NULL;
    proc->prev         = NULL;
    proc->ready_next   = NULL;
    proc->cleanup_next = NULL;
    proc->in_ready     = 0;

    if (tags && tags[0] != '\0')
    {
        TagFSState *fs = tagfs_get_state();
        if (fs && fs->registry)
        {
            const char *pos = tags;
            while (*pos)
            {
                const char *comma = strchr(pos, ',');
                size_t len = comma ? (size_t)(comma - pos) : strlen(pos);
                if (len > 0 && len < 256)
                {
                    char tag_buf[256];
                    memcpy(tag_buf, pos, len);
                    tag_buf[len] = '\0';

                    char key[256], value[256];
                    tagfs_parse_tag(tag_buf, key, sizeof(key), value, sizeof(value));

                    uint16_t tid = tag_registry_intern(fs->registry, key,
                                                       value[0] ? value : NULL);
                    if (tid != TAGFS_INVALID_TAG_ID)
                    {
                        process_set_tag_bit(proc, tid);
                    }
                }
                if (!comma)
                    break;
                pos = comma + 1;
            }
        }
    }

    proc->wait_reason = WAIT_NONE;
    proc->wait_start_time = 0;
    proc->hash_next = NULL;

    // kernel stack layout: [guard page (unmapped)] [data pages]
    size_t kernel_stack_size = CONFIG_KERNEL_STACK_PAGES * VMM_PAGE_SIZE;
    size_t total_pages = CONFIG_KERNEL_STACK_TOTAL_PAGES;

    void *stack_phys = pmm_alloc(total_pages);
    if (!stack_phys)
    {
        vmm_destroy_context(cabin);
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    void *stack_virt_base = vmm_phys_to_virt((uintptr_t)stack_phys);

    vmm_context_t *kernel_ctx = vmm_get_kernel_context();

    // guard page must be unmapped so a stack overflow raises a page fault
    pte_t *guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)stack_virt_base);
    if (!guard_pte)
    {
        debug_printf("[PROCESS] FATAL: Cannot create guard page PTE for PID %u\n", proc->pid);
        pmm_free(stack_phys, total_pages);
        vmm_destroy_context(cabin);
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    *guard_pte = 0;
    /* Cross-core shootdown — vmm_demote_large_entry (the path that
     * produced this PTE) only invalidates the writing core's TLB.
     * Other cores still cache the original 1GB Pull-Map huge entry
     * for this VA range; without a broadcast flush, a stack overflow
     * on those cores walks the huge entry and lands on real RAM
     * instead of the guard, producing silent corruption. */
    vmm_shootdown_page(kernel_ctx, (uintptr_t)stack_virt_base);

    proc->kernel_stack_guard_base = stack_virt_base;
    proc->kernel_stack = (void *)((uintptr_t)stack_virt_base + VMM_PAGE_SIZE);
    proc->kernel_stack_top = (void *)((uintptr_t)proc->kernel_stack + kernel_stack_size);

    debug_printf("[PROCESS] Kernel stack allocated: guard=0x%lx, stack=0x%lx-0x%lx (PID %u)\n",
                 (uintptr_t)proc->kernel_stack_guard_base,
                 (uintptr_t)proc->kernel_stack,
                 (uintptr_t)proc->kernel_stack_top,
                 proc->pid);

    memset(&proc->context, 0, sizeof(ProcessContext));
    proc->context.cr3 = vmm_build_cr3(proc->cabin);
    proc->context.rflags = 0x202;
    proc->context.cs = GDT_USER_CODE;
    proc->context.ds = GDT_USER_DATA;
    proc->context.es = GDT_USER_DATA;
    proc->context.fs = GDT_USER_DATA;
    proc->context.gs = GDT_USER_DATA;
    proc->context.ss = GDT_USER_DATA;

    // Allocate FPU/SSE/AVX state buffer dynamically (size depends on CPU features)
    uint32_t fpu_buf_size = fpu_alloc_size();
    proc->context.fpu_state = kmalloc(fpu_buf_size);
    if (!proc->context.fpu_state)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate FPU state buffer (%u bytes)\n", fpu_buf_size);
        if (proc->kernel_stack_guard_base)
        {
            uintptr_t sp = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);
            pmm_free((void *)sp, CONFIG_KERNEL_STACK_TOTAL_PAGES);
        }
        vmm_destroy_context(cabin);
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }
    fpu_init_state(proc->context.fpu_state);
    proc->context.fpu_initialized = true;

    spin_lock(&process_lock);

    // authoritative check under lock to prevent race between early check and insert
    if (process_count >= PROCESS_MAX_COUNT)
    {
        spin_unlock(&process_lock);
        // undo all allocations
        if (proc->context.fpu_state)
            kfree(proc->context.fpu_state);
        if (proc->kernel_stack_guard_base)
        {
            uintptr_t stack_phys = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);
            pmm_free((void *)stack_phys, CONFIG_KERNEL_STACK_TOTAL_PAGES);
        }
        vmm_destroy_context(cabin);
        pid_free(proc->pid);
        kfree(proc);
        debug_printf("[PROCESS] ERROR: Process limit reached under lock (%u)\n", PROCESS_MAX_COUNT);
        return NULL;
    }

    /* Doubly-linked insert at head: O(1). Pair with O(1) unlink in
     * process_destroy. */
    proc->prev = NULL;
    proc->next = process_list_head;
    if (process_list_head) process_list_head->prev = proc;
    process_list_head = proc;
    process_hash_insert(proc);
    process_count++;
    spin_unlock(&process_lock);

    return proc;
}

void process_ref_inc(process_t *proc)
{
    if (!proc)
    {
        return;
    }

    uint32_t old = atomic_fetch_add_u32(&proc->ref_count, 1);

#ifdef DEBUG_REFCOUNT
    debug_printf("[REFCOUNT] PID %u: %u -> %u (INC)\n", proc->pid, old, old + 1);
#endif

    if (old > UINT32_MAX - 100)
    {
        debug_printf("[PROCESS] PANIC: ref_count overflow for PID %u (old=%u)\n",
                     proc->pid, old);
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }
}

void process_ref_dec(process_t *proc)
{
    if (!proc)
        return;

    // fetch_sub returns OLD value (before subtraction)
    uint32_t old = atomic_fetch_sub_u32(&proc->ref_count, 1);

    if (old == 0)
    {
        // Underflow: ref_count was 0, subtracted 1 → wrapped to UINT32_MAX.
        // Restore via CAS so a concurrent ref_inc on another core doesn't get
        // clobbered. If someone else has already moved the counter, leave it
        // alone — they hold a fresh ref and will balance it themselves.
        uint32_t expected = UINT32_MAX;
        __atomic_compare_exchange_n(&proc->ref_count, &expected, 1u,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        kprintf("[PROCESS] BUG: ref_count underflow for PID %u (restored)\n", proc->pid);
        return;
    }

#ifdef DEBUG_REFCOUNT
    debug_printf("[REFCOUNT] PID %u: %u -> %u (DEC)\n", proc->pid, old, old - 1);
#endif

    // old == 1 means we just decremented to 0 — last reference released.
    if (old == 1)
    {
        process_state_t state = process_get_state(proc);
        if (state == PROC_DONE || state == PROC_CRASHED)
        {
            // Process already unlinked by process_destroy (magic == 0xDEADDEAD).
            // Safe to enqueue for final resource cleanup.
            if (proc->magic == CONFIG_PROCESS_POISON_MAGIC)
            {
                if (!cleanup_queue_enqueue(proc))
                {
                    process_cleanup_immediate(proc);
                }
            }
        }
        else
        {
            kprintf("[PROCESS] WARNING: ref_count=0 for live PID %u (state=%d)\n",
                    proc->pid, (int)state);
        }
    }
}

uint32_t process_ref_count(process_t *proc)
{
    if (!proc)
        return 0;
    return atomic_load_u32(&proc->ref_count);
}

void process_set_state(process_t *proc, process_state_t new_state)
{
    if (!proc)
        return;

    spin_lock(&proc->state_lock);
    process_state_t old_state = proc->state;
    proc->state = new_state;

    if (new_state == PROC_WORKING && old_state != PROC_WORKING)
    {
        error_t err = sched_enqueue(proc);
        if (err != OK)
        {
            debug_printf("[PROCESS] sched_enqueue failed for PID %u: %s\n",
                         proc->pid, ErrorString(err));
        }
    }
    else if (new_state != PROC_WORKING && old_state == PROC_WORKING)
    {
        sched_dequeue(proc);
    }

    spin_unlock(&proc->state_lock);
}

process_state_t process_get_state(process_t *proc)
{
    if (!proc)
        return PROC_CRASHED;
    spin_lock(&proc->state_lock);
    process_state_t state = proc->state;
    spin_unlock(&proc->state_lock);
    return state;
}

int process_destroy_safe(process_t *proc)
{
    if (!proc)
    {
        return -1;
    }

    process_state_t state = process_get_state(proc);
    if (state != PROC_CRASHED && state != PROC_DONE)
    {
        debug_printf("[PROCESS] ERROR: Cannot destroy PID %u in state %d\n",
                     proc->pid, state);
        return -1;
    }

    process_destroy(proc);
    return 0;
}

void process_destroy(process_t *proc)
{
    if (!proc)
        return;

    if (proc->magic != PROCESS_MAGIC)
    {
        debug_printf("[PROCESS] CORRUPTION: Invalid magic 0x%x for PID %u\n",
                     proc->magic, proc->pid);
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    // Bounds-check home_core before indexing g_core_sched[]
    if (proc->home_core >= g_amp.total_cores)
    {
        debug_printf("[PROCESS] ERROR: PID %u has invalid home_core %u (max %u)\n",
                     proc->pid, proc->home_core, g_amp.total_cores);
        return;
    }

    // CRITICAL: Set destroying flag FIRST — before any checks or locks.
    // This prevents scheduler_select_next() from selecting this process
    // during the window between is_running check and unlinking.
    // Use atomic store with SEQ_CST for visibility across all cores.
    __atomic_store_n(&proc->destroying, 1, __ATOMIC_SEQ_CST);
    mfence(); // Ensure visibility before proceeding

    // Check if process is currently executing on ANY core.
    // Use atomic load on the pointer (naturally aligned, x86-64 word-tearing safe)
    // so we avoid taking all scheduler_locks in order — that caused a lock-inversion
    // risk when process_destroy_safe callers hold process_lock first.
    // A full mfence before the scan pairs with the RELEASE store in schedule().
    mfence();
    bool is_running = false;
    uint8_t running_on_core = 0;

    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        scheduler_state_t *sched = scheduler_get_core(c);
        if (!sched) continue;
        process_t *cur = __atomic_load_n(&sched->current_process, __ATOMIC_ACQUIRE);
        if (cur == proc)
        {
            is_running = true;
            running_on_core = c;
            break;
        }
    }

    if (is_running)
    {
        __atomic_store_n(&proc->destroying, 0, __ATOMIC_SEQ_CST);
        debug_printf("[PROCESS] ERROR: Cannot destroy running process PID %u (on core %u)\n",
                     proc->pid, running_on_core);
        return;
    }

    spin_lock(&process_lock);

    // Always unlink from list and hash — process is unreachable after this.
    process_hash_remove(proc);
    /* Magic poisoning was previously done HERE, but `process_set_state` below
     * calls `sched_dequeue(proc)` which can read `proc->magic` and `proc->cabin`
     * on remote cores still holding a transient reference. Poison AFTER all
     * state transitions complete (just before the final ref_dec at the bottom
     * of this function). Audit 2026-04-29 confirmed the AMP race window. */

    /* O(1) unlink — doubly-linked list. Both ends fixed up; head adjusted
     * if proc was first. The previous O(N) walk-from-head used to dominate
     * process_destroy under high churn (every shell-spawned utility). */
    if (proc->prev) proc->prev->next = proc->next;
    else            process_list_head = proc->next;
    if (proc->next) proc->next->prev = proc->prev;
    proc->prev = NULL;
    proc->next = NULL;

    process_count--;

    spin_unlock(&process_lock);

    process_set_state(proc, PROC_CRASHED);

    uint32_t cancelled = async_io_cancel_by_pid(proc->pid);
    if (cancelled > 0)
    {
        debug_printf("[PROCESS] Cancelled %u pending async I/O for PID %u\n",
                     cancelled, proc->pid);
    }

    // Clear PHYS_TAG_SHARED on IPC ring pages — these are no longer shared
    // once the process is destroyed and its cabin will be unmapped.
    if (proc->pocket_ring_phys) {
        PhysTagClear(proc->pocket_ring_phys,
                     proc->pocket_ring_phys + CABIN_POCKET_RING_PAGES * PMM_PAGE_SIZE,
                     PHYS_TAG_SHARED);
    }
    if (proc->result_ring_phys) {
        PhysTagClear(proc->result_ring_phys,
                     proc->result_ring_phys + CABIN_RESULT_RING_PAGES * PMM_PAGE_SIZE,
                     PHYS_TAG_SHARED);
    }
    if (proc->touch_ring_phys) {
        PhysTagClear(proc->touch_ring_phys,
                     proc->touch_ring_phys + CABIN_TOUCH_RING_PAGES * PMM_PAGE_SIZE,
                     PHYS_TAG_SHARED);
    }

    TouchCleanupProcess(proc);

    /* Drop every Bay claim this cabin holds BEFORE the VMM teardown.
     * BayCleanupProcess walks proc->bay_claims_head, unmaps each claim
     * from the cabin's page tables, decrements the BayObject ref_count
     * (last drop returns chunks to PMM), and kfree's the claim. After
     * this returns there are no Bay PDEs left in the cabin; the
     * vmm_destroy_context LARGE_PAGE walker becomes a no-op for Bay
     * regions and only sees user-heap implicit-huge pages. */
    BayCleanupProcess(proc);

    /* Drop every Brook claim this cabin holds. Same lifecycle pattern
     * as Bay — peer-cabin receives -ERR_BROKEN_PIPE / END_OF_STREAM on
     * its next push/pop. Must run BEFORE vmm_destroy_context so the
     * per-claim VA windows are torn down through the proper unmap path
     * (BrookObject ref drops to zero only when both peers release). */
    BrookCleanupProcess(proc);

    /* Final state set is COMPLETE — now safe to poison the magic. Any
     * sibling-core dereference past this point is a real bug we want to
     * see, not a transient destroy-window race. */
    proc->magic = CONFIG_PROCESS_POISON_MAGIC;

    // Release the "alive" reference.  If K-Core still holds refs,
    // cleanup is deferred until the last ref_dec triggers it.
    process_ref_dec(proc);
}

int process_load_binary(process_t *proc, const void *binary_data, size_t size)
{
    if (!proc || !binary_data || size == 0)
    {
        debug_printf("[PROCESS] ERROR: Invalid arguments to process_load_binary\n");
        return -1;
    }

    if (!proc->cabin)
    {
        debug_printf("[PROCESS] ERROR: Process has no cabin\n");
        return -1;
    }

    size_t page_count = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;

    // minimum 5 pages for cabin layout (.text: 3, .rodata: 1, .data: 1)
    if (page_count < 5)
    {
        page_count = 5;
    }

    void *code_phys = pmm_alloc_zero(page_count);
    if (!code_phys)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate %zu pages for binary\n", page_count);
        return -1;
    }

    void *code_virt = vmm_phys_to_virt((uintptr_t)code_phys);
    memcpy(code_virt, binary_data, size);

    uintptr_t entry_point = VMM_CABIN_CODE_START;
    int result = vmm_map_code_region(proc->cabin, (uintptr_t)code_phys,
                                     page_count * VMM_PAGE_SIZE, &entry_point);
    if (result != 0)
    {
        debug_printf("[PROCESS] ERROR: Failed to map code region\n");
        debug_printf("[PROCESS] Physical address: 0x%lx, Size: %zu bytes\n",
                     (uintptr_t)code_phys, size);
        pmm_free(code_phys, page_count);
        return -1;
    }

    /* For ELF binaries vmm_map_code_region copies each segment into freshly
     * allocated pages and maps THOSE; the buffer we just passed in (`code_phys`)
     * served only as the parser's scratch. If we keep it, every spawned ELF
     * leaks `page_count` frames — 100 spawns ≈ 5 MiB leaked, eventually
     * exhausting PMM and producing page-faults at RIP=0xc000 on freshly
     * spawned processes (because vmm_map_pages ends up with NULL frames).
     *
     * For flat binaries the same buffer is mapped directly into the cabin
     * and must stay live; detect via ELF magic. */
    const uint8_t *probe = (const uint8_t *)code_virt;
    bool was_elf = (size >= 4 && probe[0] == 0x7F && probe[1] == 'E' &&
                    probe[2] == 'L' && probe[3] == 'F');
    if (was_elf) {
        pmm_free(code_phys, page_count);
    }

    void *user_stack_phys = pmm_alloc(CONFIG_USER_STACK_TOTAL_PAGES);
    if (!user_stack_phys)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate user stack\n");
        pmm_free(code_phys, page_count);
        return -1;
    }

    // ASLR: use randomized stack top
    uint64_t stack_top = proc->aslr_stack_top;

    // stack layout: [guard page][...data pages...]
    // only map data pages; guard page is left unmapped for overflow detection
    uint64_t guard_page_base = stack_top - (CONFIG_USER_STACK_TOTAL_PAGES * VMM_PAGE_SIZE);
    uint64_t stack_data_base = guard_page_base + (CONFIG_USER_STACK_GUARD_PAGES * VMM_PAGE_SIZE);

    vmm_map_result_t map_result = vmm_map_pages(
        proc->cabin,
        stack_data_base,
        (uintptr_t)user_stack_phys + (CONFIG_USER_STACK_GUARD_PAGES * VMM_PAGE_SIZE),
        CONFIG_USER_STACK_PAGES,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);

    if (!map_result.success)
    {
        debug_printf("[PROCESS] ERROR: Failed to map user stack: %s\n", map_result.error_msg);
        pmm_free(user_stack_phys, CONFIG_USER_STACK_TOTAL_PAGES);
        pmm_free(code_phys, page_count);
        return -1;
    }

    // === Heap initialization (ASLR: use randomized heap base) ===
    uintptr_t heap_start = proc->aslr_heap_base;
    void *heap_phys = pmm_alloc_zero(CONFIG_USER_HEAP_INITIAL_PAGES);
    if (!heap_phys)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate initial heap pages\n");
        pmm_free(user_stack_phys, CONFIG_USER_STACK_TOTAL_PAGES);
        pmm_free(code_phys, page_count);
        return -1;
    }

    vmm_map_result_t heap_map = vmm_map_pages(
        proc->cabin,
        heap_start,
        (uintptr_t)heap_phys,
        CONFIG_USER_HEAP_INITIAL_PAGES,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE);

    if (!heap_map.success)
    {
        debug_printf("[PROCESS] ERROR: Failed to map user heap: %s\n", heap_map.error_msg);
        pmm_free(heap_phys, CONFIG_USER_HEAP_INITIAL_PAGES);
        pmm_free(user_stack_phys, CONFIG_USER_STACK_TOTAL_PAGES);
        pmm_free(code_phys, page_count);
        return -1;
    }

    proc->cabin->heap_start = heap_start;
    proc->cabin->heap_end = heap_start + CONFIG_USER_HEAP_INITIAL_SIZE;
    proc->cabin->stack_top = stack_top;

    // Write ASLR cabin info to CabinInfo page so userspace knows its layout
    CabinInfo *ci = (CabinInfo *)vmm_phys_to_virt(proc->cabin_info_phys);
    ci->magic = CABIN_INFO_MAGIC;
    ci->pid = proc->pid;
    ci->spawner_pid = proc->spawner_pid;
    ci->reserved = 0;
    ci->heap_base = heap_start;
    ci->heap_max_size = CABIN_HEAP_MAX_SIZE;
    ci->buf_heap_base = proc->aslr_buf_heap_base;
    ci->stack_top = stack_top;

    proc->code_size = size;
    /* Use the entry point reported by vmm_map_code_region — equals
     * ehdr->e_entry for ELF binaries (which is normally the same as
     * VMM_CABIN_CODE_START because production binaries link with
     * .text=0xC000, but honouring it survives a future linker bump). */
    proc->context.rip = entry_point;
    proc->context.rsp = stack_top;

    debug_printf("[PROCESS] ASLR: PID %u heap=0x%lx stack=0x%lx buf=0x%lx\n",
                 proc->pid, heap_start, stack_top, proc->aslr_buf_heap_base);

    return 0;
}

void process_list_validate(const char *caller)
{
    // allocate BEFORE taking the lock to avoid kmalloc under spinlock
    process_t **seen = (process_t **)kmalloc(PROCESS_MAX_COUNT * sizeof(process_t *));
    if (!seen)
    {
        debug_printf("[PROCESS] WARNING: kmalloc failed in process_list_validate\n");
        return;
    }

    spin_lock(&process_lock);

    uint32_t count = 0;
    process_t *curr = process_list_head;

    while (curr && count < PROCESS_MAX_COUNT)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            if (seen[i] == curr)
            {
                debug_printf("[PROCESS] CORRUPTION: Cycle detected at %p (caller: %s)\n",
                             (void *)curr, caller);
                debug_printf("[PROCESS] Process list is corrupted\n");
                kfree(seen);
                while (1)
                {
                    asm volatile("cli; hlt");
                }
            }
        }

        if (curr->magic != PROCESS_MAGIC)
        {
            debug_printf("[PROCESS] CORRUPTION: Invalid magic 0x%x at %p (caller: %s)\n",
                         curr->magic, (void *)curr, caller);
            debug_printf("[PROCESS] PID: %u\n", curr->pid);
            kfree(seen);
            while (1)
            {
                asm volatile("cli; hlt");
            }
        }

        seen[count++] = curr;
        curr = curr->next;
    }

    if (count != process_count)
    {
        debug_printf("[PROCESS] CORRUPTION: Count mismatch (list=%u, expected=%u, caller=%s)\n",
                     count, process_count, caller);
        kfree(seen);
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    kfree(seen);
    spin_unlock(&process_lock);
}

uint32_t process_snapshot_pids(uint32_t *out, uint32_t max)
{
    if (!out || max == 0) return 0;
    uint32_t n = 0;
    spin_lock(&process_lock);
    for (uint32_t b = 0; b < PROCESS_HASH_SIZE && n < max; b++) {
        for (process_t *p = process_hash_table[b]; p && n < max; p = p->hash_next) {
            if (p->magic == PROCESS_MAGIC && !p->destroying) {
                out[n++] = p->pid;
            }
        }
    }
    spin_unlock(&process_lock);
    return n;
}

process_t *process_find(uint32_t pid)
{
    if (pid == PROCESS_INVALID_PID)
    {
        return NULL;
    }

    spin_lock(&process_lock);

    uint32_t idx = process_hash(pid);
    process_t *curr = process_hash_table[idx];
    while (curr)
    {
        if (curr->pid == pid && curr->magic == PROCESS_MAGIC)
        {
            spin_unlock(&process_lock);
            return curr;
        }
        curr = curr->hash_next;
    }

    spin_unlock(&process_lock);
    return NULL;
}

process_t *process_find_ref(uint32_t pid)
{
    if (pid == PROCESS_INVALID_PID)
    {
        return NULL;
    }

    spin_lock(&process_lock);

    uint32_t idx = process_hash(pid);
    process_t *curr = process_hash_table[idx];
    while (curr)
    {
        if (curr->pid == pid && curr->magic == PROCESS_MAGIC)
        {
            process_ref_inc(curr);
            spin_unlock(&process_lock);
            return curr;
        }
        curr = curr->hash_next;
    }

    spin_unlock(&process_lock);
    return NULL;
}

process_t *process_get_first(void)
{
    // Returns head without acquiring lock.
    // Caller should hold process_list_lock() for safe iteration via proc->next.
    // In IRQ context with cli, process_lock is guaranteed free (spinlock does
    // cli before acquire, so holder had IRQs disabled and IRQ couldn't fire).
    return process_list_head;
}

void process_list_lock(void)
{
    spin_lock(&process_lock);
}

void process_list_unlock(void)
{
    spin_unlock(&process_lock);
}

bool spin_trylock_process_list(void)
{
    return spin_trylock(&process_lock);
}

process_t *process_get_current(void)
{
    scheduler_state_t *sched = scheduler_get_state();
    if (!sched)
        return NULL;

    spin_lock(&sched->scheduler_lock);
    process_t *current = sched->current_process;
    spin_unlock(&sched->scheduler_lock);

    return current;
}

uint32_t process_get_count(void)
{
    return atomic_load_u32(&process_count);
}

#ifdef CONFIG_KERNEL_TESTS
void process_test(void)
{
    kprintf("\n");
    kprintf("====================================\n");
    kprintf("PROCESS MANAGEMENT TEST\n");
    kprintf("====================================\n");

    debug_printf("[TEST] Creating test process...\n");
    process_t *proc = process_create("app test");

    if (!proc)
    {
        debug_printf("[TEST] FAILED: Could not create process\n");
        return;
    }

    debug_printf("[TEST] SUCCESS: Process created\n");
    debug_printf("[TEST]   PID: %u\n", proc->pid);
    debug_printf("[TEST]   State: %s\n", proc->state == PROC_CREATED ? "CREATED" : "UNKNOWN");
    debug_printf("[TEST]   Cabin: %p\n", proc->cabin);
    debug_printf("[TEST]   CabinInfo (phys): 0x%lx\n", proc->cabin_info_phys);
    debug_printf("[TEST]   PocketRing (phys): 0x%lx\n", proc->pocket_ring_phys);
    debug_printf("[TEST]   ResultRing (phys): 0x%lx\n", proc->result_ring_phys);
    debug_printf("[TEST]   Code start (virt): 0x%lx\n", proc->code_start);
    debug_printf("[TEST]   TagBits: 0x%lx\n", proc->tag_bits);

    uint8_t test_binary[64];
    for (int i = 0; i < 64; i++)
    {
        test_binary[i] = (uint8_t)i;
    }

    debug_printf("[TEST] Loading test binary (64 bytes)...\n");
    int result = process_load_binary(proc, test_binary, sizeof(test_binary));

    if (result != 0)
    {
        debug_printf("[TEST] FAILED: Could not load binary\n");
        process_destroy(proc);
        return;
    }

    debug_printf("[TEST] SUCCESS: Binary loaded\n");
    debug_printf("[TEST]   State: %s\n", proc->state == PROC_WORKING ? "WORKING" : "UNKNOWN");
    debug_printf("[TEST]   Code size: %zu bytes\n", proc->code_size);

    debug_printf("[TEST] Verifying process lookup...\n");
    process_t *found = process_find(proc->pid);
    if (found == proc)
    {
        debug_printf("[TEST] SUCCESS: Process lookup working\n");
    }
    else
    {
        debug_printf("[TEST] FAILED: Process lookup failed\n");
    }

    debug_printf("[TEST] Process count: %u\n", process_get_count());

    debug_printf("[TEST] Destroying process...\n");
    process_destroy(proc);

    debug_printf("[TEST] SUCCESS: Process destroyed\n");
    debug_printf("[TEST] Process count: %u\n", process_get_count());

    kprintf("====================================\n");
    kprintf("PROCESS TEST COMPLETE\n");
    kprintf("====================================\n");
    kprintf("\n");
}
#endif // CONFIG_KERNEL_TESTS

bool process_has_tag_id(process_t *proc, uint16_t tag_id)
{
    if (!proc)
        return false;
    if (tag_id < 64)
        return (proc->tag_bits & ((uint64_t)1 << tag_id)) != 0;
    for (uint16_t i = 0; i < proc->tag_overflow_count; i++)
    {
        if (proc->tag_overflow_ids[i] == tag_id)
            return true;
    }
    return false;
}

bool process_has_tag(process_t *proc, const char *tag)
{
    if (!proc || !tag || tag[0] == '\0')
        return false;

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
        return false;
    TagRegistry *reg = fs->registry;

    if (!tag_is_wildcard(tag))
    {
        char key[256], value[256];
        tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));
        uint16_t tid = tag_registry_lookup(reg, key, value[0] ? value : NULL);
        if (tid == TAGFS_INVALID_TAG_ID)
            return false;
        return process_has_tag_id(proc, tid);
    }

    // Wildcard: iterate all tags this process has
    uint64_t bits = proc->tag_bits;
    while (bits)
    {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        const char *k = tag_registry_key(reg, i);
        const char *v = tag_registry_value(reg, i);
        if (!k)
            continue;
        char full[512];
        tagfs_format_tag(full, sizeof(full), k, v);
        if (tag_match(tag, full))
            return true;
    }
    for (uint16_t j = 0; j < proc->tag_overflow_count; j++)
    {
        uint16_t tid = proc->tag_overflow_ids[j];
        const char *k = tag_registry_key(reg, tid);
        const char *v = tag_registry_value(reg, tid);
        if (!k)
            continue;
        char full[512];
        tagfs_format_tag(full, sizeof(full), k, v);
        if (tag_match(tag, full))
            return true;
    }
    return false;
}

int process_add_tag(process_t *proc, const char *tag)
{
    if (!proc || !tag || tag[0] == '\0')
        return -1;

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
        return -1;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    spin_lock(&process_lock);

    uint16_t tid = tag_registry_intern(fs->registry, key, value[0] ? value : NULL);
    if (tid == TAGFS_INVALID_TAG_ID)
    {
        spin_unlock(&process_lock);
        return -1;
    }

    int ret = process_set_tag_bit(proc, tid);
    spin_unlock(&process_lock);
    return ret;
}

int process_remove_tag(process_t *proc, const char *tag)
{
    if (!proc || !tag || tag[0] == '\0')
        return -1;

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
        return -1;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    uint16_t tid = tag_registry_lookup(fs->registry, key, value[0] ? value : NULL);
    if (tid == TAGFS_INVALID_TAG_ID)
        return 0;

    spin_lock(&process_lock);
    int ret = process_clear_tag_bit(proc, tid);
    spin_unlock(&process_lock);
    return ret;
}

void process_start_initial(process_t *proc)
{
    if (!proc)
    {
        debug_printf("[PROCESS] PANIC: process_start_initial called with NULL process\n");
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    if (proc->state != PROC_WORKING)
    {
        debug_printf("[PROCESS] PANIC: process_start_initial called with process not in WORKING state\n");
        debug_printf("[PROCESS]   PID %u is in state %d (expected PROC_WORKING=%d)\n",
                     proc->pid, proc->state, PROC_WORKING);
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    if (proc->started)
    {
        debug_printf("[PROCESS] PANIC: process_start_initial called on process that already ran\n");
        debug_printf("[PROCESS]   PID %u has started=true\n", proc->pid);
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    process_set_state(proc, PROC_WORKING);

    scheduler_state_t *sched = scheduler_get_state();
    spin_lock(&sched->scheduler_lock);
    sched->current_process = proc;
    spin_unlock(&sched->scheduler_lock);

    asm volatile("cli");

    per_core_set_kernel_rsp((uint64_t)proc->kernel_stack_top);

    uint64_t target_cr3 = proc->context.cr3;
    if (vmm_pcid_active())
        target_cr3 |= (1ULL << 63); // NOFLUSH
    __asm__ volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");

    uint64_t verify_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(verify_cr3));
    if (verify_cr3 != proc->context.cr3)
    {
        debug_printf("[PROCESS] PANIC: CR3 verification failed (wrote 0x%lx, read 0x%lx)\n",
                     proc->context.cr3, verify_cr3);
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    jump_to_userspace(proc->context.rip, proc->context.rsp, proc->context.rflags);

    debug_printf("[PROCESS] PANIC: jump_to_userspace returned (should never happen)\n");
    while (1)
    {
        asm volatile("cli; hlt");
    }
}

size_t process_snapshot_tags(process_t *proc, char *buffer, size_t buffer_size)
{
    if (!proc || !buffer || buffer_size == 0)
        return 0;

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry)
    {
        buffer[0] = '\0';
        return 0;
    }
    TagRegistry *reg = fs->registry;

    spin_lock(&process_lock);

    size_t pos = 0;
    bool first = true;

    uint64_t bits = proc->tag_bits;
    while (bits)
    {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        const char *k = tag_registry_key(reg, i);
        const char *v = tag_registry_value(reg, i);
        if (!k)
            continue;

        if (!first && pos < buffer_size - 1)
            buffer[pos++] = ',';
        first = false;

        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;

        if (vlen > 0)
        {
            if (pos + klen + 1 + vlen >= buffer_size)
                break;
            memcpy(buffer + pos, k, klen);
            pos += klen;
            buffer[pos++] = ':';
            memcpy(buffer + pos, v, vlen);
            pos += vlen;
        }
        else
        {
            if (pos + klen >= buffer_size)
                break;
            memcpy(buffer + pos, k, klen);
            pos += klen;
        }
    }

    for (uint16_t j = 0; j < proc->tag_overflow_count; j++)
    {
        uint16_t tid = proc->tag_overflow_ids[j];
        const char *k = tag_registry_key(reg, tid);
        const char *v = tag_registry_value(reg, tid);
        if (!k)
            continue;

        if (!first && pos < buffer_size - 1)
            buffer[pos++] = ',';
        first = false;

        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;

        if (vlen > 0)
        {
            if (pos + klen + 1 + vlen >= buffer_size)
                break;
            memcpy(buffer + pos, k, klen);
            pos += klen;
            buffer[pos++] = ':';
            memcpy(buffer + pos, v, vlen);
            pos += vlen;
        }
        else
        {
            if (pos + klen >= buffer_size)
                break;
            memcpy(buffer + pos, k, klen);
            pos += klen;
        }
    }

    buffer[pos] = '\0';
    spin_unlock(&process_lock);
    return pos;
}

static bool cleanup_queue_enqueue(process_t *proc)
{
    if (!proc)
        return false;

    proc->cleanup_next = NULL;

    spin_lock(&g_cleanup_queue.lock);
    if (g_cleanup_queue.tail) {
        g_cleanup_queue.tail->cleanup_next = proc;
    } else {
        g_cleanup_queue.head = proc;
    }
    g_cleanup_queue.tail = proc;
    g_cleanup_queue.count++;
    spin_unlock(&g_cleanup_queue.lock);
    return true;
}

static process_t *cleanup_queue_dequeue(void)
{
    spin_lock(&g_cleanup_queue.lock);
    process_t *proc = g_cleanup_queue.head;
    if (!proc) {
        spin_unlock(&g_cleanup_queue.lock);
        return NULL;
    }
    g_cleanup_queue.head = proc->cleanup_next;
    if (!g_cleanup_queue.head) g_cleanup_queue.tail = NULL;
    g_cleanup_queue.count--;
    spin_unlock(&g_cleanup_queue.lock);
    proc->cleanup_next = NULL;
    return proc;
}

static void process_cleanup_immediate(process_t *proc)
{
    if (!proc)
        return;

    if (proc->tag_overflow_ids)
    {
        kfree(proc->tag_overflow_ids);
        proc->tag_overflow_ids = NULL;
        proc->tag_overflow_count = 0;
        proc->tag_overflow_capacity = 0;
    }

    /* Touch sub list — deferred free.
     *
     * TouchCleanupProcess (called from process_destroy) unlinks every
     * sub from its TouchBucket so concurrent publishes stop seeing this
     * process, but leaves the sub structs allocated because publishers
     * on other cores can still be holding pointers into them. We now
     * hold the "last reference" (ref_count just hit 0) so no publisher
     * can be active — safe to free the per-process chain. */
    TouchFinalizeProcess(proc);

    // Free dynamically allocated FPU state buffer
    if (proc->context.fpu_state)
    {
        kfree(proc->context.fpu_state);
        proc->context.fpu_state = NULL;
    }

    if (proc->kernel_stack_guard_base)
    {
        uintptr_t guard_virt = (uintptr_t)proc->kernel_stack_guard_base;
        uintptr_t stack_phys = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);

        // Restore guard page PTE before freeing (use physical, not virtual address)
        vmm_context_t *kernel_ctx = vmm_get_kernel_context();
        pte_t *guard_pte = vmm_get_pte(kernel_ctx, guard_virt);
        if (guard_pte)
        {
            *guard_pte = vmm_make_pte(stack_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
            /* Cross-core shootdown — kernel_ctx is shared by every CPU.
             * Intel SDM Vol 3A §4.10.4: local invlpg leaves stale "guard
             * hole" translations on remote cores; the next kernel walk
             * on them returns NULL via an entry this core has already
             * re-populated. */
            vmm_shootdown_page(kernel_ctx, guard_virt);
        }

        pmm_free((void *)stack_phys, CONFIG_KERNEL_STACK_TOTAL_PAGES);

        debug_printf("[PROCESS] Freed kernel stack: guard=0x%lx (PID %u)\n",
                     guard_virt, proc->pid);

        proc->kernel_stack_guard_base = NULL;
        proc->kernel_stack = NULL;
    }

    if (proc->cabin)
    {
        vmm_destroy_context(proc->cabin);
        proc->cabin = NULL;
    }

    pid_free(proc->pid);
    proc->pid = PID_INVALID;

    kfree(proc);
}

void process_cleanup_deferred(void)
{
    uint32_t cleaned = 0;

    while (cleaned < CONFIG_PROCESS_CLEANUP_BATCH)
    {
        process_t *proc = cleanup_queue_dequeue();
        if (!proc)
            break;

        process_cleanup_immediate(proc);
        cleaned++;
    }

    if (cleaned > 0)
    {
        debug_printf("[PROCESS] Deferred cleanup: freed %u processes\n", cleaned);
    }
}

uint32_t process_cleanup_queue_size(void)
{
    spin_lock(&g_cleanup_queue.lock);
    uint32_t size = g_cleanup_queue.count;
    spin_unlock(&g_cleanup_queue.lock);
    return size;
}

void process_cleanup_queue_flush(void)
{
    debug_printf("[PROCESS] Flushing cleanup queue (pending=%u)...\n",
                 process_cleanup_queue_size());

    uint32_t total_cleaned = 0;

    // Use cleanup_queue_dequeue() as the authoritative emptiness check
    // (it acquires the lock internally). Avoid reading g_cleanup_queue.count
    // without the lock — another core could be enqueuing concurrently.
    for (;;)
    {
        process_t *proc = cleanup_queue_dequeue();
        if (!proc)
            break;

        process_cleanup_immediate(proc);
        total_cleaned++;
    }

    debug_printf("[PROCESS] Cleanup queue flushed (%u processes freed)\n",
                 total_cleaned);
}
