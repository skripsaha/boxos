#ifndef PROCESS_H
#define PROCESS_H

#include "ktypes.h"
#include "vmm.h"
#include "boxos_magic.h"
#include "boxos_limits.h"

/*
 * Lock ordering:
 *   1. scheduler_lock  (scheduler.c) — acquired BRIEFLY only to check current_process
 *   2. process_lock    (process.c)   — global list protection
 *   3. cleanup_queue.lock            — deferred cleanup queue
 *   4. process_t->state_lock         — per-process state
 *
 * process_destroy() releases scheduler_lock before any resource cleanup.
 * scheduler_cleanup_finished() releases scheduler_lock before calling process_destroy().
 * process_cleanup_deferred() runs without scheduler_lock or process_lock.
 */

#define PROCESS_MAX_COUNT       MAX_PROCESSES
#define PROCESS_TAG_SIZE        64
#define PROCESS_INVALID_PID     0

struct process_t;

typedef enum {
    WAIT_NONE = 0,
    WAIT_RESULT,
    WAIT_IO
} wait_reason_t;

#define PROCESS_CLEANUP_QUEUE_SIZE 512

typedef struct {
    struct process_t* queue[PROCESS_CLEANUP_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    spinlock_t lock;
} process_cleanup_queue_t;

typedef enum {
    PROC_CREATED = 0,
    PROC_WORKING,
    PROC_WAITING,
    PROC_STOPPED,
    PROC_DONE,
    PROC_CRASHED
} process_state_t;

typedef struct {
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
    uint8_t* fpu_state;
    bool fpu_initialized;
} ProcessContext;

// These offsets must match context_switch.asm — if the struct layout changes,
// update the %define constants in context_switch.asm to match.
_Static_assert(offsetof(ProcessContext, rip) == 128, "ProcessContext.rip offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, cs) == 136, "ProcessContext.cs offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, rflags) == 152, "ProcessContext.rflags offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_state) == 168, "ProcessContext.fpu_state offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_initialized) == 176, "ProcessContext.fpu_initialized offset mismatch with asm");

typedef struct process_t {
    uint32_t magic;
    uint32_t pid;
    vmm_context_t* cabin;
    uint64_t cabin_info_phys;
    uint64_t pocket_ring_phys;
    uint64_t result_ring_phys;

    int32_t score;
    uint64_t last_run_time;
    uint32_t consecutive_runs;
    uint64_t total_cpu_time;

    volatile process_state_t state;
    spinlock_t state_lock;

    volatile uint32_t ref_count;

    char tags[PROCESS_TAG_SIZE];

    uintptr_t code_start;
    size_t code_size;

    ProcessContext context;
    void* kernel_stack;
    void* kernel_stack_top;
    void* kernel_stack_guard_base;

    bool started;
    uint32_t spawner_pid;
    uint64_t buf_heap_next;  // next free virtual address for buffer mapping

    // ASLR: per-process randomized addresses (set at creation time)
    uint64_t aslr_heap_base;     // actual heap start (CABIN_HEAP_BASE + random)
    uint64_t aslr_stack_top;     // actual stack top (USER_STACK_TOP - random)
    uint64_t aslr_buf_heap_base; // actual buffer heap start

    volatile wait_reason_t wait_reason;
    uint64_t wait_start_time;         // TSC timestamp when waiting (0 = not waiting)

    struct process_t* hash_next;  // hash table collision chain
    struct process_t* next;       // global process list
} process_t;

_Static_assert(sizeof(process_t) < 4096, "Process structure must fit in one page");

void process_init(void);

process_t* process_create(const char* tags);
void process_destroy(process_t* proc);

int process_load_binary(process_t* proc, const void* binary_data, size_t size);

process_t* process_find(uint32_t pid);
process_t* process_find_ref(uint32_t pid);  // returns ref-counted pointer, caller must process_ref_dec()
process_t* process_get_first(void);
process_t* process_get_current(void);

// Lock/unlock the global process list for safe iteration.
// Caller must hold process_list_lock while iterating via proc->next.
// The spinlock disables IRQs, so this is safe from any context.
void process_list_lock(void);
void process_list_unlock(void);
bool spin_trylock_process_list(void);

uint32_t process_get_count(void);

void process_test(void);

void process_start_initial(process_t* proc);

bool process_has_tag(process_t* proc, const char* tag);
int process_add_tag(process_t* proc, const char* tag);
int process_remove_tag(process_t* proc, const char* tag);
size_t process_snapshot_tags(process_t* proc, char* buffer, size_t buffer_size);

void process_ref_inc(process_t* proc);
void process_ref_dec(process_t* proc);
uint32_t process_ref_count(process_t* proc);

void process_set_state(process_t* proc, process_state_t new_state);
process_state_t process_get_state(process_t* proc);

int process_destroy_safe(process_t* proc);

void process_list_validate(const char* caller);

void process_cleanup_deferred(void);
uint32_t process_cleanup_queue_size(void);
void process_cleanup_queue_flush(void);

#endif // PROCESS_H
