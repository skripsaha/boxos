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
    WAIT_RING_FULL,
    WAIT_IO,
    WAIT_OVERFLOW
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
    // fxsave requires 16-byte alignment; kmalloc does not guarantee it.
    // fpu_save/fpu_restore compute the aligned pointer at runtime via fpu_align().
    // Buffer is 528 bytes (512 + 16) to allow runtime alignment adjustment.
    uint8_t fpu_state[512 + 16];
    bool fpu_initialized;
} ProcessContext;

// These offsets must match context_switch.asm — if the struct layout changes,
// update the %define constants in context_switch.asm to match.
_Static_assert(offsetof(ProcessContext, rip) == 128, "ProcessContext.rip offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, cs) == 136, "ProcessContext.cs offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, rflags) == 152, "ProcessContext.rflags offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_state) == 168, "ProcessContext.fpu_state offset mismatch with asm");
_Static_assert(offsetof(ProcessContext, fpu_initialized) == 696, "ProcessContext.fpu_initialized offset mismatch with asm");

typedef struct process_t {
    uint32_t magic;
    uint32_t pid;
    vmm_context_t* cabin;
    uint64_t notify_page_phys;
    uint64_t result_page_phys;

    int32_t score;
    volatile bool result_there;       // set from IRQ context
    uint64_t last_run_time;
    uint32_t consecutive_runs;
    uint64_t total_cpu_time;

    volatile uint32_t result_overflow_count;
    volatile uint8_t result_overflow_flag; // uint8 for atomic consistency

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
    struct process_t* next_waiting;   // wait queue linkage (EventRing overflow)
    uint64_t wait_start_time;         // TSC timestamp when waiting (0 = not waiting)

    struct process_t* next;
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
