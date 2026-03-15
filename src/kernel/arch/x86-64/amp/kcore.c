#include "kcore.h"
#include "process.h"
#include "guide.h"
#include "scheduler.h"
#include "lapic.h"
#include "irqchip.h"
#include "atomics.h"
#include "klib.h"
#include "vmm.h"
#include "pocket_ring.h"
#include "perf_trace.h"
#include "idle.h"

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
        // CAS failed — another App Core beat us, retry
        cpu_pause();
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
    // Wake the target K-Core from HLT
    lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
}

// ---------------------------------------------------------------------------
// Process one entry from the queue: drain all pockets, write results, wake
// ---------------------------------------------------------------------------

static void kcore_process_entry(struct process_t* proc)
{
    if (!proc || proc->magic != PROCESS_MAGIC) {
        return;
    }

    // Hold a reference to prevent destruction while we're processing
    process_ref_inc(proc);

    // Clear the pending flag BEFORE draining.
    // If the process does another notify() while we drain, it will see
    // kcore_pending == 0 and submit again (to the same or different K-Core).
    // That's fine — the new entry will be processed after we finish this drain.
    atomic_store_u8(&proc->kcore_pending, 0);
    mfence();

    // Drain ALL pockets from this process's PocketRing
    guide_process_one(proc);

    // Check if process is still alive after guide processing
    process_state_t st = process_get_state(proc);
    if (st == PROC_DONE || st == PROC_CRASHED) {
        process_ref_dec(proc);
        return;
    }

    // Wake the process: PROC_WAITING → PROC_WORKING (enqueues to home RunQueue)
    if (process_get_state(proc) == PROC_WAITING) {
        uint8_t home = proc->home_core;
        process_set_state(proc, PROC_WORKING);
        // process_set_state already calls sched_enqueue when transitioning to WORKING.
        // Send IPI_WAKE to the home App Core so it reschedules immediately
        // instead of waiting for the next timer tick (10ms latency → ~0).
        if (home < g_amp.total_cores && !g_amp.cores[home].is_kcore) {
            lapic_send_ipi(g_amp.cores[home].lapic_id, IPI_WAKE_VECTOR);
        }
    }

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

        // Periodic deferred cleanup on BSP K-Core (processes marked DONE/CRASHED).
        // schedule() no longer runs on K-Cores, so cleanup must happen here.
        if (my_idx == g_amp.bsp_index && (++loop_count % 10) == 0) {
            process_cleanup_deferred();
        }

        // Nothing to do — sleep until IPI_WAKE or LAPIC timer
        __asm__ volatile("hlt");
    }
}
