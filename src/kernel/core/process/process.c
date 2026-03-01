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

static process_t* process_list_head = NULL;
static volatile uint32_t process_count = 0;
static spinlock_t process_lock;
static process_cleanup_queue_t g_cleanup_queue;

static bool cleanup_queue_enqueue(process_t* proc);
static process_t* cleanup_queue_dequeue(void);
static void process_cleanup_immediate(process_t* proc);

void process_init(void) {
    pid_allocator_init();

    process_list_head = NULL;
    process_count = 0;
    spinlock_init(&process_lock);

    g_cleanup_queue.head = 0;
    g_cleanup_queue.tail = 0;
    g_cleanup_queue.count = 0;
    spinlock_init(&g_cleanup_queue.lock);

    debug_printf("[PROCESS] Process management initialized\n");
    debug_printf("[PROCESS] Max processes: %u\n", PROCESS_MAX_COUNT);
    debug_printf("[PROCESS] Cabin layout: 0x%lx-0x%lx (NULL trap), 0x%lx (Notify), 0x%lx (Result), 0x%lx+ (Code)\n",
                 CABIN_NULL_TRAP_START, CABIN_NULL_TRAP_END,
                 CABIN_NOTIFY_PAGE_ADDR, CABIN_RESULT_PAGE_ADDR, CABIN_CODE_START_ADDR);
    debug_printf("[PROCESS] Deferred cleanup queue initialized (max_size=%u)\n",
                 PROCESS_CLEANUP_QUEUE_SIZE);
}

process_t* process_create(const char* tags) {
    if (process_count >= PROCESS_MAX_COUNT) {
        debug_printf("[PROCESS] ERROR: Process limit reached (%u)\n", PROCESS_MAX_COUNT);
        return NULL;
    }

    process_t* proc = kmalloc(sizeof(process_t));
    if (!proc) {
        debug_printf("[PROCESS] ERROR: Failed to allocate process structure\n");
        return NULL;
    }

    memset(proc, 0, sizeof(process_t));
    proc->magic = PROCESS_MAGIC;

    uint64_t notify_phys = 0;
    uint64_t result_phys = 0;
    vmm_context_t* cabin = vmm_create_cabin(&notify_phys, &result_phys);
    if (!cabin) {
        debug_printf("[PROCESS] ERROR: Failed to create cabin\n");
        kfree(proc);
        return NULL;
    }

    proc->pid = pid_alloc();
    if (proc->pid == PID_INVALID) {
        debug_printf("[PROCESS] ERROR: PID allocation failed (exhaustion at %u/%u)\n",
                     pid_allocated_count(), PID_MAX_COUNT);
        vmm_destroy_context(cabin);
        kfree(proc);
        return NULL;
    }

    proc->cabin = cabin;
    proc->notify_page_phys = notify_phys;
    proc->result_page_phys = result_phys;
    proc->score = 0;
    proc->result_there = false;
    proc->last_run_time = 0;
    proc->consecutive_runs = 0;
    proc->total_cpu_time = 0;
    proc->result_overflow_count = 0;
    proc->result_overflow_flag = 0;
    proc->state = PROC_NEW;
    spinlock_init(&proc->state_lock);
    proc->ref_count = 0;
    proc->code_start = VMM_CABIN_CODE_START;
    proc->code_size = 0;
    proc->started = false;
    proc->next = NULL;

    if (tags) {
        strncpy(proc->tags, tags, PROCESS_TAG_SIZE - 1);
        proc->tags[PROCESS_TAG_SIZE - 1] = '\0';
    } else {
        proc->tags[0] = '\0';
    }

    proc->block_reason = PROC_BLOCK_NONE;
    proc->next_blocked = NULL;
    proc->block_start_time = 0;

    // kernel stack layout: [guard page (unmapped)] [data pages]
    size_t kernel_stack_size = CONFIG_KERNEL_STACK_PAGES * VMM_PAGE_SIZE;
    size_t total_pages = CONFIG_KERNEL_STACK_TOTAL_PAGES;

    void* stack_phys = pmm_alloc(total_pages);
    if (!stack_phys) {
        vmm_destroy_context(cabin);
        pid_free(proc->pid);
        kfree(proc);
        return NULL;
    }

    void* stack_virt_base = vmm_phys_to_virt((uintptr_t)stack_phys);

    vmm_context_t* kernel_ctx = vmm_get_kernel_context();

    // guard page must be unmapped so a stack overflow raises a page fault
    pte_t* guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)stack_virt_base);
    if (!guard_pte) {
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
    proc->kernel_stack = (void*)((uintptr_t)stack_virt_base + VMM_PAGE_SIZE);
    proc->kernel_stack_top = (void*)((uintptr_t)proc->kernel_stack + kernel_stack_size);

    debug_printf("[PROCESS] Kernel stack allocated: guard=0x%lx, stack=0x%lx-0x%lx (PID %u)\n",
                 (uintptr_t)proc->kernel_stack_guard_base,
                 (uintptr_t)proc->kernel_stack,
                 (uintptr_t)proc->kernel_stack_top,
                 proc->pid);

    memset(&proc->context, 0, sizeof(ProcessContext));
    proc->context.cr3 = proc->cabin->pml4_phys;
    proc->context.rflags = 0x202;
    proc->context.cs = GDT_USER_CODE;
    proc->context.ds = GDT_USER_DATA;
    proc->context.es = GDT_USER_DATA;
    proc->context.fs = GDT_USER_DATA;
    proc->context.gs = GDT_USER_DATA;
    proc->context.ss = GDT_USER_DATA;

    fpu_init_state(proc->context.fpu_state);
    proc->context.fpu_initialized = true;

    spin_lock(&process_lock);
    proc->next = process_list_head;
    process_list_head = proc;
    process_count++;
    spin_unlock(&process_lock);

    return proc;
}

void process_ref_inc(process_t* proc) {
    if (!proc) {
        return;
    }

    uint32_t old = atomic_fetch_add_u32(&proc->ref_count, 1);

    #ifdef DEBUG_REFCOUNT
    debug_printf("[REFCOUNT] PID %u: %u -> %u (INC)\n", proc->pid, old, old + 1);
    #endif

    if (old > UINT32_MAX - 100) {
        debug_printf("[PROCESS] PANIC: ref_count overflow for PID %u (old=%u)\n",
                     proc->pid, old);
        while (1) { asm volatile("cli; hlt"); }
    }
}

void process_ref_dec(process_t* proc) {
    if (!proc) return;

    // fetch_sub returns OLD value (before subtraction)
    uint32_t old = atomic_fetch_sub_u32(&proc->ref_count, 1);

    if (old == 0) {
        debug_printf("[PROCESS] PANIC: ref_count underflow for PID %u (old=0)\n",
                     proc->pid);
        while (1) { asm volatile("cli; hlt"); }
    }

    #ifdef DEBUG_REFCOUNT
    debug_printf("[REFCOUNT] PID %u: %u -> %u (DEC)\n", proc->pid, old, old - 1);
    #endif
}

uint32_t process_ref_count(process_t* proc) {
    if (!proc) return 0;
    return atomic_load_u32(&proc->ref_count);
}

void process_set_state(process_t* proc, process_state_t new_state) {
    if (!proc) return;
    spin_lock(&proc->state_lock);
    proc->state = new_state;
    spin_unlock(&proc->state_lock);
}

process_state_t process_get_state(process_t* proc) {
    if (!proc) return PROC_TERMINATED;
    spin_lock(&proc->state_lock);
    process_state_t state = proc->state;
    spin_unlock(&proc->state_lock);
    return state;
}

int process_destroy_safe(process_t* proc) {
    if (!proc) {
        return -1;
    }

    process_state_t state = process_get_state(proc);
    if (state != PROC_TERMINATED && state != PROC_ZOMBIE) {
        debug_printf("[PROCESS] ERROR: Cannot destroy PID %u in state %d\n",
                     proc->pid, state);
        return -1;
    }

    uint32_t refs = process_ref_count(proc);
    if (refs > 0) {
        debug_printf("[PROCESS] ERROR: Cannot destroy PID %u with ref_count=%u\n",
                     proc->pid, refs);
        return -1;
    }

    process_destroy(proc);
    return 0;
}

void process_destroy(process_t* proc) {
    if (!proc) return;

    if (proc->magic != PROCESS_MAGIC) {
        debug_printf("[PROCESS] CORRUPTION: Invalid magic 0x%x for PID %u\n",
                     proc->magic, proc->pid);
        while (1) { asm volatile("cli; hlt"); }
    }

    scheduler_state_t* sched = scheduler_get_state();

    // check current_process under short lock
    spin_lock(&sched->scheduler_lock);
    if (sched->current_process == proc) {
        spin_unlock(&sched->scheduler_lock);
        debug_printf("[PROCESS] ERROR: Cannot destroy current process PID %u\n", proc->pid);
        return;
    }
    spin_unlock(&sched->scheduler_lock);
    // scheduler_lock is now FREE — no deadlock possible below

    spin_lock(&process_lock);

    uint32_t refs = process_ref_count(proc);
    if (refs > 0) {
        spin_unlock(&process_lock);
        debug_printf("[PROCESS] PID %u has ref_count=%u, transitioning to ZOMBIE\n",
                     proc->pid, refs);
        process_set_state(proc, PROC_ZOMBIE);
        return;
    }

    // poison magic before removing from list to catch use-after-remove
    proc->magic = 0xDEADDEAD;

    if (process_list_head == proc) {
        process_list_head = proc->next;
    } else {
        process_t* curr = process_list_head;
        while (curr && curr->next != proc) {
            curr = curr->next;
        }
        if (curr) {
            curr->next = proc->next;
        }
    }

    process_count--;

    spin_unlock(&process_lock);

    process_set_state(proc, PROC_TERMINATED);

    uint32_t cancelled = async_io_cancel_by_pid(proc->pid);
    if (cancelled > 0) {
        debug_printf("[PROCESS] Cancelled %u pending async I/O for PID %u\n",
                     cancelled, proc->pid);
    }

    if (!cleanup_queue_enqueue(proc)) {
        // queue full — fall back to immediate cleanup
        debug_printf("[PROCESS] WARNING: Cleanup queue full, immediate cleanup for PID %u\n",
                     proc->pid);
        process_cleanup_immediate(proc);
    }
    // resources (PMM/VMM) are NOT freed here; deferred to process_cleanup_deferred()
}

int process_load_binary(process_t* proc, const void* binary_data, size_t size) {
    if (!proc || !binary_data || size == 0) {
        debug_printf("[PROCESS] ERROR: Invalid arguments to process_load_binary\n");
        return -1;
    }

    if (!proc->cabin) {
        debug_printf("[PROCESS] ERROR: Process has no cabin\n");
        return -1;
    }

    size_t page_count = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;

    // minimum 5 pages for cabin layout (.text: 3, .rodata: 1, .data: 1)
    if (page_count < 5) {
        page_count = 5;
    }

    void* code_phys = pmm_alloc(page_count);
    if (!code_phys) {
        debug_printf("[PROCESS] ERROR: Failed to allocate %zu pages for binary\n", page_count);
        return -1;
    }

    void* code_virt = vmm_phys_to_virt((uintptr_t)code_phys);
    memcpy(code_virt, binary_data, size);

    int result = vmm_map_code_region(proc->cabin, (uintptr_t)code_phys, page_count * VMM_PAGE_SIZE);
    if (result != 0) {
        debug_printf("[PROCESS] ERROR: Failed to map code region\n");
        debug_printf("[PROCESS] Physical address: 0x%lx, Size: %zu bytes\n",
                     (uintptr_t)code_phys, size);
        pmm_free(code_phys, page_count);
        return -1;
    }

    void* user_stack_phys = pmm_alloc(CONFIG_USER_STACK_TOTAL_PAGES);
    if (!user_stack_phys) {
        debug_printf("[PROCESS] ERROR: Failed to allocate user stack\n");
        pmm_free(code_phys, page_count);
        return -1;
    }

    // stack layout: [guard page][...data pages...]
    // only map data pages; guard page is left unmapped for overflow detection
    uint64_t guard_page_base = VMM_USER_STACK_TOP - (CONFIG_USER_STACK_TOTAL_PAGES * VMM_PAGE_SIZE);
    uint64_t stack_data_base = guard_page_base + (CONFIG_USER_STACK_GUARD_PAGES * VMM_PAGE_SIZE);

    vmm_map_result_t map_result = vmm_map_pages(
        proc->cabin,
        stack_data_base,
        (uintptr_t)user_stack_phys + (CONFIG_USER_STACK_GUARD_PAGES * VMM_PAGE_SIZE),
        CONFIG_USER_STACK_PAGES,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER
    );

    if (!map_result.success) {
        debug_printf("[PROCESS] ERROR: Failed to map user stack: %s\n", map_result.error_msg);
        pmm_free(user_stack_phys, CONFIG_USER_STACK_TOTAL_PAGES);
        pmm_free(code_phys, page_count);
        return -1;
    }

    proc->code_size = size;
    proc->context.rip = VMM_CABIN_CODE_START;
    proc->context.rsp = VMM_USER_STACK_TOP;

    return 0;
}

void process_list_validate(const char* caller) {
    spin_lock(&process_lock);

    uint32_t count = 0;
    process_t* curr = process_list_head;
    process_t** seen = (process_t**)kmalloc(PROCESS_MAX_COUNT * sizeof(process_t*));
    if (!seen) {
        debug_printf("[PROCESS] WARNING: kmalloc failed in process_list_validate\n");
        spin_unlock(&process_lock);
        return;
    }

    while (curr && count < PROCESS_MAX_COUNT) {
        for (uint32_t i = 0; i < count; i++) {
            if (seen[i] == curr) {
                debug_printf("[PROCESS] CORRUPTION: Cycle detected at %p (caller: %s)\n",
                             (void*)curr, caller);
                debug_printf("[PROCESS] Process list is corrupted\n");
                kfree(seen);
                while (1) { asm volatile("cli; hlt"); }
            }
        }

        if (curr->magic != PROCESS_MAGIC) {
            debug_printf("[PROCESS] CORRUPTION: Invalid magic 0x%x at %p (caller: %s)\n",
                         curr->magic, (void*)curr, caller);
            debug_printf("[PROCESS] PID: %u\n", curr->pid);
            kfree(seen);
            while (1) { asm volatile("cli; hlt"); }
        }

        seen[count++] = curr;
        curr = curr->next;
    }

    if (count != process_count) {
        debug_printf("[PROCESS] CORRUPTION: Count mismatch (list=%u, expected=%u, caller=%s)\n",
                     count, process_count, caller);
        kfree(seen);
        while (1) { asm volatile("cli; hlt"); }
    }

    kfree(seen);
    spin_unlock(&process_lock);
}

process_t* process_find(uint32_t pid) {
    if (pid == PROCESS_INVALID_PID) {
        return NULL;
    }

    spin_lock(&process_lock);

    process_t* curr = process_list_head;
    while (curr) {
        if (curr->pid == pid) {
            spin_unlock(&process_lock);
            return curr;
        }
        curr = curr->next;
    }

    spin_unlock(&process_lock);
    return NULL;
}

process_t* process_get_first(void) {
    return process_list_head;
}

process_t* process_get_current(void) {
    scheduler_state_t* sched = scheduler_get_state();
    if (!sched) return NULL;

    spin_lock(&sched->scheduler_lock);
    process_t* current = sched->current_process;
    spin_unlock(&sched->scheduler_lock);

    return current;
}

uint32_t process_get_count(void) {
    return atomic_load_u32(&process_count);
}

void process_test(void) {
    kprintf("\n");
    kprintf("====================================\n");
    kprintf("PROCESS MANAGEMENT TEST\n");
    kprintf("====================================\n");

    debug_printf("[TEST] Creating test process...\n");
    process_t* proc = process_create("app test");

    if (!proc) {
        debug_printf("[TEST] FAILED: Could not create process\n");
        return;
    }

    debug_printf("[TEST] SUCCESS: Process created\n");
    debug_printf("[TEST]   PID: %u\n", proc->pid);
    debug_printf("[TEST]   State: %s\n", proc->state == PROC_NEW ? "NEW" : "UNKNOWN");
    debug_printf("[TEST]   Cabin: %p\n", proc->cabin);
    debug_printf("[TEST]   Notify page (phys): 0x%lx\n", proc->notify_page_phys);
    debug_printf("[TEST]   Result page (phys): 0x%lx\n", proc->result_page_phys);
    debug_printf("[TEST]   Code start (virt): 0x%lx\n", proc->code_start);
    debug_printf("[TEST]   Tags: %s\n", proc->tags);

    uint8_t test_binary[64];
    for (int i = 0; i < 64; i++) {
        test_binary[i] = (uint8_t)i;
    }

    debug_printf("[TEST] Loading test binary (64 bytes)...\n");
    int result = process_load_binary(proc, test_binary, sizeof(test_binary));

    if (result != 0) {
        debug_printf("[TEST] FAILED: Could not load binary\n");
        process_destroy(proc);
        return;
    }

    debug_printf("[TEST] SUCCESS: Binary loaded\n");
    debug_printf("[TEST]   State: %s\n", proc->state == PROC_READY ? "READY" : "UNKNOWN");
    debug_printf("[TEST]   Code size: %zu bytes\n", proc->code_size);

    debug_printf("[TEST] Verifying process lookup...\n");
    process_t* found = process_find(proc->pid);
    if (found == proc) {
        debug_printf("[TEST] SUCCESS: Process lookup working\n");
    } else {
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

bool process_has_tag(process_t* proc, const char* tag) {
    if (!proc || !tag || tag[0] == '\0') {
        return false;
    }

    size_t tag_len = strlen(tag);
    const char* tags = proc->tags;
    const char* pos = tags;

    while (*pos) {
        const char* comma = strchr(pos, ',');
        size_t current_len = comma ? (size_t)(comma - pos) : strlen(pos);

        if (current_len == tag_len && strncmp(pos, tag, tag_len) == 0) {
            return true;
        }

        if (!comma) {
            break;
        }
        pos = comma + 1;
    }

    return false;
}

int process_add_tag(process_t* proc, const char* tag) {
    if (!proc || !tag || tag[0] == '\0') {
        return -1;
    }

    size_t tag_len = strlen(tag);
    if (tag_len >= PROCESS_TAG_SIZE) {
        return -1;
    }

    spin_lock(&process_lock);

    if (process_has_tag(proc, tag)) {
        spin_unlock(&process_lock);
        return 0;
    }

    size_t current_len = strlen(proc->tags);

    if (current_len + tag_len + 2 > PROCESS_TAG_SIZE) {
        spin_unlock(&process_lock);
        return -1;
    }

    if (current_len > 0) {
        strcat(proc->tags, ",");
    }
    strcat(proc->tags, tag);

    spin_unlock(&process_lock);

    return 0;
}

int process_remove_tag(process_t* proc, const char* tag) {
    if (!proc || !tag || tag[0] == '\0') {
        return -1;
    }

    spin_lock(&process_lock);

    if (!process_has_tag(proc, tag)) {
        spin_unlock(&process_lock);
        return 0;
    }

    char new_tags[PROCESS_TAG_SIZE];
    new_tags[0] = '\0';

    const char* pos = proc->tags;
    bool first = true;

    while (*pos) {
        const char* comma = strchr(pos, ',');
        size_t current_len = comma ? (size_t)(comma - pos) : strlen(pos);

        if (current_len != strlen(tag) || strncmp(pos, tag, current_len) != 0) {
            if (!first) {
                size_t remaining = PROCESS_TAG_SIZE - strlen(new_tags) - 1;
                if (remaining < 1) {
                    debug_printf("[PROCESS] ERROR: Tag string overflow during removal (comma)\n");
                    spin_unlock(&process_lock);
                    return -1;
                }
                strncat(new_tags, ",", remaining);
            }
            size_t remaining = PROCESS_TAG_SIZE - strlen(new_tags) - 1;
            if (current_len > remaining) {
                debug_printf("[PROCESS] ERROR: Tag string overflow during removal (tag)\n");
                spin_unlock(&process_lock);
                return -1;
            }
            strncat(new_tags, pos, current_len < remaining ? current_len : remaining);
            first = false;
        }

        if (!comma) {
            break;
        }
        pos = comma + 1;
    }

    strncpy(proc->tags, new_tags, PROCESS_TAG_SIZE - 1);
    proc->tags[PROCESS_TAG_SIZE - 1] = '\0';

    spin_unlock(&process_lock);

    return 0;
}

void process_start_initial(process_t* proc) {
    if (!proc) {
        debug_printf("[PROCESS] PANIC: process_start_initial called with NULL process\n");
        while (1) { asm volatile("cli; hlt"); }
    }

    if (proc->state != PROC_READY) {
        debug_printf("[PROCESS] PANIC: process_start_initial called with process not in READY state\n");
        debug_printf("[PROCESS]   PID %u is in state %d (expected PROC_READY=%d)\n",
                proc->pid, proc->state, PROC_READY);
        while (1) { asm volatile("cli; hlt"); }
    }

    if (proc->started) {
        debug_printf("[PROCESS] PANIC: process_start_initial called on process that already ran\n");
        debug_printf("[PROCESS]   PID %u has started=true\n", proc->pid);
        while (1) { asm volatile("cli; hlt"); }
    }

    process_set_state(proc, PROC_RUNNING);

    scheduler_state_t* sched = scheduler_get_state();
    spin_lock(&sched->scheduler_lock);
    sched->current_process = proc;
    spin_unlock(&sched->scheduler_lock);

    asm volatile("cli");

    tss_set_rsp0((uint64_t)proc->kernel_stack_top);

    __asm__ volatile("mov %0, %%cr3" : : "r"(proc->context.cr3) : "memory");

    uint64_t verify_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(verify_cr3));
    if (verify_cr3 != proc->context.cr3) {
        debug_printf("[PROCESS] PANIC: CR3 verification failed (wrote 0x%lx, read 0x%lx)\n",
                proc->context.cr3, verify_cr3);
        while (1) { asm volatile("cli; hlt"); }
    }

    jump_to_userspace(proc->context.rip, proc->context.rsp, proc->context.rflags);

    debug_printf("[PROCESS] PANIC: jump_to_userspace returned (should never happen)\n");
    while (1) { asm volatile("cli; hlt"); }
}

size_t process_snapshot_tags(process_t* proc, char* buffer, size_t buffer_size) {
    if (!proc || !buffer || buffer_size == 0) {
        return 0;
    }

    spin_lock(&process_lock);

    size_t len = strlen(proc->tags);
    size_t copy_len = (len < buffer_size - 1) ? len : buffer_size - 1;

    memcpy(buffer, proc->tags, copy_len);
    buffer[copy_len] = '\0';

    spin_unlock(&process_lock);

    return copy_len;
}

static bool cleanup_queue_enqueue(process_t* proc) {
    if (!proc) return false;

    spin_lock(&g_cleanup_queue.lock);

    if (g_cleanup_queue.count >= PROCESS_CLEANUP_QUEUE_SIZE) {
        spin_unlock(&g_cleanup_queue.lock);
        return false;
    }

    g_cleanup_queue.queue[g_cleanup_queue.tail] = proc;
    g_cleanup_queue.tail = (g_cleanup_queue.tail + 1) % PROCESS_CLEANUP_QUEUE_SIZE;
    g_cleanup_queue.count++;

    spin_unlock(&g_cleanup_queue.lock);
    return true;
}

static process_t* cleanup_queue_dequeue(void) {
    spin_lock(&g_cleanup_queue.lock);

    if (g_cleanup_queue.count == 0) {
        spin_unlock(&g_cleanup_queue.lock);
        return NULL;
    }

    process_t* proc = g_cleanup_queue.queue[g_cleanup_queue.head];
    g_cleanup_queue.head = (g_cleanup_queue.head + 1) % PROCESS_CLEANUP_QUEUE_SIZE;
    g_cleanup_queue.count--;

    spin_unlock(&g_cleanup_queue.lock);
    return proc;
}

static void process_cleanup_immediate(process_t* proc) {
    if (!proc) return;

    if (proc->kernel_stack_guard_base) {
        uintptr_t guard_virt = (uintptr_t)proc->kernel_stack_guard_base;

        vmm_context_t* kernel_ctx = vmm_get_kernel_context();
        pte_t* guard_pte = vmm_get_pte(kernel_ctx, guard_virt);
        if (guard_pte) {
            *guard_pte = vmm_make_pte(guard_virt, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
            vmm_flush_tlb_page(guard_virt);
        }

        uintptr_t stack_phys = vmm_virt_to_phys_direct(proc->kernel_stack_guard_base);
        pmm_free((void*)stack_phys, CONFIG_KERNEL_STACK_TOTAL_PAGES);

        debug_printf("[PROCESS] Freed kernel stack: guard=0x%lx (PID %u)\n",
                     guard_virt, proc->pid);

        proc->kernel_stack_guard_base = NULL;
        proc->kernel_stack = NULL;
    }

    if (proc->cabin) {
        vmm_destroy_context(proc->cabin);
        proc->cabin = NULL;
    }

    pid_free(proc->pid);
    proc->pid = PID_INVALID;

    kfree(proc);
}

void process_cleanup_deferred(void) {
    uint32_t cleaned = 0;
    const uint32_t MAX_PER_CALL = 8;  // throttle to avoid long latency spikes

    while (cleaned < MAX_PER_CALL) {
        process_t* proc = cleanup_queue_dequeue();
        if (!proc) break;

        process_cleanup_immediate(proc);
        cleaned++;
    }

    if (cleaned > 0) {
        debug_printf("[PROCESS] Deferred cleanup: freed %u processes\n", cleaned);
    }
}

uint32_t process_cleanup_queue_size(void) {
    spin_lock(&g_cleanup_queue.lock);
    uint32_t size = g_cleanup_queue.count;
    spin_unlock(&g_cleanup_queue.lock);
    return size;
}

void process_cleanup_queue_flush(void) {
    debug_printf("[PROCESS] Flushing cleanup queue (pending=%u)...\n",
                 g_cleanup_queue.count);

    uint32_t total_cleaned = 0;

    while (g_cleanup_queue.count > 0) {
        process_t* proc = cleanup_queue_dequeue();
        if (!proc) break;

        process_cleanup_immediate(proc);
        total_cleaned++;
    }

    debug_printf("[PROCESS] Cleanup queue flushed (%u processes freed)\n",
                 total_cleaned);
}
