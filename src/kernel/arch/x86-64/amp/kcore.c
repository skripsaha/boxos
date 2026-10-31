#include "kcore.h"
#include "boardroom.h"
#include "xhci_hub.h"
#include "hardware_deck.h"
#include "xhci_msd.h"
#include "xhci_enumeration.h"
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
#include "error.h"
#include "kring.h"  /* KPocketIsEmpty for re-arm after pending clear */
#include "nightwatch.h"
#include "baton.h"  /* Never-drop MPSC: async storage continuations */
#include "tagfs.h"
#include "use_context.h"

KCorePocketQueue *g_kcore_queues = NULL;

void kcore_init(void)
{
    uint32_t n = g_amp.total_cores;
    /* From the PMM: this array is sized by CORE COUNT — 65 KiB at 16 cores,
     * ~1 MiB at MAX_CORES — in whole pages. Allocated once at boot and never
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
    /* There may be no queues at all. kcore_init runs only when the machine has
     * more than one core, so on a uniprocessor g_kcore_queues is NULL for the
     * kernel's whole life — and a reader asking how deep a queue is on a
     * machine with none is asking a fair question with an obvious answer.
     * It used to be a #PF at 0x8008 from inside Nightwatch's summary, which is
     * a thing to crash on only if you are certain SMP is the only shape this
     * kernel runs in. */
    if (!g_kcore_queues || core_idx >= g_amp.total_cores) return 0;
    return atomic_load_u32(&g_kcore_queues[core_idx].count);
}

/* One producer protocol, in two steps: a position is CLAIMED (the CAS on
 * `tail`) and then PUBLISHED (the slot store). Between the two the position
 * belongs to the producer and to nobody else — and that gap is not small
 * everywhere: on the board it can hold an SMI, on the stand a descheduled
 * vCPU. The consumer below is written so that the gap costs it nothing but
 * a little patience, never a slot. */
bool kcore_is_serving(const struct process_t* proc)
{
    if (!g_kcore_queues || !proc) return false;
    for (uint8_t c = 0; c < g_amp.total_cores; c++) {
        if (!g_amp.cores[c].is_kcore) continue;
        if (__atomic_load_n(&g_kcore_queues[c].serving, __ATOMIC_ACQUIRE) == proc)
            return true;
    }
    return false;
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

    /* Claimed. `count` rises HERE, before the publish, so it says "claimed
     * and not yet consumed": the consumer's sleep gate reads it under CLI
     * and will not lie down over a claim still in flight, and a consumer
     * that takes the slot the instant it appears cannot drive the count
     * below zero. It used to rise after the publish, and a pop that beat it
     * left 0xFFFFFFFF in every depth — the least-loaded choice, the sleep
     * gate and Nightwatch's summary all read it. */
    atomic_fetch_add_u32(&q->count, 1);
    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);
    /* Published. The doorbell follows in kcore_submit, and in x2APIC mode
     * that is a WRMSR to the ICR, which may complete before this store is
     * globally visible (Intel SDM Vol 3A §10.12.3) — so the fence stays
     * between the publish and the bell. */
    mfence();
    return OK;
}

/* The consumer owns `head` and judges nothing but the slot there.
 *
 * An empty slot at the head means one of two things, and the consumer cannot
 * tell which: nothing has been claimed, or a position has been claimed and
 * not yet published. The answer is the same either way — there is nothing to
 * take — and the head stays where it is. The pop that stood here read `tail`
 * too, and when tail said "claimed" while the slot said "not yet" it waited a
 * million spins and then MOVED HEAD PAST THE CLAIM: it took the position from
 * under a producer that was still walking up to it. The pocket was then
 * published behind the head, where nobody would ever look; its owner kept
 * kcore_pending set, so no later notify could submit it again; and it waited
 * for an answer to a question nobody would ever open. The warning it printed
 * was debug_printf, which the shipped kernel compiles to nothing. And 4096
 * claims later the head would have come round to that stale pointer and
 * served a dead entry as a fresh one.
 *
 * Nothing waits here. The run loop goes round — the pumps, then the sleep
 * gate, which reads `count` under CLI: a claim in flight keeps the count up,
 * so the K-Core stays awake and comes back to this slot; a queue with nothing
 * claimed lets it sleep, and the producer's IPI, sent after the publish,
 * wakes it. A stalled producer costs the consumer exactly the stall, and
 * nothing else. logcheck kcoreclaim widens the gap to two milliseconds on
 * every application submit and requires the machine to come through;
 * kcoreclaimmut puts the old pop back and requires the machine to stop. */
static struct process_t* kcore_queue_pop(KCorePocketQueue* q)
{
    if (!q)
        return NULL;

    uint32_t h = q->head;
    struct process_t* proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
    if (!proc) return NULL;

    q->slots[h] = NULL;
    mfence();
    q->head = (h + 1) & KCORE_QUEUE_MASK;
    atomic_fetch_sub_u32(&q->count, 1);
    return proc;
}

/* Least loaded, where the load a K-Core carries is what is queued on it PLUS
 * the strand it is serving right now: `count` falls at pop, before the guide
 * runs, so a K-Core deep in a long pocket used to read as empty and win every
 * choice. On a tie the BSP loses — it already takes every interrupt, every
 * baton and the tick — so with nothing else to tell them apart the pocket goes
 * to a K-Core that has only pockets to serve. */
static uint8_t kcore_find_least_loaded(void)
{
    uint8_t  best_core = g_amp.bsp_index;
    uint32_t best_rank = UINT32_MAX;

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        CoreDescriptor *c = &g_amp.cores[i];
        if (!c->is_kcore) continue;
        /* Skip K-Cores that never came online (boot timed out) or have
         * been quiesced. Without this check a dead K-Core's empty queue
         * always wins the "least loaded" race, the producer pushes the
         * pocket, fires IPI_WAKE at a CPU that never returned from
         * INIT — and the pocket sits in the ring forever. */
        if (!amp_core_online(c)) continue;
        uint32_t load = kcore_queue_depth(i) +
                        (__atomic_load_n(&g_kcore_queues[i].serving, __ATOMIC_ACQUIRE) ? 1u : 0u);
        uint32_t rank = load * 2u + (i == g_amp.bsp_index ? 1u : 0u);
        if (rank < best_rank) {
            best_rank = rank;
            best_core = i;
        }
    }
    return best_core;
}

error_t kcore_submit(struct process_t* proc)
{
    if (!proc)
        return ERR_INVALID_ARGUMENT;

    /* The queue holds the strand, not a pointer to whatever is there when a
     * K-Core comes round to it. The slot takes a reference here; the K-Core
     * that pops it gets that reference with it (kcore_process_entry) and drops
     * it once the pocket is served. The reaper knows nothing of the queues:
     * without the reference it could tear a strand down and free it while a
     * slot still named it, and the pop read freed memory — or a strand made
     * since in the same place, whose ring it then served. The caller holds the
     * strand alive across this call: the syscall gate runs on it, and the
     * re-arm holds the reference of the K-Core serving it. */
    process_ref_inc(proc);

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

    /* Unreachable by construction: capacity equals the process limit and
     * kcore_pending dedup holds each process to one slot, so no mix of
     * live processes can fill a queue. If this ever prints, an invariant
     * broke (slot leak, dedup bypass) — and the old quiet handling here
     * was itself the wedge: keeping kcore_pending=1 with the pocket queued
     * nowhere made every later notify skip the submit ("already pending"),
     * stranding the process forever while the K-Cores slept over truly
     * empty queues. Say it loudly; leave pending set so the evidence
     * (ring non-empty, pending=1, queues full) stays intact for Nightwatch
     * rather than being papered over by a retry that cannot succeed. */
    kprintf("[KCORE] DEFECT: no queue slot for PID %u — dedup/capacity invariant broken\n",
            proc->pid);
    process_ref_dec(proc);
    return ERR_KCORE_SUBMIT_FAILED;

submitted:
    const char* my_type = amp_is_kcore() ? "K" : (my_core == g_amp.bsp_index ? "BSP" : "A");
    debug_printf("[%s%u] KCORE submit PID %u -> K%u (depth=%u)\n",
                 my_type, my_core, proc->pid, target, kcore_queue_depth(target));

    /* The doorbell only for a K-Core in HLT (see `in_hlt`, kcore.h). An awake
     * one — serving, pumping batons, or on its way round the loop — reaches
     * the pocket on its next pop, and an interrupt would only break into the
     * work it is doing. Never to ourselves: a K-Core submits only from the
     * re-arm inside its own pop loop. The push above ended in a lock xadd on
     * `count` and an mfence, so this read cannot pass the claim. */
    if (target != my_core &&
        __atomic_load_n(&g_kcore_queues[target].in_hlt, __ATOMIC_SEQ_CST))
        lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
    return OK;
}

/* The gate's store of in_hlt and its last look are both sequentially
 * consistent, and so is this load after the caller published its work: either
 * the gate sees the work, or this sees the core committed to its HLT. */
void kcore_wake(uint8_t core_idx)
{
    if (!g_kcore_queues || core_idx >= g_amp.total_cores || core_idx == amp_get_core_index())
        return;
    if (__atomic_load_n(&g_kcore_queues[core_idx].in_hlt, __ATOMIC_SEQ_CST))
        lapic_send_ipi(g_amp.cores[core_idx].lapic_id, IPI_WAKE_VECTOR);
}

static void kcore_process_entry(struct process_t* proc)
{
    if (!proc)
        return;

    /* The slot's reference came with the strand (kcore_submit) and is dropped
     * at the end, whatever happens here. A strand torn down while it waited in
     * the queue — process_destroy poisons its magic — is alive only by that
     * reference, and is not served. */
    if (proc->magic != PROCESS_MAGIC) {
        process_ref_dec(proc);
        return;
    }

    uint8_t core_idx = amp_get_core_index();
    KCorePocketQueue* q = &g_kcore_queues[core_idx];
    /* Nightwatch: work arrived, so this K-Core is no longer idle — and it is
     * THIS strand's work. Its pocket stays at the head of its ring until the
     * guide is done with it, which can be minutes; `serving` is what tells
     * that apart from a pocket nobody came for (POCKET UNSERVED). */
    nightwatch_core_busy(core_idx);
    __atomic_store_n(&q->serving, proc, __ATOMIC_RELEASE);
    debug_printf("[K%u] Processing PID %u\n", core_idx, proc->pid);

    guide_process_one(proc);

    __atomic_store_n(&q->serving, NULL, __ATOMIC_RELEASE);
    mfence();
    atomic_store_u8(&proc->kcore_pending, 0);
    mfence();

    /* The guide served one pocket (RT-11), so a ring that still holds some
     * is the common case here, not a race: the strand goes back into a queue
     * — the least loaded, behind whatever waits there — and its next pocket
     * is served when its turn comes round again, by whichever K-Core that
     * is. Its pockets stay in order: only the K-Core that holds
     * kcore_pending serves the ring. And the race is covered by the same
     * look: between the guide's pocket and the kcore_pending=0 store above,
     * userspace can have pushed a fresh Pocket whose SYSCALL found pending
     * still 1, and would otherwise sit forever.
     *
     * We're allowed to call kcore_submit recursively here — the proc
     * goes back into a queue, whose slot takes a reference of its own while
     * this one is still held, and the next pop will land in another
     * kcore_process_entry. No locks held across this. */
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
        /*
         * ‼ ONE POCKET A TURN, AND THEN THE WHOLE LOOP (RT-11).
         *
         * The boundary is the pocket, never the program. This used to pop until
         * the queue was empty, and the guide served each program's ring until
         * IT was empty: a program that handed pockets in faster than they were
         * served — any program, a fire-and-forget call in a loop — kept its
         * K-Core inside its ring for good. Every other program's pocket queued
         * behind it; on the drain core no baton ran (a completion, a park's
         * deadline, the power button), no Helm work, no service below; on the
         * committer's core no cycle began, the one in flight stood still
         * between two steps holding the volume's door, and the window the
         * mount promises was not kept. Now one pocket is served — a program
         * with more is queued again behind the others (kcore_process_entry) —
         * and everything below has its turn before the next: the batons and a
         * halt's one-way pass, the services, the volume's Helm work and a step
         * of its Settle cycle. Whether pockets still wait is what holds a
         * cycle Lull merely permits back (settle.h): it never stands before
         * work already handed in.
         */
        struct process_t* proc = kcore_queue_pop(q);
        if (proc)
            kcore_process_entry(proc);

        /* Drain the batons on this K-Core (never-drop MPSC): storage
         * completions that came back from an IRQ, port-recovery COMRESETs,
         * park deadlines, and every knock — the Touch IRQ ring, the power
         * button, a GHES source, an MCE slot. Same K-Core context, so the
         * continuations can kmalloc / tagfs / process_walk safely; running
         * them in the same loop body keeps cache-locality with the pocket
         * pump. Single-consumer: each K-Core drains only its own queue. All
         * passes route to the drain core, so non-drain queues are a one-load
         * early-out. No pocket is being served here, so this is where a
         * one-way pass — the power button's halt — is taken, and a pump inside
         * a pocket's wait leaves it here (baton.h). A cycle this core runs may
         * stand between two of its steps meanwhile: the halt runs it to its
         * end itself (settle.h). */
        BatonPumpAtRest(my_idx);

        /* USB hubs, when one has reported a change on its ports.
         *
         * Finding out what a hub means by that takes control transfers, and
         * those have to be waited for — which the interrupt handler that
         * received the report cannot do. This is the same reasoning that puts
         * the deferred IRQ work above in this loop, and the same context.
         *
         * It is here rather than in the idle loop because on a multi-core
         * machine the idle loop is not where the cores are: they are here.
         * Measured — a hub reported, the flag was raised, and cpu_idle was not
         * reached once in eight seconds.
         *
         * The check is one atomic load when there is nothing to do, which is
         * every iteration but the ones after somebody touched a socket. */
        xhci_hub_service_if_pending();

        /* And USB devices that have been unplugged. Taking one down means
         * waiting for the controller to say it has let go of the device
         * context and for whatever was mid-transfer to come out — and it is
         * here for the same reason the hubs are, which had to be measured
         * twice: the first attempt hung this on cpu_idle, where the cores are
         * parked in MWAIT, and sixty-three devices came and went without a
         * single one of them being taken down. */
        xhci_slot_service_if_pending();

        /* And a controller that has stopped itself. Host Controller Error and
         * Host System Error both mean it has halted and will not start again
         * on its own; the answer is a reset, which takes up to a second and so
         * cannot happen where the error was noticed. The same call now also
         * takes back a command ring that has stopped answering, which the
         * specification allows the controller five seconds to let go of — and
         * five seconds is what the timer interrupt that noticed would have
         * spent not sending its end-of-interrupt. One core does both and the
         * rest go away. One load of a flag per controller when nothing has
         * failed. */
        xhci_recover_if_needed();

        /* And, in a build made with USBRECOVER=on and in no other, the one
         * proof that the repair above actually works: the hardware deck is
         * asked, by name, to put a controller back in service, once, on a
         * machine that has one and a volume to lose. Not a call at all when
         * the switch is off. */
        HardwareDeckUsbRecoverProof();

        /* And a read on a USB disk that nobody is standing over and that was
         * never answered: giving up on one means resetting the transport, and
         * a Bulk-Only Transport reset is three control transfers. Same
         * reasoning, same context; it compares a TSC deadline, so it does not
         * care how often it is asked. */
        xhci_msd_watchdog();

        /* And a medium that arrived after the room was called to order — a
         * stick pushed in while the machine runs, or the one it booted from
         * coming back after its controller was reset. Seating it means asking
         * it how large it is and whether it is ready, which is transfers, so
         * it belongs here for the same reason as the three above. */
        BoardroomAttendIfPending();

        /* And the volume: the work of a file's Helm listed for a service point
         * — a write's next window, a USB disk's next step — a step of the
         * Settle cycle this core runs or the start of one that is due, and a
         * volume that was shut when its medium left and could not be let go
         * of, because somebody was still inside it. It is here for the same
         * reason as everything above: none of it can happen where it was
         * noticed, and on a multi-core machine this loop is where the cores
         * actually are. One atomic load a duty when there is nothing to do. */
        TagFSServiceIfPending();

        /* And the Use Context's verdicts, when one went stale: the recount for
         * cabins that never enter the kernel. One load when none is owed. */
        UseContextServiceIfPending();

        if ((++loop_count % 10) == 0) {
            /* P5b: reclaim exited strands (PROC_DONE/CRASHED zombies) before
             * draining the cleanup queue, so a reaped strand's process_destroy
             * → cleanup-queue enqueue is freed in this same tick. Without the
             * reaper, std::thread-style strand churn would exhaust the process
             * table. Serialised internally so multiple K-Cores cooperate. */
            process_reap_strands();
            process_cleanup_deferred();
        }

        /* Pockets still wait: the next turn, with no look at sleeping. */
        if (kcore_queue_depth(my_idx) != 0)
            continue;

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
         * Batons ARE re-checked (BatonPending): a never-drop node can be
         * posted cross-core (an App-Core token handoff, an xHCI MSI on another
         * core), which also sends IPI_WAKE — and the re-check closes the
         * post-pump / pre-CLI window so a pass that landed there is never left
         * queued across a HLT. A same-core pass comes from an interrupt, which
         * wakes HLT itself.
         *
         * (Before the K-Core timer was masked, the 100 Hz tick papered over
         * this race by waking every 10 ms; this is the proper fix.) */
        /* Nightwatch: about to sleep with an empty queue. Marked before the
         * CLI so the mark is never held across the sleep decision. */
        nightwatch_core_idle(my_idx);

        /* Measured, when interrupt work still lived in a ring of its own that
         * this gate did not ask about: a program being read off a stick
         * throttled to 512 bytes a second sat in PROC_WAITING for 248 seconds
         * with every core idle, and Nightwatch named it ("ring holds head=0
         * tail=1, yet this process still waits"). Everything an interrupt
         * hands over now rides a baton, and the baton queue is asked. So is
         * the volume (TagFSStepOwedHere): Helm work listed meanwhile — its
         * knock may have been run by a pump nested inside the service point
         * above, after the list was looked at — a step of this core's cycle,
         * a cycle due, or one owed later that no Lull is armed yet to wake
         * this core for. */
        __asm__ volatile("cli");
        __atomic_store_n(&q->in_hlt, 1u, __ATOMIC_SEQ_CST);  /* before the last look */
        if (kcore_queue_depth(my_idx) != 0 || BatonPending(my_idx) || TagFSStepOwedHere()) {
            __atomic_store_n(&q->in_hlt, 0u, __ATOMIC_RELEASE);
            __asm__ volatile("sti");        /* raced submit — loop, don't sleep */
        } else {
            __asm__ volatile("sti; hlt");   /* atomic arm-and-sleep */
            __atomic_store_n(&q->in_hlt, 0u, __ATOMIC_RELEASE);
        }
    }
}
