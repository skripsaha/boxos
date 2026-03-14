#include "process.h"
#include "klib.h"
#include "pmm.h"
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
#include "tagfs.h"

static process_t *process_list_head = NULL;
static volatile uint32_t process_count = 0;
static spinlock_t process_lock;
static process_cleanup_queue_t g_cleanup_queue;

static int process_set_tag_bit(process_t *proc, uint16_t tag_id)
{
    if (tag_id < 64) {
        proc->tag_bits |= ((uint64_t)1 << tag_id);
        return 0;
    }
    for (uint16_t i = 0; i < proc->tag_overflow_count; i++) {
        if (proc->tag_overflow_ids[i] == tag_id)
            return 0;
    }
    if (proc->tag_overflow_count >= proc->tag_overflow_capacity) {
        uint16_t new_cap = proc->tag_overflow_capacity == 0 ? 8 : proc->tag_overflow_capacity * 2;
        uint16_t *new_ids = kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids)
            return -1;
        if (proc->tag_overflow_ids) {
            memcpy(new_ids, proc->tag_overflow_ids, sizeof(uint16_t) * proc->tag_overflow_count);
            kfree(proc->tag_overflow_ids);
        }
        proc->tag_overflow_ids = new_ids;
        proc->tag_overflow_capacity = new_cap;
    }
    proc->tag_overflow_ids[proc->tag_overflow_count++] = tag_id;
    return 0;
}

static int process_clear_tag_bit(process_t *proc, uint16_t tag_id)
{
    if (tag_id < 64) {
        proc->tag_bits &= ~((uint64_t)1 << tag_id);
        return 0;
    }
    for (uint16_t i = 0; i < proc->tag_overflow_count; i++) {
        if (proc->tag_overflow_ids[i] == tag_id) {
            proc->tag_overflow_ids[i] = proc->tag_overflow_ids[proc->tag_overflow_count - 1];
            proc->tag_overflow_count--;
            return 0;
        }
    }
    return -1;
}

// Hash table for O(1) process_find(pid)
#define PROCESS_HASH_SIZE 256
static process_t *process_hash_table[PROCESS_HASH_SIZE];

static inline uint32_t process_hash(uint32_t pid)
{
    return pid % PROCESS_HASH_SIZE;
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
    while (*pp) {
        if (*pp == proc) {
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

    g_cleanup_queue.head = 0;
    g_cleanup_queue.tail = 0;
    g_cleanup_queue.count = 0;
    spinlock_init(&g_cleanup_queue.lock);

    memset(process_hash_table, 0, sizeof(process_hash_table));

    debug_printf("[PROCESS] Process management initialized\n");
    debug_printf("[PROCESS] Max processes: %u\n", PROCESS_MAX_COUNT);
    debug_printf("[PROCESS] Cabin layout: 0x%lx (NULL trap), 0x%lx (CabinInfo), 0x%lx (PocketRing), 0x%lx (ResultRing), 0x%lx+ (Code)\n",
                 CABIN_NULL_TRAP_START,
                 CABIN_INFO_ADDR, CABIN_POCKET_RING_ADDR, CABIN_RESULT_RING_ADDR, CABIN_CODE_START_ADDR);
    debug_printf("[PROCESS] Hash table size: %u buckets\n", PROCESS_HASH_SIZE);
    debug_printf("[PROCESS] Deferred cleanup queue initialized (max_size=%u)\n",
                 PROCESS_CLEANUP_QUEUE_SIZE);
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

    uint64_t info_phys = 0;
    uint64_t pocket_phys = 0;
    uint64_t result_phys = 0;
    vmm_context_t *cabin = vmm_create_cabin(&info_phys, &pocket_phys, &result_phys);
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
    proc->cabin_info_phys = info_phys;
    proc->pocket_ring_phys = pocket_phys;
    proc->result_ring_phys = result_phys;
    proc->score = 0;
    proc->last_run_time = 0;
    proc->consecutive_runs = 0;
    proc->total_cpu_time = 0;
    proc->state = PROC_CREATED;
    spinlock_init(&proc->state_lock);
    proc->ref_count = 0;
    proc->code_start = VMM_CABIN_CODE_START;
    proc->code_size = 0;
    proc->started = false;

    // ASLR: generate per-process random offsets
    aslr_offsets_t aslr = aslr_generate();
    proc->aslr_stack_top = VMM_USER_STACK_TOP - aslr.stack_offset;
    proc->aslr_heap_base = CABIN_HEAP_BASE + aslr.heap_offset;
    proc->aslr_buf_heap_base = CABIN_BUF_HEAP_START + aslr.buf_heap_offset;
    proc->buf_heap_next = proc->aslr_buf_heap_base;
    proc->next = NULL;

    if (tags && tags[0] != '\0')
    {
        TagFSState *fs = tagfs_get_state();
        if (fs && fs->registry) {
            const char *pos = tags;
            while (*pos) {
                const char *comma = strchr(pos, ',');
                size_t len = comma ? (size_t)(comma - pos) : strlen(pos);
                if (len > 0 && len < 256) {
                    char tag_buf[256];
                    memcpy(tag_buf, pos, len);
                    tag_buf[len] = '\0';

                    char key[256], value[256];
                    tagfs_parse_tag(tag_buf, key, sizeof(key), value, sizeof(value));

                    uint16_t tid = tag_registry_intern(fs->registry, key,
                                                        value[0] ? value : NULL);
                    if (tid != TAGFS_INVALID_TAG_ID) {
                        process_set_tag_bit(proc, tid);
                    }
                }
                if (!comma) break;
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
    vmm_flush_tlb_page((uintptr_t)stack_virt_base);

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
    if (!proc->context.fpu_state) {
        debug_printf("[PROCESS] ERROR: Failed to allocate FPU state buffer (%u bytes)\n", fpu_buf_size);
        if (proc->kernel_stack_guard_base) {
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

    proc->next = process_list_head;
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
        // underflow detected — restore to 1 to prevent further damage
        // the process will leak but the system survives
        atomic_store_u32(&proc->ref_count, 1);
        kprintf("[PROCESS] ERROR: ref_count underflow for PID %u — recovered (process leaked)\n",
                proc->pid);
        return;
    }

#ifdef DEBUG_REFCOUNT
    debug_printf("[REFCOUNT] PID %u: %u -> %u (DEC)\n", proc->pid, old, old - 1);
#endif
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
    proc->state = new_state;
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

    uint32_t refs = process_ref_count(proc);
    if (refs > 0)
    {
        debug_printf("[PROCESS] ERROR: Cannot destroy PID %u with ref_count=%u\n",
                     proc->pid, refs);
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

    scheduler_state_t *sched = scheduler_get_state();

    // check current_process under short lock
    spin_lock(&sched->scheduler_lock);
    if (sched->current_process == proc)
    {
        spin_unlock(&sched->scheduler_lock);
        debug_printf("[PROCESS] ERROR: Cannot destroy current process PID %u\n", proc->pid);
        return;
    }
    spin_unlock(&sched->scheduler_lock);
    // scheduler_lock is now FREE — no deadlock possible below

    spin_lock(&process_lock);

    uint32_t refs = process_ref_count(proc);
    if (refs > 0)
    {
        spin_unlock(&process_lock);
        debug_printf("[PROCESS] PID %u has ref_count=%u, transitioning to DONE\n",
                     proc->pid, refs);
        process_set_state(proc, PROC_DONE);
        return;
    }

    // remove from hash table before poisoning magic
    process_hash_remove(proc);

    // poison magic before removing from list to catch use-after-remove
    proc->magic = 0xDEADDEAD;

    if (process_list_head == proc)
    {
        process_list_head = proc->next;
    }
    else
    {
        process_t *curr = process_list_head;
        while (curr && curr->next != proc)
        {
            curr = curr->next;
        }
        if (curr)
        {
            curr->next = proc->next;
        }
    }

    process_count--;

    spin_unlock(&process_lock);

    process_set_state(proc, PROC_CRASHED);

    uint32_t cancelled = async_io_cancel_by_pid(proc->pid);
    if (cancelled > 0)
    {
        debug_printf("[PROCESS] Cancelled %u pending async I/O for PID %u\n",
                     cancelled, proc->pid);
    }

    if (!cleanup_queue_enqueue(proc))
    {
        // queue full — fall back to immediate cleanup
        debug_printf("[PROCESS] WARNING: Cleanup queue full, immediate cleanup for PID %u\n",
                     proc->pid);
        process_cleanup_immediate(proc);
    }
    // resources (PMM/VMM) are NOT freed here; deferred to process_cleanup_deferred()
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

    int result = vmm_map_code_region(proc->cabin, (uintptr_t)code_phys, page_count * VMM_PAGE_SIZE);
    if (result != 0)
    {
        debug_printf("[PROCESS] ERROR: Failed to map code region\n");
        debug_printf("[PROCESS] Physical address: 0x%lx, Size: %zu bytes\n",
                     (uintptr_t)code_phys, size);
        pmm_free(code_phys, page_count);
        return -1;
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
    proc->context.rip = VMM_CABIN_CODE_START;
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

bool process_has_tag_id(process_t *proc, uint16_t tag_id)
{
    if (!proc) return false;
    if (tag_id < 64)
        return (proc->tag_bits & ((uint64_t)1 << tag_id)) != 0;
    for (uint16_t i = 0; i < proc->tag_overflow_count; i++) {
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

    if (!tag_is_wildcard(tag)) {
        char key[256], value[256];
        tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));
        uint16_t tid = tag_registry_lookup(reg, key, value[0] ? value : NULL);
        if (tid == TAGFS_INVALID_TAG_ID) return false;
        return process_has_tag_id(proc, tid);
    }

    // Wildcard: iterate all tags this process has
    uint64_t bits = proc->tag_bits;
    while (bits) {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        const char *k = tag_registry_key(reg, i);
        const char *v = tag_registry_value(reg, i);
        if (!k) continue;
        char full[512];
        tagfs_format_tag(full, sizeof(full), k, v);
        if (tag_match(tag, full)) return true;
    }
    for (uint16_t j = 0; j < proc->tag_overflow_count; j++) {
        uint16_t tid = proc->tag_overflow_ids[j];
        const char *k = tag_registry_key(reg, tid);
        const char *v = tag_registry_value(reg, tid);
        if (!k) continue;
        char full[512];
        tagfs_format_tag(full, sizeof(full), k, v);
        if (tag_match(tag, full)) return true;
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
    if (tid == TAGFS_INVALID_TAG_ID) {
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

    tss_set_rsp0((uint64_t)proc->kernel_stack_top);
    notify_set_kernel_rsp((uint64_t)proc->kernel_stack_top);

    uint64_t target_cr3 = proc->context.cr3;
    if (vmm_pcid_active()) target_cr3 |= (1ULL << 63);  // NOFLUSH
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
    if (!fs || !fs->registry) {
        buffer[0] = '\0';
        return 0;
    }
    TagRegistry *reg = fs->registry;

    spin_lock(&process_lock);

    size_t pos = 0;
    bool first = true;

    uint64_t bits = proc->tag_bits;
    while (bits) {
        uint16_t i = (uint16_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        const char *k = tag_registry_key(reg, i);
        const char *v = tag_registry_value(reg, i);
        if (!k) continue;

        if (!first && pos < buffer_size - 1) buffer[pos++] = ',';
        first = false;

        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;

        if (vlen > 0) {
            if (pos + klen + 1 + vlen >= buffer_size) break;
            memcpy(buffer + pos, k, klen); pos += klen;
            buffer[pos++] = ':';
            memcpy(buffer + pos, v, vlen); pos += vlen;
        } else {
            if (pos + klen >= buffer_size) break;
            memcpy(buffer + pos, k, klen); pos += klen;
        }
    }

    for (uint16_t j = 0; j < proc->tag_overflow_count; j++) {
        uint16_t tid = proc->tag_overflow_ids[j];
        const char *k = tag_registry_key(reg, tid);
        const char *v = tag_registry_value(reg, tid);
        if (!k) continue;

        if (!first && pos < buffer_size - 1) buffer[pos++] = ',';
        first = false;

        size_t klen = strlen(k);
        size_t vlen = v ? strlen(v) : 0;

        if (vlen > 0) {
            if (pos + klen + 1 + vlen >= buffer_size) break;
            memcpy(buffer + pos, k, klen); pos += klen;
            buffer[pos++] = ':';
            memcpy(buffer + pos, v, vlen); pos += vlen;
        } else {
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

    spin_lock(&g_cleanup_queue.lock);

    if (g_cleanup_queue.count >= PROCESS_CLEANUP_QUEUE_SIZE)
    {
        spin_unlock(&g_cleanup_queue.lock);
        return false;
    }

    g_cleanup_queue.queue[g_cleanup_queue.tail] = proc;
    g_cleanup_queue.tail = (g_cleanup_queue.tail + 1) % PROCESS_CLEANUP_QUEUE_SIZE;
    g_cleanup_queue.count++;

    spin_unlock(&g_cleanup_queue.lock);
    return true;
}

static process_t *cleanup_queue_dequeue(void)
{
    spin_lock(&g_cleanup_queue.lock);

    if (g_cleanup_queue.count == 0)
    {
        spin_unlock(&g_cleanup_queue.lock);
        return NULL;
    }

    process_t *proc = g_cleanup_queue.queue[g_cleanup_queue.head];
    g_cleanup_queue.head = (g_cleanup_queue.head + 1) % PROCESS_CLEANUP_QUEUE_SIZE;
    g_cleanup_queue.count--;

    spin_unlock(&g_cleanup_queue.lock);
    return proc;
}

static void process_cleanup_immediate(process_t *proc)
{
    if (!proc)
        return;

    if (proc->tag_overflow_ids) {
        kfree(proc->tag_overflow_ids);
        proc->tag_overflow_ids = NULL;
        proc->tag_overflow_count = 0;
        proc->tag_overflow_capacity = 0;
    }

    // Free dynamically allocated FPU state buffer
    if (proc->context.fpu_state) {
        kfree(proc->context.fpu_state);
        proc->context.fpu_state = NULL;
    }

    if (proc->kernel_stack_guard_base)
    {
        uintptr_t guard_virt = (uintptr_t)proc->kernel_stack_guard_base;

        vmm_context_t *kernel_ctx = vmm_get_kernel_context();
        pte_t *guard_pte = vmm_get_pte(kernel_ctx, guard_virt);
        if (guard_pte)
        {
            *guard_pte = vmm_make_pte(guard_virt, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
            vmm_flush_tlb_page(guard_virt);
        }

        uintptr_t stack_phys = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);
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
    const uint32_t MAX_PER_CALL = 8; // throttle to avoid long latency spikes

    while (cleaned < MAX_PER_CALL)
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
                 g_cleanup_queue.count);

    uint32_t total_cleaned = 0;

    while (g_cleanup_queue.count > 0)
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
