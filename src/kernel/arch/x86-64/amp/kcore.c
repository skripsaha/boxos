#include "kcore.h"
#include "process.h"
#include "guide.h"
#include "lapic.h"
#include "irqchip.h"
#include "atomics.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "perf_trace.h"
#include "amp.h"
#include "irq_defer.h"
#include "error.h"
#include "kring.h"  /* KPocketIsEmpty for re-arm after pending clear */
#include "nightwatch.h"
#include "storage_completion.h"  /* Never-drop MPSC: async storage continuations */

KCorePocketQueue *g_kcore_queues = NULL;

void kcore_init(void)
{
    uint32_t n = g_amp.total_cores;
    /* From the PMM: this array is sized by CORE COUNT — 65 KiB at 16 cores,
     * ~1 MiB at MAX_CORES — and the kernel heap is a fixed small-object pool
     * that does not grow with the machine. Allocated once at boot and never
     * freed, so the Pull-Map pointer is valid for the kernel's lifetime. */
    size_t queue_bytes = sizeof(KCorePocketQueue) * n;
    size_t queue_pages = (queue_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *queue_phys  = pmm_alloc_zero(queue_pages);
    g_kcore_queues = queue_phys ? (KCorePocketQueue *)vmm_phys_to_virt((uintptr_t)queue_phys)
                                : NULL;
    if (!g_kcore_queues) {
        kprintf("[KCORE] FATAL: cannot allocate queue array (%u cores, %zu pages)\n",
                n, queue_pages);
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    for (uint32_t i = 0; i < n; i++) {
        g_kcore_queues[i].kcore_idx = (uint8_t)i;
    }
    debug_printf("[KCORE] %u K-Core queue(s) initialized (%u bytes)\n",
                 g_amp.k_count, (uint32_t)(sizeof(KCorePocketQueue) * n));
}

uint32_t kcore_queue_depth(uint8_t core_idx)
{
    return atomic_load_u32(&g_kcore_queues[core_idx].count);
}

static error_t kcore_queue_push(KCorePocketQueue* q, struct process_t* proc)
{
    if (!q || !proc)
        return ERR_INVALID_ARGUMENT;

    uint32_t old_tail, new_tail;
    uint32_t spins = 0;
    
    for (;;) {
        old_tail = atomic_load_u32(&q->tail);
        new_tail = (old_tail + 1) & KCORE_QUEUE_MASK;

        if (new_tail == atomic_load_u32(&q->head)) {
            return ERR_KCORE_QUEUE_FULL;
        }

        if (atomic_cas_u32(&q->tail, old_tail, new_tail)) {
            break;
        }
        
        uint32_t backoff = 1u << (spins < 4 ? spins : 4);
        for (uint32_t b = 0; b < backoff; b++) {
            cpu_pause();
        }
        spins++;
    }

    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);
    mfence();
    atomic_fetch_add_u32(&q->count, 1);
    return OK;
}

static struct process_t* kcore_queue_pop(KCorePocketQueue* q)
{
    if (!q)
        return NULL;

    uint32_t h = q->head;
    uint32_t t = atomic_load_u32(&q->tail);
    
    if (h == t) {
        return NULL;
    }

    struct process_t* proc;
    uint32_t spin_limit = 0;
    for (;;) {
        proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
        if (proc != NULL) break;
        cpu_pause();
        if (++spin_limit > KCORE_POP_SPIN_LIMIT) {
            debug_printf("[KCORE] WARNING: slot %u stale after %u spins, resetting\n",
                         h, spin_limit);
            q->slots[h] = NULL;
            q->head = (h + 1) & KCORE_QUEUE_MASK;
            atomic_fetch_sub_u32(&q->count, 1);
            return NULL;
        }
    }

    q->slots[h] = NULL;
    mfence();
    q->head = (h + 1) & KCORE_QUEUE_MASK;
    atomic_fetch_sub_u32(&q->count, 1);
    return proc;
}

static uint8_t kcore_find_least_loaded(void)
{
    uint8_t best_core = g_amp.bsp_index;
    uint32_t best_depth = UINT32_MAX;

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        CoreDescriptor *c = &g_amp.cores[i];
        if (!c->is_kcore) continue;
        /* Skip K-Cores that never came online (boot timed out) or have
         * been quiesced. Without this check a dead K-Core's empty queue
         * always wins the "least loaded" race, the producer pushes the
         * pocket, fires IPI_WAKE at a CPU that never returned from
         * INIT — and the pocket sits in the ring forever. */
        if (!amp_core_online(c)) continue;
        uint32_t depth = kcore_queue_depth(i);
        if (depth < best_depth) {
            best_depth = depth;
            best_core = i;
        }
    }
    return best_core;
}

error_t kcore_submit(struct process_t* proc)
{
    if (!proc)
        return ERR_INVALID_ARGUMENT;

    uint8_t target = kcore_find_least_loaded();
    uint8_t my_core = amp_get_core_index();

    error_t result = kcore_queue_push(&g_kcore_queues[target], proc);
    if (result == OK) {
        goto submitted;
    }

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        if (!g_amp.cores[i].is_kcore || i == target) continue;
        result = kcore_queue_push(&g_kcore_queues[i], proc);
        if (result == OK) {
            target = i;
            goto submitted;
        }
    }

    // CRITICAL: Do NOT clear kcore_pending here.
    // Userspace may have written new pockets to PocketRing before we returned.
    // Keeping kcore_pending=1 ensures the next notify() will retry submission.
    // Clearing it would cause pocket loss (in ring but never processed).
    debug_printf("[KCORE] All queues full for PID %u, keeping kcore_pending=1 for retry\n", proc->pid);
    return ERR_KCORE_SUBMIT_FAILED;

submitted:
    const char* my_type = amp_is_kcore() ? "K" : (my_core == g_amp.bsp_index ? "BSP" : "A");
    debug_printf("[%s%u] KCORE submit PID %u -> K%u (depth=%u)\n",
                 my_type, my_core, proc->pid, target, kcore_queue_depth(target));

    lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
    return OK;
}

static void kcore_process_entry(struct process_t* proc)
{
    if (!proc || proc->magic != PROCESS_MAGIC) {
        return;
    }

    process_ref_inc(proc);

    uint8_t core_idx = amp_get_core_index();
    /* Nightwatch: work arrived, so this K-Core is no longer idle. */
    nightwatch_core_busy(core_idx);
    debug_printf("[K%u] Processing PID %u\n", core_idx, proc->pid);

    guide_process_one(proc);

    mfence();
    atomic_store_u8(&proc->kcore_pending, 0);
    mfence();

    /* Re-arm window: between guide_process_one's last KPocketIsEmpty
     * check and the kcore_pending=0 store above, userspace can have
     * pushed a fresh Pocket. The next SYSCALL's CAS would fail (pending
     * was still 1) and the Pocket would sit forever. After clearing
     * pending we re-inspect the ring; if it's not empty we resubmit so
     * a K-Core picks it up.
     *
     * We're allowed to call kcore_submit recursively here — the proc
     * goes back into a queue, ref_inc balances ref_dec, and the next
     * pop will land in another kcore_process_entry. No locks held
     * across this. */
    if (!KPocketIsEmpty(proc)) {
        if (atomic_cas_u8(&proc->kcore_pending, 0, 1)) {
            kcore_submit(proc);
        }
    }

    process_ref_dec(proc);
}

void kcore_run_loop(void)
{
    uint8_t my_idx = amp_get_core_index();
    KCorePocketQueue* q = &g_kcore_queues[my_idx];
    uint32_t loop_count = 0;

    kprintf("[KCORE] Core %u guide loop started\n", (uint32_t)my_idx);

    __asm__ volatile("sti");

    for (;;) {
        struct process_t* proc;
        while ((proc = kcore_queue_pop(q)) != NULL) {
            kcore_process_entry(proc);
        }

        /* Drain async storage completions on this K-Core (never-drop MPSC).
         * Read + write state-machine steps that came back from an AHCI IRQ,
         * plus port-recovery COMRESETs, land here; running them in the same
         * loop body keeps cache-locality with the pocket pump. Single-
         * consumer: each K-Core drains only its own queue. All completions
         * route to the drain core, so non-drain queues are a one-load
         * early-out. */
        StorageCompletionPump(my_idx);

        /* Universal IRQ-defer drain — runs SCI/GPE/AHCI bottom-halves
         * that the IRQ stowed away with irq_defer(). Same K-Core
         * context, so handlers can kmalloc / tagfs / process_walk
         * safely. */
        irq_defer_pump(my_idx);

        if ((++loop_count % 10) == 0) {
            /* P5b: reclaim exited strands (PROC_DONE/CRASHED zombies) before
             * draining the cleanup queue, so a reaped strand's process_destroy
             * → cleanup-queue enqueue is freed in this same tick. Without the
             * reaper, std::thread-style strand churn would exhaust the process
             * table. Serialised internally so multiple K-Cores cooperate. */
            process_reap_strands();
            process_cleanup_deferred();
        }

        /* Sleep race-free. kcore_submit() pushes a pocket THEN sends IPI_WAKE;
         * since the K-Core LAPIC timer is masked (App Cores carry preemption),
         * that IPI is the ONLY pocket wakeup — so a bare HLT here has a classic
         * lost-wakeup: a submit landing between the drain above and the HLT can
         * have its IPI consumed before we sleep, stranding us in HLT with a
         * queued pocket forever (the symptom: a command hangs with no crash,
         * seen at 16 cores where there are several AP K-Cores). Close the
         * window: disable interrupts, re-check the queue, and HLT only while
         * it is still empty. STI;HLT is atomic — the one-instruction STI
         * interrupt shadow defers delivery until after HLT executes — so an IPI
         * that arrived under CLI wakes us the instant we sleep.
         *
         * Storage async completions ARE re-checked (StorageCompletionPending):
         * their never-drop node can be posted cross-core (an App-Core token
         * handoff), which also sends IPI_WAKE — and the re-check closes the
         * post-pump / pre-CLI window so a completion that landed there is never
         * left queued across a HLT (AHCI port recovery rides the same storage
         * queue, so it is covered too). Other irq-defer work (SCI/GPE/ATA) is
         * fed by its own device IRQ, which wakes HLT directly, so it needs no
         * re-check.
         *
         * (Before the K-Core timer was masked, the 100 Hz tick papered over
         * this race by waking every 10 ms; this is the proper fix.) */
        /* Nightwatch: about to sleep with an empty queue. Marked before the
         * CLI so the mark is never held across the sleep decision. */
        nightwatch_core_idle(my_idx);

        __asm__ volatile("cli");
        if (kcore_queue_depth(my_idx) != 0 || StorageCompletionPending(my_idx)) {
            __asm__ volatile("sti");        /* raced submit — loop, don't sleep */
        } else {
            __asm__ volatile("sti; hlt");   /* atomic arm-and-sleep */
        }
    }
}
