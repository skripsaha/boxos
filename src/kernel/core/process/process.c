#include "process.h"
#include "cabin.h"
#include "auth_tags.h"
#include "klib.h"
#include "kernel_config.h"
#include "pmm.h"
#include "memtag.h"
#include "manifest.h"
#include "cabin_layout.h"
#include "kring.h"
#include "touch_ring.h"
#include "vmm.h"
#include "nameplate.h"
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
#include "proc_exit.h"
#include "bay.h"
#include "brook.h"
#include "tagfs.h"
#include "use_context.h"
#include "per_core.h"
#include "amp.h"
#include "cet_lifecycle.h"
#include "strand_rings.h"
#include "strand_pool_abi.h"

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

static volatile uint32_t g_appcore_rr_counter = 0;

static int process_set_tag_bit(process_t *proc, uint16_t tag_id)
{
    cabin_t *cabin = proc->cabin;

    if (tag_id < 64)
    {
        __atomic_or_fetch(&cabin->tag_bits, ((uint64_t)1 << tag_id), __ATOMIC_RELAXED);
        return 0;
    }

    for (uint16_t i = 0; i < cabin->tag_overflow_count; i++)
    {
        if (cabin->tag_overflow_ids[i] == tag_id)
            return 0;
    }

    if (cabin->tag_overflow_count >= cabin->tag_overflow_capacity)
    {
        uint16_t new_cap = cabin->tag_overflow_capacity == 0 ? 8 : cabin->tag_overflow_capacity * 2;
        uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids)
            return -1;

        uint16_t *old_ids = cabin->tag_overflow_ids;
        if (old_ids)
            memcpy(new_ids, old_ids, sizeof(uint16_t) * cabin->tag_overflow_count);

        __atomic_store_n(&cabin->tag_overflow_ids, new_ids, __ATOMIC_RELEASE);
        __atomic_store_n(&cabin->tag_overflow_capacity, new_cap, __ATOMIC_RELEASE);

        mfence();

        if (old_ids)
        {
            TagOverflowRetired *node = kmalloc(sizeof(TagOverflowRetired));
            if (node)
            {
                node->buf  = old_ids;
                node->next = cabin->tag_overflow_retired;
                cabin->tag_overflow_retired = node;
            }
        }
    }

    cabin->tag_overflow_ids[cabin->tag_overflow_count++] = tag_id;
    return 0;
}

static int process_clear_tag_bit(process_t *proc, uint16_t tag_id)
{
    cabin_t *cabin = proc->cabin;

    if (tag_id < 64)
    {
        cabin->tag_bits &= ~((uint64_t)1 << tag_id);
        return 0;
    }
    for (uint16_t i = 0; i < cabin->tag_overflow_count; i++)
    {
        if (cabin->tag_overflow_ids[i] == tag_id)
        {
            cabin->tag_overflow_ids[i] = cabin->tag_overflow_ids[cabin->tag_overflow_count - 1];
            cabin->tag_overflow_count--;
            return 0;
        }
    }
    return -1;
}

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

#define PEAK_REPORT_STEP 16u
static uint32_t g_proc_peak = 0;
static uint32_t g_proc_peak_reported = 0;
static uint32_t g_cleanup_peak = 0;
static uint32_t g_cleanup_peak_reported = 0;

static uint32_t process_note_peak_locked(void)
{
    if (process_count <= g_proc_peak)
        return 0;
    g_proc_peak = process_count;
    if (g_proc_peak < g_proc_peak_reported + PEAK_REPORT_STEP)
        return 0;
    g_proc_peak_reported = g_proc_peak;
    return g_proc_peak;
}

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


static void process_init_strand_fields(process_t *proc)
{
    proc->score            = 0;
    proc->last_run_time    = 0;
    proc->consecutive_runs = 0;
    proc->total_cpu_time   = 0;
    proc->cpu_tsc_stamp    = 0;
    proc->park_seq         = 0;
    proc->state            = PROC_CREATED;
    spinlock_init(&proc->state_lock);
    atomic_store_u32(&proc->ref_count, 1);
    proc->destroying    = 0;
    proc->started       = false;
    proc->kcore_pending = 0;
    proc->touch_cleaned = 0;
    proc->quiesce_core  = QUIESCE_CORE_NONE;
    proc->quiesce_seq   = 0;
    proc->on_cpu        = -1;

    proc->irq_stack_top    = 0;
    proc->irq_rip          = 0;
    proc->irq_active       = 0;
    proc->irq_saved_rip    = 0;
    proc->irq_saved_rsp    = 0;
    proc->irq_saved_rflags = 0;
    proc->irq_pending_head = NULL;
    spinlock_init(&proc->irq_lock);

    proc->owed_head     = NULL;
    proc->owed_tail     = NULL;
    proc->owed_count    = 0;
    proc->owed_draining = 0;
    spinlock_init(&proc->owed_lock);

    proc->gone_waiters    = NULL;
    spinlock_init(&proc->gone_lock);
    ChitInit(&proc->chit);

    proc->wait_reason     = WAIT_NONE;
    proc->wait_start_time = 0;
    proc->hash_next       = NULL;
}

static void process_assign_home_core(process_t *proc)
{
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

    if (proc->home_core >= g_amp.total_cores)
    {
        debug_printf("[PROCESS] ERROR: Invalid home_core %u assigned (max %u), using BSP\n",
                     proc->home_core, g_amp.total_cores);
        proc->home_core = g_amp.bsp_index;
    }
}

static bool process_alloc_kernel_stack(process_t *proc)
{
    size_t kernel_stack_size = CONFIG_KERNEL_STACK_PAGES * VMM_PAGE_SIZE;
    size_t total_pages       = CONFIG_KERNEL_STACK_TOTAL_PAGES;

    void *stack_phys = pmm_alloc(total_pages);
    if (!stack_phys)
    {
        kprintf("[PROCESS] ERROR: kernel stack alloc failed (%zu pages, PID %u)\n",
                total_pages, proc->pid);
        return false;
    }

    void *stack_virt_base   = vmm_phys_to_virt((uintptr_t)stack_phys);
    vmm_context_t *kernel_ctx = vmm_get_kernel_context();

    pte_t *guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)stack_virt_base);
    if (!guard_pte)
    {
        kprintf("[PROCESS] ERROR: cannot create guard page PTE for PID %u\n", proc->pid);
        pmm_free(stack_phys, total_pages);
        return false;
    }

    *guard_pte = 0;
    vmm_shootdown_page(kernel_ctx, (uintptr_t)stack_virt_base);

    proc->kernel_stack_guard_base = stack_virt_base;
    proc->kernel_stack            = (void *)((uintptr_t)stack_virt_base + VMM_PAGE_SIZE);
    proc->kernel_stack_top        = (void *)((uintptr_t)proc->kernel_stack + kernel_stack_size);

    debug_printf("[PROCESS] Kernel stack allocated: guard=0x%lx, stack=0x%lx-0x%lx (PID %u)\n",
                 (uintptr_t)proc->kernel_stack_guard_base,
                 (uintptr_t)proc->kernel_stack,
                 (uintptr_t)proc->kernel_stack_top,
                 proc->pid);
    return true;
}

static void process_free_kernel_stack(process_t *proc)
{
    if (!proc->kernel_stack_guard_base)
        return;

    uintptr_t guard_virt = (uintptr_t)proc->kernel_stack_guard_base;
    uintptr_t stack_phys = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);

    proc->kernel_stack_guard_base = NULL;
    proc->kernel_stack            = NULL;

    vmm_context_t *kernel_ctx = vmm_get_kernel_context();
    pte_t *guard_pte = vmm_get_pte(kernel_ctx, guard_virt);
    if (!guard_pte)
    {
        kprintf("[PROCESS] ERROR: guard page 0x%lx has no leaf PTE, leaking %u pages (PID %u)\n",
                guard_virt, (unsigned)CONFIG_KERNEL_STACK_TOTAL_PAGES, proc->pid);
        return;
    }

    *guard_pte = vmm_make_pte(stack_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    vmm_shootdown_page(kernel_ctx, guard_virt);

    pmm_free((void *)stack_phys, CONFIG_KERNEL_STACK_TOTAL_PAGES);

    debug_printf("[PROCESS] Freed kernel stack: guard=0x%lx (PID %u)\n", guard_virt, proc->pid);
}

static bool process_init_context_base(process_t *proc)
{
    memset(&proc->context, 0, sizeof(ProcessContext));
    proc->context.cr3    = vmm_build_cr3(proc->cabin->vmm);
    proc->context.rflags = 0x202;
    proc->context.cs     = GDT_USER_CODE;
    proc->context.ds     = GDT_USER_DATA;
    proc->context.es     = GDT_USER_DATA;
    proc->context.fs     = GDT_USER_DATA;
    proc->context.gs     = GDT_USER_DATA;
    proc->context.ss     = GDT_USER_DATA;

    uint32_t fpu_buf_size = fpu_alloc_size();
    proc->context.fpu_state = kmalloc(fpu_buf_size);
    if (!proc->context.fpu_state)
    {
        kprintf("[PROCESS] ERROR: FPU state buffer alloc failed (%u bytes, PID %u)\n",
                fpu_buf_size, proc->pid);
        return false;
    }
    fpu_init_state(proc->context.fpu_state);
    proc->context.fpu_initialized = true;
    return true;
}

static void process_free_strand_stack(process_t *proc)
{
    if (!proc || proc->user_stack_phys == 0 || !proc->cabin)
        return;
    uint64_t stack_data_base =
        proc->hammock_base + (uint64_t)CABIN_HAMMOCK_STACK_PAGE_OFF * VMM_PAGE_SIZE;
    for (uint32_t i = 0; i < CONFIG_USER_STACK_PAGES; i++)
        vmm_unmap_page(proc->cabin->vmm, stack_data_base + (uint64_t)i * VMM_PAGE_SIZE);
    pmm_free((void *)proc->user_stack_phys, CONFIG_USER_STACK_PAGES);
    proc->user_stack_phys = 0;
}

process_t *process_create(const char *tags)
{
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
    proc->magic    = PROCESS_MAGIC;
    proc->rq_prio  = -1;
    proc->rq_index = -1;

    proc->pid = pid_alloc();
    if (proc->pid == PID_INVALID)
    {
        kprintf("[PROCESS] ERROR: PID allocation failed (exhaustion at %u/%u)\n",
                pid_allocated_count(), PID_MAX_COUNT);
        kfree(proc);
        return NULL;
    }
    proc->generation = pid_generation(proc->pid);

    cabin_t *cabin = cabin_create(proc->pid, tags);
    if (!cabin)
    {
        debug_printf("[PROCESS] ERROR: Failed to create cabin\n");
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    proc->cabin = cabin;

    proc->pocket_ring_phys = cabin->pocket_ring_phys;
    proc->result_ring_phys = cabin->result_ring_phys;
    proc->touch_ring_phys  = cabin->touch_ring_phys;

    process_init_strand_fields(proc);
    process_assign_home_core(proc);

    if (!process_alloc_kernel_stack(proc))
    {
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    if (!process_init_context_base(proc))
    {
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    proc->next         = NULL;
    proc->prev         = NULL;
    proc->ready_next   = NULL;
    proc->cleanup_next = NULL;
    proc->in_ready     = 0;

    spin_lock(&process_lock);

    if (process_count >= PROCESS_MAX_COUNT)
    {
        spin_unlock(&process_lock);
        if (proc->context.fpu_state)
            kfree(proc->context.fpu_state);
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        debug_printf("[PROCESS] ERROR: Process limit reached under lock (%u)\n", PROCESS_MAX_COUNT);
        return NULL;
    }

    proc->prev = NULL;
    proc->next = process_list_head;
    if (process_list_head) process_list_head->prev = proc;
    process_list_head = proc;
    process_hash_insert(proc);
    process_count++;
    uint32_t peak = process_note_peak_locked();
    spin_unlock(&process_lock);

    if (peak)
        kprintf("[PROCESS] live high-water %u processes (limit %u)\n", peak, PROCESS_MAX_COUNT);

    return proc;
}

void process_ref_inc(process_t *proc)
{
    if (!proc)
        return;

    uint32_t old = atomic_fetch_add_u32(&proc->ref_count, 1);

#ifdef DEBUG_REFCOUNT
    debug_printf("[REFCOUNT] PID %u: %u -> %u (INC)\n", proc->pid, old, old + 1);
#endif

    if (old > UINT32_MAX - 100)
    {
        debug_printf("[PROCESS] PANIC: ref_count overflow for PID %u (old=%u)\n",
                     proc->pid, old);
        while (1)
            asm volatile("cli; hlt");
    }
}

void process_ref_dec(process_t *proc)
{
    if (!proc)
        return;

    uint32_t old = atomic_fetch_sub_u32(&proc->ref_count, 1);

    if (old == 0)
    {
        uint32_t expected = UINT32_MAX;
        __atomic_compare_exchange_n(&proc->ref_count, &expected, 1u,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        kprintf("[PROCESS] BUG: ref_count underflow for PID %u (restored)\n", proc->pid);
        return;
    }

#ifdef DEBUG_REFCOUNT
    debug_printf("[REFCOUNT] PID %u: %u -> %u (DEC)\n", proc->pid, old, old - 1);
#endif

    if (old == 1)
    {
        process_state_t state = process_get_state(proc);
        if (state == PROC_DONE || state == PROC_CRASHED)
        {
            if (proc->magic == CONFIG_PROCESS_POISON_MAGIC)
            {
                if (!cleanup_queue_enqueue(proc))
                    process_cleanup_immediate(proc);
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

    if (new_state == PROC_WORKING &&
        (old_state == PROC_DONE || old_state == PROC_CRASHED))
    {
        spin_unlock(&proc->state_lock);
        return;
    }

    if (new_state == PROC_WAITING && KResultRingHasPendingReply(proc))
    {
        new_state = PROC_WORKING;
    }

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
        return -1;

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
            asm volatile("cli; hlt");
    }

    if (proc->home_core >= g_amp.total_cores)
    {
        debug_printf("[PROCESS] ERROR: PID %u has invalid home_core %u (max %u)\n",
                     proc->pid, proc->home_core, g_amp.total_cores);
        return;
    }

    __atomic_store_n(&proc->destroying, 1, __ATOMIC_SEQ_CST);
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
            is_running     = true;
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

    process_hash_remove(proc);

    if (proc->prev) proc->prev->next = proc->next;
    else            process_list_head = proc->next;
    if (proc->next) proc->next->prev = proc->prev;
    proc->prev = NULL;
    proc->next = NULL;

    process_count--;

    spin_unlock(&process_lock);

    process_set_state(proc, PROC_CRASHED);

    sched_dequeue_all_cores(proc);

    uint32_t cancelled = async_io_cancel_by_pid(proc->pid);
    if (cancelled > 0)
    {
        debug_printf("[PROCESS] Cancelled %u pending async I/O for PID %u\n",
                     cancelled, proc->pid);
    }

    ManifestReleaseAllForOwner(proc->pid);


    TouchCleanupProcess(proc, PROC_EXIT_CRASHED);

    {
        struct __attribute__((packed)) { uint32_t pid; uint32_t cabin_pid; } ev;
        ev.pid       = proc->pid;
        ev.cabin_pid = (proc->cabin ? proc->cabin->spawner_pid : 0);
        TouchPublish("strand:exited", &ev, sizeof(ev));
    }

    _Static_assert(__builtin_offsetof(StrandPool, GenState) == 116,
                   "kernel StrandPool GenState offset");
    if (proc->strand_pool_va && proc->cabin) {
        uintptr_t ph = vmm_virt_to_phys(proc->cabin->vmm, proc->strand_pool_va);
        if (ph) {
            volatile uint32_t *gs = (volatile uint32_t *)
                ((uint8_t *)vmm_phys_to_virt(ph) +
                 __builtin_offsetof(StrandPool, GenState));
            uint32_t expect = STRANDPOOL_PACK(proc->strand_pool_gen, STRANDPOOL_LIVE);
            uint32_t orphan = STRANDPOOL_PACK(proc->strand_pool_gen, STRANDPOOL_ORPHANED);
            bool stamped = __atomic_compare_exchange_n(gs, &expect, orphan, false,
                                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
            if (stamped && proc->strand_pool_orphan_va) {
                uintptr_t fph = vmm_virt_to_phys(proc->cabin->vmm,
                                                  proc->strand_pool_orphan_va);
                if (fph)
                    __atomic_store_n((volatile uint32_t *)vmm_phys_to_virt(fph),
                                     1u, __ATOMIC_RELEASE);
            }
        }
    }

    BrookCleanupProcess(proc);

    cet_process_destroy(proc);
    cet_process_destroy_kernel_ssp(proc);

    AddrWaitUnlinkIfLinked(&proc->addr_wait_entry);

    proc->magic = CONFIG_PROCESS_POISON_MAGIC;

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

    if (page_count < 5)
        page_count = 5;

    void *code_phys = pmm_alloc_zero(page_count);
    if (!code_phys)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate %zu pages for binary\n", page_count);
        return -1;
    }

    void *code_virt = vmm_phys_to_virt((uintptr_t)code_phys);
    memcpy(code_virt, binary_data, size);

    {
        const uint8_t *head = (const uint8_t *)code_virt;
        size_t look = size < 16 ? size : 16;
        bool anything = false;
        for (size_t i = 0; i < look; i++) {
            if (head[i]) { anything = true; break; }
        }
        if (!anything) {
            kprintf("[PROCESS] refusing to start an image whose first %zu "
                    "bytes are all zero — this is a read that did not happen, "
                    "not a program\n", look);
            pmm_free(code_phys, page_count);
            return -1;
        }
    }

    uintptr_t entry_point = VMM_CABIN_CODE_START;
    int result = vmm_map_code_region(proc->cabin->vmm, (uintptr_t)code_phys,
                                     page_count * VMM_PAGE_SIZE, &entry_point);
    if (result != 0)
    {
        debug_printf("[PROCESS] ERROR: Failed to map code region\n");
        debug_printf("[PROCESS] Physical address: 0x%lx, Size: %zu bytes\n",
                     (uintptr_t)code_phys, size);
        pmm_free(code_phys, page_count);
        return -1;
    }

    proc->nameplate_va    = 0;
    proc->nameplate_bytes = 0;
    (void)nameplate_locate(code_virt, size, VMM_CABIN_CODE_START,
                           &proc->nameplate_va, &proc->nameplate_bytes);

    const uint8_t *probe = (const uint8_t *)code_virt;
    bool was_elf = (size >= 4 && probe[0] == 0x7F && probe[1] == 'E' &&
                    probe[2] == 'L' && probe[3] == 'F');
    if (was_elf)
        pmm_free(code_phys, page_count);

    void *user_stack_phys = pmm_alloc(CONFIG_USER_STACK_TOTAL_PAGES);
    if (!user_stack_phys)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate user stack\n");
        pmm_free(code_phys, page_count);
        return -1;
    }

    uint64_t stack_top = proc->cabin->aslr_stack_top;

    uint64_t guard_page_base = stack_top - (CONFIG_USER_STACK_TOTAL_PAGES * VMM_PAGE_SIZE);
    uint64_t stack_data_base = guard_page_base + (CONFIG_USER_STACK_GUARD_PAGES * VMM_PAGE_SIZE);

    vmm_map_result_t map_result = vmm_map_pages(
        proc->cabin->vmm,
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

    uintptr_t heap_start = proc->cabin->aslr_heap_base;
    void *heap_phys = pmm_alloc_zero(CONFIG_USER_HEAP_INITIAL_PAGES);
    if (!heap_phys)
    {
        debug_printf("[PROCESS] ERROR: Failed to allocate initial heap pages\n");
        pmm_free(user_stack_phys, CONFIG_USER_STACK_TOTAL_PAGES);
        pmm_free(code_phys, page_count);
        return -1;
    }

    vmm_map_result_t heap_map = vmm_map_pages(
        proc->cabin->vmm,
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

    proc->cabin->vmm->heap_start = heap_start;
    proc->cabin->vmm->heap_end   = heap_start + CONFIG_USER_HEAP_INITIAL_SIZE;
    proc->cabin->vmm->stack_top  = stack_top;

    CabinInfo *ci = (CabinInfo *)vmm_phys_to_virt(proc->cabin->cabin_info_phys);
    ci->magic       = CABIN_INFO_MAGIC;
    ci->pid         = proc->pid;
    ci->spawner_pid = proc->cabin->spawner_pid;
    ci->generation  = proc->generation;
    ci->heap_base   = heap_start;
    ci->heap_max_size = CABIN_HEAP_MAX_SIZE;
    ci->buf_heap_base = proc->cabin->aslr_buf_heap_base;
    ci->stack_top   = stack_top;
    ci->luggage_addr     = 0;
    ci->luggage_length   = 0;
    ci->luggage_reserved = 0;

    proc->cabin->code_size = size;
    proc->context.rip = entry_point;
    proc->context.rsp = stack_top;

    (void)cet_process_create(proc);
    (void)cet_process_create_kernel_ssp(proc, entry_point);

    debug_printf("[PROCESS] ASLR: PID %u heap=0x%lx stack=0x%lx buf=0x%lx ssp=0x%lx\n",
                 proc->pid, heap_start, stack_top, proc->cabin->aslr_buf_heap_base,
                 (unsigned long)proc->user_ssp_va);

    return 0;
}

process_t *strand_spawn(cabin_t *cabin, uintptr_t entry_va, uint64_t arg, bool joinable)
{
    if (!cabin || !cabin->vmm)
    {
        debug_printf("[STRAND] ERROR: strand_spawn with no cabin\n");
        return NULL;
    }
    if (entry_va == 0 || entry_va >= CABIN_USER_VA_CANONICAL_END)
    {
        debug_printf("[STRAND] ERROR: invalid entry VA 0x%lx\n", (unsigned long)entry_va);
        return NULL;
    }
    if (!g_fsgsbase_active)
    {
        debug_printf("[STRAND] ERROR: strand_spawn requires FSGSBASE (per-strand TLS)\n");
        return NULL;
    }
    if (atomic_load_u32(&process_count) >= PROCESS_MAX_COUNT)
    {
        kprintf("[STRAND] ERROR: process limit reached (%u)\n", PROCESS_MAX_COUNT);
        return NULL;
    }

    process_t *proc = kmalloc(sizeof(process_t));
    if (!proc)
        return NULL;

    memset(proc, 0, sizeof(process_t));
    proc->magic    = PROCESS_MAGIC;
    proc->rq_prio  = -1;
    proc->rq_index = -1;
    proc->reap_blocked = joinable ? 1u : 0u;

    proc->pid = pid_alloc();
    if (proc->pid == PID_INVALID)
    {
        kprintf("[STRAND] ERROR: PID allocation failed (exhaustion)\n");
        kfree(proc);
        return NULL;
    }
    proc->generation = pid_generation(proc->pid);

    proc->cabin = cabin;
    cabin_ref_inc(cabin);

    process_init_strand_fields(proc);
    process_assign_home_core(proc);

    if (!process_alloc_kernel_stack(proc))
    {
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    if (!process_init_context_base(proc))
    {
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    spin_lock(&cabin->hammock_lock);
    uint64_t slot_base = cabin->hammock_va_next;
    bool hammock_ok = (slot_base + CABIN_HAMMOCK_SLOT_SIZE) <= CABIN_HAMMOCK_END;
    if (hammock_ok)
        cabin->hammock_va_next = slot_base + CABIN_HAMMOCK_SLOT_SIZE;
    spin_unlock(&cabin->hammock_lock);

    if (!hammock_ok)
    {
        kprintf("[STRAND] ERROR: hammock window exhausted (PID %u)\n", proc->pid);
        kfree(proc->context.fpu_state);
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    proc->hammock_base = slot_base;

    uint64_t stack_data_base =
        slot_base + (uint64_t)CABIN_HAMMOCK_STACK_PAGE_OFF * VMM_PAGE_SIZE;
    uint64_t stack_top = stack_data_base + (uint64_t)CONFIG_USER_STACK_PAGES * VMM_PAGE_SIZE;

    void *ustack_phys = pmm_alloc(CONFIG_USER_STACK_PAGES);
    if (!ustack_phys)
    {
        kprintf("[STRAND] ERROR: user stack alloc failed (%u pages, PID %u)\n",
                (unsigned)CONFIG_USER_STACK_PAGES, proc->pid);
        kfree(proc->context.fpu_state);
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    vmm_map_result_t smap = vmm_map_pages(
        cabin->vmm, stack_data_base, (uintptr_t)ustack_phys,
        CONFIG_USER_STACK_PAGES,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);
    if (!smap.success)
    {
        kprintf("[STRAND] ERROR: user stack map failed: %s (PID %u)\n", smap.error_msg, proc->pid);
        pmm_free(ustack_phys, CONFIG_USER_STACK_PAGES);
        kfree(proc->context.fpu_state);
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }
    proc->user_stack_phys = (uintptr_t)ustack_phys;

    proc->context.rip = entry_va;
    proc->context.rsp = stack_top - 8;
    proc->context.rdi = arg;

    (void)cet_process_create(proc);
    (void)cet_process_create_kernel_ssp(proc, entry_va);

    if (!strand_rings_create(proc))
    {
        kprintf("[STRAND] ERROR: per-strand rings/TLS setup failed (PID %u)\n", proc->pid);
        cet_process_destroy(proc);
        cet_process_destroy_kernel_ssp(proc);
        process_free_strand_stack(proc);
        kfree(proc->context.fpu_state);
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    proc->next         = NULL;
    proc->prev         = NULL;
    proc->ready_next   = NULL;
    proc->cleanup_next = NULL;
    proc->in_ready     = 0;

    spin_lock(&process_lock);
    if (process_count >= PROCESS_MAX_COUNT)
    {
        spin_unlock(&process_lock);
        strand_rings_destroy(proc);
        cet_process_destroy(proc);
        cet_process_destroy_kernel_ssp(proc);
        process_free_strand_stack(proc);
        kfree(proc->context.fpu_state);
        process_free_kernel_stack(proc);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        kprintf("[STRAND] ERROR: process limit reached under lock (%u)\n", PROCESS_MAX_COUNT);
        return NULL;
    }
    proc->prev = NULL;
    proc->next = process_list_head;
    if (process_list_head) process_list_head->prev = proc;
    process_list_head = proc;
    process_hash_insert(proc);
    process_count++;
    uint32_t peak = process_note_peak_locked();
    spin_unlock(&process_lock);

    if (peak)
        kprintf("[PROCESS] live high-water %u processes (limit %u)\n", peak, PROCESS_MAX_COUNT);

    debug_printf("[STRAND] spawned PID %u in cabin (entry=0x%lx rsp=0x%lx arg=0x%lx home_core=%u)\n",
                 proc->pid, (unsigned long)entry_va, (unsigned long)stack_top,
                 (unsigned long)arg, proc->home_core);

    {
        struct __attribute__((packed)) { uint32_t pid; uint32_t cabin_pid; } ev;
        ev.pid       = proc->pid;
        ev.cabin_pid = cabin->spawner_pid;
        TouchPublish("strand:spawned", &ev, sizeof(ev));
    }

    process_set_state(proc, PROC_WORKING);

    return proc;
}

void process_list_validate(const char *caller)
{
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
                kfree(seen);
                while (1)
                    asm volatile("cli; hlt");
            }
        }

        if (curr->magic != PROCESS_MAGIC)
        {
            debug_printf("[PROCESS] CORRUPTION: Invalid magic 0x%x at %p (caller: %s)\n",
                         curr->magic, (void *)curr, caller);
            kfree(seen);
            while (1)
                asm volatile("cli; hlt");
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
            asm volatile("cli; hlt");
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
            if (p->magic == PROCESS_MAGIC && !p->destroying)
                out[n++] = p->pid;
        }
    }
    spin_unlock(&process_lock);
    return n;
}

process_t *process_find(uint32_t pid)
{
    if (pid == PROCESS_INVALID_PID)
        return NULL;

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
        return NULL;

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
    debug_printf("[TEST]   CabinInfo (phys): 0x%lx\n", proc->cabin->cabin_info_phys);
    debug_printf("[TEST]   PocketRing (phys): 0x%lx\n", proc->cabin->pocket_ring_phys);
    debug_printf("[TEST]   ResultRing (phys): 0x%lx\n", proc->cabin->result_ring_phys);
    debug_printf("[TEST]   Code start (virt): 0x%lx\n", proc->cabin->code_start);
    debug_printf("[TEST]   TagBits: 0x%lx\n", proc->cabin->tag_bits);

    uint8_t test_binary[64];
    for (int i = 0; i < 64; i++)
        test_binary[i] = (uint8_t)i;

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
    debug_printf("[TEST]   Code size: %zu bytes\n", proc->cabin->code_size);

    debug_printf("[TEST] Verifying process lookup...\n");
    process_t *found = process_find(proc->pid);
    if (found == proc)
        debug_printf("[TEST] SUCCESS: Process lookup working\n");
    else
        debug_printf("[TEST] FAILED: Process lookup failed\n");

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
#endif

bool process_has_tag_id(process_t *proc, uint16_t tag_id)
{
    if (!proc || !proc->cabin)
        return false;
    if (tag_id < 64)
        return (__atomic_load_n(&proc->cabin->tag_bits, __ATOMIC_ACQUIRE)
                & ((uint64_t)1 << tag_id)) != 0;
    uint16_t  n   = __atomic_load_n(&proc->cabin->tag_overflow_count, __ATOMIC_ACQUIRE);
    uint16_t *ids = __atomic_load_n(&proc->cabin->tag_overflow_ids,   __ATOMIC_ACQUIRE);
    for (uint16_t i = 0; ids && i < n; i++)
    {
        if (ids[i] == tag_id)
            return true;
    }
    return false;
}

uint64_t *process_active_memtags(process_t *proc)
{
    return (proc && proc->cabin) ? proc->cabin->active_memtags : NULL;
}


#define PROCESS_USER_SSP_REGION_SIZE       (4u * 4096u)
#define PROCESS_USER_SSP_STACK_SLACK       (8ULL * 1024ULL * 1024ULL)
#define PROCESS_USER_SSP_REGION_TOP        \
    (VMM_USER_STACK_TOP - PROCESS_USER_SSP_STACK_SLACK)
#define PROCESS_USER_SSP_REGION_BASE       \
    (PROCESS_USER_SSP_REGION_TOP - PROCESS_USER_SSP_REGION_SIZE)
#define PROCESS_USER_SSP_GUARD_HI_BASE     PROCESS_USER_SSP_REGION_TOP
#define PROCESS_USER_SSP_GUARD_LO_BASE     (PROCESS_USER_SSP_REGION_BASE - 0x1000ULL)

#define PROCESS_USER_SSP_GUARD_PAGE        0x1000ULL
_Static_assert(CABIN_HAMMOCK_STACK_PAGE_OFF + CONFIG_USER_STACK_PAGES + 1u + 4u + 1u
                   <= CABIN_HAMMOCK_SLOT_PAGES,
               "hammock slot too small for [guard|stack|guard|SSP(4pg)|guard]");
_Static_assert(PROCESS_USER_SSP_REGION_SIZE == 4u * 4096u,
               "user SSP region must be 4 pages (matches CET_USER_SSP_PAGES)");

static uintptr_t process_strand_ssp_base(process_t *proc)
{
    if (proc && proc->hammock_base != 0)
    {
        uint32_t ssp_page = CABIN_HAMMOCK_STACK_PAGE_OFF + CONFIG_USER_STACK_PAGES + 1u;
        return proc->hammock_base + (uintptr_t)ssp_page * VMM_PAGE_SIZE;
    }
    return PROCESS_USER_SSP_REGION_BASE;
}

uintptr_t process_user_ssp_va_for(process_t *proc)
{
    return process_strand_ssp_base(proc);
}

uintptr_t process_user_ssp_guard_lo_for(process_t *proc)
{
    return process_strand_ssp_base(proc) - PROCESS_USER_SSP_GUARD_PAGE;
}

uintptr_t process_user_ssp_guard_hi_for(process_t *proc)
{
    return process_strand_ssp_base(proc) + PROCESS_USER_SSP_REGION_SIZE;
}

uintptr_t process_get_user_ssp_phys(process_t *proc)
{
    return proc ? proc->user_ssp_phys : 0;
}

uintptr_t process_get_user_ssp_va(process_t *proc)
{
    return proc ? proc->user_ssp_va : 0;
}

uint32_t process_get_user_ssp_size(process_t *proc)
{
    return proc ? proc->user_ssp_size : 0;
}

void process_set_user_ssp(process_t *proc, uintptr_t phys, uintptr_t va, uint32_t size)
{
    if (!proc) return;
    proc->user_ssp_phys = phys;
    proc->user_ssp_va   = va;
    proc->user_ssp_size = size;
}

void *process_get_cabin(process_t *proc)
{
    return (proc && proc->cabin) ? (void *)proc->cabin->vmm : NULL;
}

bool process_has_tag(process_t *proc, const char *tag)
{
    if (!proc || !proc->cabin || !tag || tag[0] == '\0')
        return false;

    if (!tag_is_wildcard(tag))
    {
        uint16_t tid = tagfs_tag_lookup(tag);
        if (tid == TAGFS_INVALID_TAG_ID)
            return false;
        return process_has_tag_id(proc, tid);
    }

    uint64_t bits = proc->cabin->tag_bits;
    while (bits)
    {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        char full[512];
        if (!tagfs_tag_text(i, full, sizeof(full))) continue;
        if (tag_match(tag, full))
            return true;
    }
    for (uint16_t j = 0; j < proc->cabin->tag_overflow_count; j++)
    {
        char full[512];
        if (!tagfs_tag_text(proc->cabin->tag_overflow_ids[j], full, sizeof(full)))
            continue;
        if (tag_match(tag, full))
            return true;
    }
    return false;
}

int process_add_tag(process_t *proc, const char *tag)
{
    if (!proc || !proc->cabin || !tag || tag[0] == '\0')
        return -1;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized)
    {
        uint32_t bit = value[0] ? 0u : auth_bit_for_key(key);
        if (bit == 0)
            return -1;
        __atomic_or_fetch(&proc->cabin->auth_bits, bit, __ATOMIC_RELAXED);
        return 0;
    }

    spin_lock(&process_lock);

    uint16_t tid = tagfs_tag_intern(tag);
    if (tid == TAGFS_INVALID_TAG_ID)
    {
        spin_unlock(&process_lock);
        return -1;
    }

    UseContextBindTag(tag, tid);

    int ret = process_set_tag_bit(proc, tid);
    if (ret == 0 && !value[0])
        __atomic_or_fetch(&proc->cabin->auth_bits, auth_bit_for_key(key), __ATOMIC_RELAXED);
    spin_unlock(&process_lock);
    return ret;
}

int process_remove_tag(process_t *proc, const char *tag)
{
    if (!proc || !proc->cabin || !tag || tag[0] == '\0')
        return -1;

    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized)
        return -1;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));

    uint16_t tid = tagfs_tag_lookup(tag);
    if (tid == TAGFS_INVALID_TAG_ID)
        return 0;

    spin_lock(&process_lock);
    int ret = process_clear_tag_bit(proc, tid);
    if (!value[0])
        __atomic_and_fetch(&proc->cabin->auth_bits, ~auth_bit_for_key(key), __ATOMIC_RELAXED);
    spin_unlock(&process_lock);
    return ret;
}

void process_start_initial(process_t *proc)
{
    if (!proc)
    {
        debug_printf("[PROCESS] PANIC: process_start_initial called with NULL process\n");
        while (1)
            asm volatile("cli; hlt");
    }

    if (proc->state != PROC_WORKING)
    {
        debug_printf("[PROCESS] PANIC: process_start_initial called with process not in WORKING state\n");
        debug_printf("[PROCESS]   PID %u is in state %d (expected PROC_WORKING=%d)\n",
                     proc->pid, proc->state, PROC_WORKING);
        while (1)
            asm volatile("cli; hlt");
    }

    if (proc->started)
    {
        debug_printf("[PROCESS] PANIC: process_start_initial called on process that already ran\n");
        debug_printf("[PROCESS]   PID %u has started=true\n", proc->pid);
        while (1)
            asm volatile("cli; hlt");
    }

    process_set_state(proc, PROC_WORKING);

    scheduler_state_t *sched = scheduler_get_state();
    spin_lock(&sched->scheduler_lock);
    sched->current_process = proc;
    spin_unlock(&sched->scheduler_lock);

    asm volatile("cli");

    per_core_set_kernel_rsp((uint64_t)proc->kernel_stack_top,
                            (uint64_t)proc->kernel_stack);

    uint64_t target_cr3 = proc->context.cr3;
    if (vmm_pcid_active())
        target_cr3 |= (1ULL << 63);
    __asm__ volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");

    uint64_t verify_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(verify_cr3));
    if (verify_cr3 != proc->context.cr3)
    {
        debug_printf("[PROCESS] PANIC: CR3 verification failed (wrote 0x%lx, read 0x%lx)\n",
                     proc->context.cr3, verify_cr3);
        while (1)
            asm volatile("cli; hlt");
    }

    cet_load_user_ssp_for_iretq(proc);

    jump_to_userspace(proc->context.rip, proc->context.rsp, proc->context.rflags);

    debug_printf("[PROCESS] PANIC: jump_to_userspace returned (should never happen)\n");
    while (1)
        asm volatile("cli; hlt");
}

static bool snapshot_append_tag(char *buffer, size_t buffer_size, size_t *pos,
                                bool *first, uint16_t tag_id)
{
    char text[512];
    if (!tagfs_tag_text(tag_id, text, sizeof(text)))
        return true;

    size_t len  = strlen(text);
    size_t need = len + (*first ? 0u : 1u);
    if (*pos + need >= buffer_size)
        return false;

    if (!*first)
        buffer[(*pos)++] = ',';
    *first = false;
    memcpy(buffer + *pos, text, len);
    *pos += len;
    return true;
}

size_t process_snapshot_tags(process_t *proc, char *buffer, size_t buffer_size)
{
    if (!proc || !proc->cabin || !buffer || buffer_size == 0)
        return 0;

    spin_lock(&process_lock);

    size_t pos = 0;
    bool first = true;

    uint64_t bits = proc->cabin->tag_bits;
    while (bits)
    {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        if (!snapshot_append_tag(buffer, buffer_size, &pos, &first, i))
            break;
    }

    for (uint16_t j = 0; j < proc->cabin->tag_overflow_count; j++)
    {
        if (!snapshot_append_tag(buffer, buffer_size, &pos, &first,
                                 proc->cabin->tag_overflow_ids[j]))
            break;
    }

    buffer[pos] = '\0';
    spin_unlock(&process_lock);
    return pos;
}

static bool cleanup_queue_enqueue(process_t *proc)
{
    if (!proc)
        return false;

    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&proc->cleanup_enqueued, &expected, 1u,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        kprintf("[PROCESS] BUG: double cleanup-enqueue prevented for PID %u\n",
                proc->pid);
        return true;
    }

    proc->cleanup_next = NULL;

    spin_lock(&g_cleanup_queue.lock);
    if (g_cleanup_queue.tail)
        g_cleanup_queue.tail->cleanup_next = proc;
    else
        g_cleanup_queue.head = proc;
    g_cleanup_queue.tail = proc;
    g_cleanup_queue.count++;
    uint32_t depth = g_cleanup_queue.count;
    bool report = (depth > g_cleanup_peak &&
                   depth >= g_cleanup_peak_reported + PEAK_REPORT_STEP);
    if (depth > g_cleanup_peak)
        g_cleanup_peak = depth;
    if (report)
        g_cleanup_peak_reported = depth;
    spin_unlock(&g_cleanup_queue.lock);

    if (report)
        kprintf("[PROCESS] cleanup-queue high-water %u pending\n", depth);
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

    if (proc->context.fpu_state)
    {
        kfree(proc->context.fpu_state);
        proc->context.fpu_state = NULL;
    }

    process_free_kernel_stack(proc);

    TouchOwedRelease(proc);

    strand_rings_destroy(proc);
    process_free_strand_stack(proc);

    cabin_ref_dec(proc->cabin);
    proc->cabin = NULL;

    AddrWaitUnlinkIfLinked(&proc->addr_wait_entry);

    pid_free(proc->pid);
    proc->pid = PID_INVALID;

    kfree(proc);
}

void process_force_release_locks_for_shutdown(void)
{
    spin_force_release(&process_lock);
    spin_force_release(&g_cleanup_queue.lock);
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
        debug_printf("[PROCESS] Deferred cleanup: freed %u processes\n", cleaned);
}

static bool strand_stack_in_use(process_t *proc)
{
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        scheduler_state_t *s = scheduler_get_core(c);
        if (s && __atomic_load_n(&s->current_process, __ATOMIC_ACQUIRE) == proc)
            return true;
    }

    uint8_t qc = proc->quiesce_core;
    if (qc == QUIESCE_CORE_NONE)
        return false;
    scheduler_state_t *qs = scheduler_get_core(qc);
    if (!qs)
        return false;
    return __atomic_load_n(&qs->quiesce_seq, __ATOMIC_ACQUIRE) <= proc->quiesce_seq;
}

static volatile uint8_t g_strand_reaping = 0;

void process_reap_strands(void)
{
    if (__atomic_exchange_n(&g_strand_reaping, 1, __ATOMIC_ACQUIRE) != 0)
        return;

    process_t *batch[CONFIG_STRAND_REAP_BATCH];
    uint32_t n = 0;

    spin_lock(&process_lock);
    for (process_t *p = process_list_head;
         p && n < CONFIG_STRAND_REAP_BATCH;
         p = p->next)
    {
        if (p->magic != PROCESS_MAGIC) continue;
        if (__atomic_load_n(&p->destroying, __ATOMIC_ACQUIRE)) continue;
        if (__atomic_load_n(&p->reap_blocked, __ATOMIC_ACQUIRE)) continue;
        process_state_t st = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE);
        if (st == PROC_DONE || st == PROC_CRASHED)
        {
            if (strand_stack_in_use(p))
                continue;
            if (p->rq_prio >= 0 || p->rq_index >= 0)
                continue;
            process_ref_inc(p);
            batch[n++] = p;
        }
    }
    spin_unlock(&process_lock);

    for (uint32_t i = 0; i < n; i++)
    {
        process_destroy(batch[i]);
        process_ref_dec(batch[i]);
    }

    __atomic_store_n(&g_strand_reaping, 0, __ATOMIC_RELEASE);
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

    for (;;)
    {
        process_t *proc = cleanup_queue_dequeue();
        if (!proc)
            break;
        process_cleanup_immediate(proc);
        total_cleaned++;
    }

    debug_printf("[PROCESS] Cleanup queue flushed (%u processes freed)\n", total_cleaned);
}