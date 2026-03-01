#ifndef PROCESS_H
#define PROCESS_H

#include "ktypes.h"
#include "vmm.h"
#include "boxos_magic.h"
#include "boxos_limits.h"

/*
 * LOCK ORDERING HIERARCHY (CRITICAL-5 fix):
 *
 * 1. scheduler_lock       (scheduler/scheduler.c)
 * 2. process_lock         (process.c, global list protection)
 * 3. cleanup_queue_lock   (process.c, deferred cleanup queue)
 * 4. process_t->state_lock (per-process state)
 *
 * CRITICAL RULES:
 * - process_destroy() acquires scheduler_lock BRIEFLY (only to check current_process)
 * - process_destroy() NEVER holds scheduler_lock during resource cleanup
 * - scheduler_reap_zombies() releases scheduler_lock BEFORE calling process_destroy()
 * - Deferred cleanup (process_cleanup_deferred) operates WITHOUT scheduler_lock/process_lock
 *
 * This ordering prevents the CRITICAL-5 deadlock where scheduler_reap_zombies()
 * held scheduler_lock and called process_destroy() which tried to re-acquire it.
 */

#define PROCESS_MAX_COUNT       BOXOS_MAX_PROCESSES
#define PROCESS_TAG_SIZE        64
#define PROCESS_INVALID_PID     0
#define PROCESS_MAGIC           BOXOS_PROCESS_MAGIC  // "PROC" in ASCII

// Forward declaration
struct process_t;

typedef enum {
    PROC_BLOCK_NONE = 0,
    PROC_BLOCK_WAITING_RESULT,
    PROC_BLOCK_EVENT_RING_FULL,
    PROC_BLOCK_IO,
    PROC_BLOCK_RESULT_OVERFLOW
} process_block_reason_t;

// Deferred cleanup queue for process destruction
#define PROCESS_CLEANUP_QUEUE_SIZE 256

typedef struct {
    struct process_t* queue[PROCESS_CLEANUP_QUEUE_SIZE];
    uint32_t head;        // Read index
    uint32_t tail;        // Write index
    uint32_t count;       // Current size
    spinlock_t lock;      // Short-lived lock
} process_cleanup_queue_t;

typedef enum {
    PROC_NEW = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE,
    PROC_TERMINATED
} process_state_t;

typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;

    uint64_t rip;

    uint16_t cs, ds, es, fs, gs, ss;

    uint64_t rflags;

    uint64_t cr3;

    // FPU/SSE state saved by fxsave (512 bytes + 16 for runtime alignment).
    // kmalloc does not guarantee 16-byte alignment, so fpu_save/fpu_restore
    // compute the aligned pointer at runtime via fpu_align().
    uint8_t fpu_state[512 + 16];
    bool fpu_initialized;
} ProcessContext;

typedef struct process_t {
    uint32_t magic;        // Magic number for corruption detection
    uint32_t pid;
    vmm_context_t* cabin;
    uint64_t notify_page_phys;
    uint64_t result_page_phys;

    int32_t score;
    volatile bool result_there;  // Process has result waiting (accessed from IRQ)
    uint64_t last_run_time;
    uint32_t consecutive_runs;   // Counter for consecutive scheduling
    uint64_t total_cpu_time;     // Total ticks spent running

    volatile uint32_t result_overflow_count;  // Count of lost results
    volatile uint8_t result_overflow_flag;    // Sticky overflow indicator (changed from bool for atomic consistency)

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

    // Blocking state
    volatile process_block_reason_t block_reason;

    // Wait queue linkage (for EventRing overflow)
    struct process_t* next_blocked;

    // ResultRing overflow tracking
    uint64_t block_start_time;  // TSC timestamp when blocked (0 = not blocked)

    struct process_t* next;
} process_t;

// Compile-time checks
_Static_assert(sizeof(process_t) < 4096, "Process structure must fit in one page");

void process_init(void);

process_t* process_create(const char* tags);
void process_destroy(process_t* proc);

int process_load_binary(process_t* proc, const void* binary_data, size_t size);

process_t* process_find(uint32_t pid);
process_t* process_get_first(void);
process_t* process_get_current(void);

uint32_t process_get_count(void);

void process_test(void);

void process_start_initial(process_t* proc);

// Tag management functions
bool process_has_tag(process_t* proc, const char* tag);
int process_add_tag(process_t* proc, const char* tag);
int process_remove_tag(process_t* proc, const char* tag);
size_t process_snapshot_tags(process_t* proc, char* buffer, size_t buffer_size);

// Reference counting
void process_ref_inc(process_t* proc);
void process_ref_dec(process_t* proc);
uint32_t process_ref_count(process_t* proc);

// Atomic state management
void process_set_state(process_t* proc, process_state_t new_state);
process_state_t process_get_state(process_t* proc);

// Safe destruction
int process_destroy_safe(process_t* proc);

// Process list validation
void process_list_validate(const char* caller);

// Deferred cleanup API
void process_cleanup_deferred(void);
uint32_t process_cleanup_queue_size(void);  // For monitoring
void process_cleanup_queue_flush(void);  // Drain all pending cleanups (for shutdown)

#endif // PROCESS_H
