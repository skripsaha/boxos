#include "process.h"
#include "cabin.h"
#include "klib.h"
#include "kernel_config.h"
#include "pmm.h"
#include "memtag.h"
#include "manifest.h"
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
#include "cet_lifecycle.h"  /* Phase 2K+ per-process shadow-stack hooks */

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

/*
 * Tag bit mutation on process tags — these are now cabin-level operations
 * but called from process.c helpers that hold process_lock.
 *
 * process_set_tag_bit / process_clear_tag_bit operate on proc->cabin->tag_bits
 * and proc->cabin->tag_overflow_*.
 *
 * See cabin.c: cabin_set_tag_bit for the matching creation-time path.
 * Lock-free read protocol (overflow_ids) is identical to the old process.c
 * version — atomic store-release on publish, atomic load-acquire by readers.
 */
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
            kfree(old_ids);
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
        debug_printf("[PROCESS] ERROR: PID allocation failed (exhaustion at %u/%u)\n",
                     pid_allocated_count(), PID_MAX_COUNT);
        kfree(proc);
        return NULL;
    }

    cabin_t *cabin = cabin_create(proc->pid, tags);
    if (!cabin)
    {
        debug_printf("[PROCESS] ERROR: Failed to create cabin\n");
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    proc->cabin = cabin;

    proc->score            = 0;
    proc->last_run_time    = 0;
    proc->consecutive_runs = 0;
    proc->total_cpu_time   = 0;
    proc->state            = PROC_CREATED;
    spinlock_init(&proc->state_lock);
    atomic_store_u32(&proc->ref_count, 1);
    proc->destroying   = 0;
    proc->started      = false;
    proc->kcore_pending = 0;
    proc->touch_cleaned = 0;

    proc->irq_stack_top    = 0;
    proc->irq_rip          = 0;
    proc->irq_active       = 0;
    proc->irq_saved_rip    = 0;
    proc->irq_saved_rsp    = 0;
    proc->irq_saved_rflags = 0;
    proc->irq_pending_head = NULL;
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

    if (proc->home_core >= g_amp.total_cores)
    {
        debug_printf("[PROCESS] ERROR: Invalid home_core %u assigned (max %u), using BSP\n",
                     proc->home_core, g_amp.total_cores);
        proc->home_core = g_amp.bsp_index;
    }

    proc->wait_reason     = WAIT_NONE;
    proc->wait_start_time = 0;
    proc->hash_next       = NULL;

    size_t kernel_stack_size  = CONFIG_KERNEL_STACK_PAGES * VMM_PAGE_SIZE;
    size_t total_pages        = CONFIG_KERNEL_STACK_TOTAL_PAGES;

    void *stack_phys = pmm_alloc(total_pages);
    if (!stack_phys)
    {
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    void *stack_virt_base = vmm_phys_to_virt((uintptr_t)stack_phys);

    vmm_context_t *kernel_ctx = vmm_get_kernel_context();

    pte_t *guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)stack_virt_base);
    if (!guard_pte)
    {
        debug_printf("[PROCESS] FATAL: Cannot create guard page PTE for PID %u\n", proc->pid);
        pmm_free(stack_phys, total_pages);
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
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
        debug_printf("[PROCESS] ERROR: Failed to allocate FPU state buffer (%u bytes)\n", fpu_buf_size);
        if (proc->kernel_stack_guard_base)
        {
            uintptr_t sp = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);
            pmm_free((void *)sp, CONFIG_KERNEL_STACK_TOTAL_PAGES);
        }
        cabin_ref_dec(cabin);
        proc->cabin = NULL;
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }
    fpu_init_state(proc->context.fpu_state);
    proc->context.fpu_initialized = true;

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
        if (proc->kernel_stack_guard_base)
        {
            uintptr_t sp = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);
            pmm_free((void *)sp, CONFIG_KERNEL_STACK_TOTAL_PAGES);
        }
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
    spin_unlock(&process_lock);

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

    uint32_t cancelled = async_io_cancel_by_pid(proc->pid);
    if (cancelled > 0)
    {
        debug_printf("[PROCESS] Cancelled %u pending async I/O for PID %u\n",
                     cancelled, proc->pid);
    }

    ManifestReleaseAllForOwner(proc->pid);

    if (proc->cabin)
    {
        if (proc->cabin->pocket_ring_phys) {
            MemTagClearByPhys(proc->cabin->pocket_ring_phys, "purpose:shared");
            MemTagClearByPhys(proc->cabin->pocket_ring_phys, "purpose:pocket-ring");
        }
        if (proc->cabin->result_ring_phys) {
            MemTagClearByPhys(proc->cabin->result_ring_phys, "purpose:shared");
            MemTagClearByPhys(proc->cabin->result_ring_phys, "purpose:result-ring");
        }
        if (proc->cabin->touch_ring_phys) {
            MemTagClearByPhys(proc->cabin->touch_ring_phys, "purpose:shared");
            MemTagClearByPhys(proc->cabin->touch_ring_phys, "purpose:touch-ring");
        }
    }

    TouchCleanupProcess(proc);

    BayCleanupProcess(proc);

    BrookCleanupProcess(proc);

    cet_process_destroy(proc);
    cet_process_destroy_kernel_ssp(proc);

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
    ci->reserved    = 0;
    ci->heap_base   = heap_start;
    ci->heap_max_size = CABIN_HEAP_MAX_SIZE;
    ci->buf_heap_base = proc->cabin->aslr_buf_heap_base;
    ci->stack_top   = stack_top;

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
#endif // CONFIG_KERNEL_TESTS

bool process_has_tag_id(process_t *proc, uint16_t tag_id)
{
    if (!proc || !proc->cabin)
        return false;
    if (tag_id < 64)
        return (proc->cabin->tag_bits & ((uint64_t)1 << tag_id)) != 0;
    for (uint16_t i = 0; i < proc->cabin->tag_overflow_count; i++)
    {
        if (proc->cabin->tag_overflow_ids[i] == tag_id)
            return true;
    }
    return false;
}

uint64_t *process_active_memtags(process_t *proc)
{
    return (proc && proc->cabin) ? proc->cabin->active_memtags : NULL;
}

/* ─── Phase 2K+ CET SSP accessors ────────────────────────────────── */

#define PROCESS_USER_SSP_REGION_SIZE       (4u * 4096u)
#define PROCESS_USER_SSP_STACK_SLACK       (8ULL * 1024ULL * 1024ULL)
#define PROCESS_USER_SSP_REGION_TOP        \
    (VMM_USER_STACK_TOP - PROCESS_USER_SSP_STACK_SLACK)
#define PROCESS_USER_SSP_REGION_BASE       \
    (PROCESS_USER_SSP_REGION_TOP - PROCESS_USER_SSP_REGION_SIZE)
#define PROCESS_USER_SSP_GUARD_HI_BASE     PROCESS_USER_SSP_REGION_TOP
#define PROCESS_USER_SSP_GUARD_LO_BASE     (PROCESS_USER_SSP_REGION_BASE - 0x1000ULL)

uintptr_t process_user_ssp_va_for(process_t *proc)
{
    (void)proc;
    return PROCESS_USER_SSP_REGION_BASE;
}

uintptr_t process_user_ssp_guard_lo_for(process_t *proc)
{
    (void)proc;
    return PROCESS_USER_SSP_GUARD_LO_BASE;
}

uintptr_t process_user_ssp_guard_hi_for(process_t *proc)
{
    (void)proc;
    return PROCESS_USER_SSP_GUARD_HI_BASE;
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

/* MemTag Phase 2C accessor — exposes the cabin's vmm_context_t* so memtag.c
 * can locate it for PTE manipulation without pulling process.h into its
 * public headers (avoids circular dep). NULL-safe. */
void *process_get_cabin(process_t *proc)
{
    return (proc && proc->cabin) ? (void *)proc->cabin->vmm : NULL;
}

bool process_has_tag(process_t *proc, const char *tag)
{
    if (!proc || !proc->cabin || !tag || tag[0] == '\0')
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

    uint64_t bits = proc->cabin->tag_bits;
    while (bits)
    {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        const char *k = tag_registry_key(reg, i);
        const char *v = tag_registry_value(reg, i);
        if (!k) continue;
        char full[512];
        tagfs_format_tag(full, sizeof(full), k, v);
        if (tag_match(tag, full))
            return true;
    }
    for (uint16_t j = 0; j < proc->cabin->tag_overflow_count; j++)
    {
        uint16_t tid = proc->cabin->tag_overflow_ids[j];
        const char *k = tag_registry_key(reg, tid);
        const char *v = tag_registry_value(reg, tid);
        if (!k) continue;
        char full[512];
        tagfs_format_tag(full, sizeof(full), k, v);
        if (tag_match(tag, full))
            return true;
    }
    return false;
}

int process_add_tag(process_t *proc, const char *tag)
{
    if (!proc || !proc->cabin || !tag || tag[0] == '\0')
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
    if (!proc || !proc->cabin || !tag || tag[0] == '\0')
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

    per_core_set_kernel_rsp((uint64_t)proc->kernel_stack_top);

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

size_t process_snapshot_tags(process_t *proc, char *buffer, size_t buffer_size)
{
    if (!proc || !proc->cabin || !buffer || buffer_size == 0)
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

    uint64_t bits = proc->cabin->tag_bits;
    while (bits)
    {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        const char *k = tag_registry_key(reg, i);
        const char *v = tag_registry_value(reg, i);
        if (!k) continue;

        if (!first && pos < buffer_size - 1)
            buffer[pos++] = ',';
        first = false;

        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;

        if (vlen > 0)
        {
            if (pos + klen + 1 + vlen >= buffer_size) break;
            memcpy(buffer + pos, k, klen); pos += klen;
            buffer[pos++] = ':';
            memcpy(buffer + pos, v, vlen); pos += vlen;
        }
        else
        {
            if (pos + klen >= buffer_size) break;
            memcpy(buffer + pos, k, klen); pos += klen;
        }
    }

    for (uint16_t j = 0; j < proc->cabin->tag_overflow_count; j++)
    {
        uint16_t tid = proc->cabin->tag_overflow_ids[j];
        const char *k = tag_registry_key(reg, tid);
        const char *v = tag_registry_value(reg, tid);
        if (!k) continue;

        if (!first && pos < buffer_size - 1)
            buffer[pos++] = ',';
        first = false;

        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;

        if (vlen > 0)
        {
            if (pos + klen + 1 + vlen >= buffer_size) break;
            memcpy(buffer + pos, k, klen); pos += klen;
            buffer[pos++] = ':';
            memcpy(buffer + pos, v, vlen); pos += vlen;
        }
        else
        {
            if (pos + klen >= buffer_size) break;
            memcpy(buffer + pos, k, klen); pos += klen;
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
    if (g_cleanup_queue.tail)
        g_cleanup_queue.tail->cleanup_next = proc;
    else
        g_cleanup_queue.head = proc;
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

    /* Free sub structs that TouchCleanupProcess (in process_destroy) unlinked
     * from buckets but left allocated.  Reads proc->cabin->subs_head. */
    TouchFinalizeProcess(proc);

    if (proc->context.fpu_state)
    {
        kfree(proc->context.fpu_state);
        proc->context.fpu_state = NULL;
    }

    if (proc->kernel_stack_guard_base)
    {
        uintptr_t guard_virt = (uintptr_t)proc->kernel_stack_guard_base;
        uintptr_t stack_phys = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);

        vmm_context_t *kernel_ctx = vmm_get_kernel_context();
        pte_t *guard_pte = vmm_get_pte(kernel_ctx, guard_virt);
        if (guard_pte)
        {
            *guard_pte = vmm_make_pte(stack_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
            vmm_shootdown_page(kernel_ctx, guard_virt);
        }

        pmm_free((void *)stack_phys, CONFIG_KERNEL_STACK_TOTAL_PAGES);

        debug_printf("[PROCESS] Freed kernel stack: guard=0x%lx (PID %u)\n",
                     guard_virt, proc->pid);

        proc->kernel_stack_guard_base = NULL;
        proc->kernel_stack = NULL;
    }

    /* Drop the strand's reference to its cabin.  In P1 strand_count goes
     * 1→0 here, which triggers cabin_destroy (VMM teardown + ring page
     * free + tag_overflow free). */
    cabin_ref_dec(proc->cabin);
    proc->cabin = NULL;

    /* Unlink the embedded addr-wait entry if still in a bucket — a strand
     * destroyed while parked must not leave a dangling pointer behind.
     * Takes only the per-bucket spinlock (leaf lock; no other locks held). */
    AddrWaitUnlinkIfLinked(&proc->addr_wait_entry);

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
        debug_printf("[PROCESS] Deferred cleanup: freed %u processes\n", cleaned);
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
