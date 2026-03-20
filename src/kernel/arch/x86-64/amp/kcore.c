#include "kcore.h"
#include "process.h"
#include "guide.h"
#include "lapic.h"
#include "irqchip.h"
#include "atomics.h"
#include "klib.h"
#include "vmm.h"
#include "perf_trace.h"
#include "amp.h"

KCorePocketQueue g_kcore_queues[MAX_CORES];

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void kcore_init(void)
{
    for (int i = 0; i < MAX_CORES; i++) {
        memset(&g_kcore_queues[i], 0, sizeof(KCorePocketQueue));
        g_kcore_queues[i].kcore_idx = (uint8_t)i;
    }
    debug_printf("[KCORE] %u K-Core queue(s) initialized (capacity=%d each)\n",
                 g_amp.k_count, KCORE_QUEUE_CAPACITY);
}

// ---------------------------------------------------------------------------
// Queue depth (approximate, for load balancing)
// ---------------------------------------------------------------------------

uint32_t kcore_queue_depth(uint8_t core_idx)
{
    return atomic_load_u32(&g_kcore_queues[core_idx].count);
}

// ---------------------------------------------------------------------------
// MPSC push (App Core → K-Core queue)
// ---------------------------------------------------------------------------

static bool kcore_queue_push(KCorePocketQueue* q, struct process_t* proc)
{
    uint32_t old_tail, new_tail;
    uint32_t spins = 0;
    for (;;) {
        old_tail = atomic_load_u32(&q->tail);
        new_tail = (old_tail + 1) & KCORE_QUEUE_MASK;

        // Full check: if new_tail would collide with head
        if (new_tail == atomic_load_u32(&q->head)) {
            return false;  // queue full
        }

        // CAS: claim the slot
        if (atomic_cas_u32(&q->tail, old_tail, new_tail)) {
            break;
        }
        // CAS failed — exponential backoff to reduce cache line bouncing
        uint32_t backoff = 1u << (spins < 4 ? spins : 4);
        for (uint32_t b = 0; b < backoff; b++) {
            cpu_pause();
        }
        spins++;
    }

    // Write the pointer into the claimed slot.
    // The K-Core consumer spins on NULL before reading.
    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);

    atomic_fetch_add_u32(&q->count, 1);
    return true;
}

// ---------------------------------------------------------------------------
// MPSC pop (K-Core consumer — single reader, no CAS needed)
// ---------------------------------------------------------------------------

static struct process_t* kcore_queue_pop(KCorePocketQueue* q)
{
    uint32_t h = q->head;
    uint32_t t = atomic_load_u32(&q->tail);
    if (h == t) {
        return NULL;  // empty
    }

    // Spin until the producer finishes writing the pointer.
    // This handles the gap between CAS-advancing tail and store-writing slot.
    struct process_t* proc;
    for (;;) {
        proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
        if (proc != NULL) break;
        cpu_pause();
    }

    q->slots[h] = NULL;
    q->head = (h + 1) & KCORE_QUEUE_MASK;
    atomic_fetch_sub_u32(&q->count, 1);
    return proc;
}

// ---------------------------------------------------------------------------
// Least-loaded K-Core selection
// ---------------------------------------------------------------------------

static uint8_t kcore_find_least_loaded(void)
{
    uint8_t best_core = g_amp.bsp_index;  // BSP is always a K-Core
    uint32_t best_depth = UINT32_MAX;

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        if (!g_amp.cores[i].is_kcore) continue;
        uint32_t depth = kcore_queue_depth(i);
        if (depth < best_depth) {
            best_depth = depth;
            best_core = i;
        }
    }
    return best_core;
}

// ---------------------------------------------------------------------------
// Submit: App Core → least-loaded K-Core
// ---------------------------------------------------------------------------

void kcore_submit(struct process_t* proc)
{
    uint8_t target = kcore_find_least_loaded();
    uint8_t my_core = amp_get_core_index();

    if (!kcore_queue_push(&g_kcore_queues[target], proc)) {
        // Queue full — fallback: try all K-Cores
        for (uint8_t i = 0; i < g_amp.total_cores; i++) {
            if (!g_amp.cores[i].is_kcore || i == target) continue;
            if (kcore_queue_push(&g_kcore_queues[i], proc)) {
                target = i;
                goto submitted;
            }
        }
        // All queues full — should not happen in practice.
        // Clear pending flag so the process can retry on next notify().
        atomic_store_u8(&proc->kcore_pending, 0);
        kprintf("[KCORE] WARNING: All K-Core queues full, dropping submit for PID %u\n",
                proc->pid);
        return;
    }

submitted:
    // Debug: show submission
    const char* my_type = amp_is_kcore() ? "K" : (my_core == g_amp.bsp_index ? "BSP" : "A");
    const char* target_type = g_amp.cores[target].is_kcore ? "K" : "A";
    debug_printf("[%s%u] KCORE submit PID %u -> %s%u (queue depth=%u)\n",
                 my_type, my_core, proc->pid, 
                 target_type, target, kcore_queue_depth(target));

    // Wake the target K-Core from HLT
    lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
}

// ---------------------------------------------------------------------------
// Process one entry from the queue: drain all pockets, write results
// ---------------------------------------------------------------------------

static void kcore_process_entry(struct process_t* proc)
{
    if (!proc || proc->magic != PROCESS_MAGIC) {
        return;
    }

    // Hold a reference to prevent destruction while we're processing
    process_ref_inc(proc);

    // Debug: show processing start
    uint8_t core_idx = amp_get_core_index();
    debug_printf("[K%u] KCORE process PID %u\n", core_idx, proc->pid);

    // Drain ALL pockets from this process's PocketRing.
    // Results are written to ResultRing — the process is already running
    // on its App Core spinning in result_wait(), and will see them immediately.
    //
    // IMPORTANT: kcore_pending must stay == 1 during the entire drain.
    // If we cleared it before draining, userspace could re-notify() and get
    // queued to a different K-Core, creating two concurrent consumers on the
    // same SPSC PocketRing — a data race that causes double-processing.
    guide_process_one(proc);

    // Clear the pending flag AFTER draining is complete.
    // If userspace submitted new pockets while we were draining, the next
    // notify() will CAS kcore_pending 0->1 and re-submit — no pockets lost.
    mfence();
    atomic_store_u8(&proc->kcore_pending, 0);

    process_ref_dec(proc);
}

// ---------------------------------------------------------------------------
// K-Core guide loop — infinite, never returns
// ---------------------------------------------------------------------------

void kcore_run_loop(void)
{
    uint8_t my_idx = amp_get_core_index();
    KCorePocketQueue* q = &g_kcore_queues[my_idx];
    uint32_t loop_count = 0;

    kprintf("[KCORE] Core %u entering guide loop (queue capacity=%d)\n",
            (uint32_t)my_idx, KCORE_QUEUE_CAPACITY);

    __asm__ volatile("sti");

    for (;;) {
        // Drain all pending entries
        struct process_t* proc;
        while ((proc = kcore_queue_pop(q)) != NULL) {
            kcore_process_entry(proc);
        }

        // Periodic deferred cleanup on ALL K-Cores (distributed).
        // Each K-Core drains a portion of the shared cleanup queue under lock,
        // so throughput scales with K-Core count. No double-free: queue lock
        // ensures each entry is dequeued exactly once.
        if ((++loop_count % 10) == 0) {
            process_cleanup_deferred();
        }

        // Nothing to do — sleep until IPI_WAKE or LAPIC timer
        __asm__ volatile("hlt");
    }
}
