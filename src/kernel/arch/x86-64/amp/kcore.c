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
#include "cpuid.h"  /* has_mwait_c1, cpu_monitor — sleeping watching the queue */
#include "cpu_calibrate.h"  /* cpu_us_to_tsc — the serving turn */
#include "tagfs.h"
#include "use_context.h"
#include "scheduler.h"  /* sched_fare_for, sched_rebase — a Tip is priced and placed as a Fare is */

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
    /* Each K-Core's held heap, from the PMM for the same reason: sized by
     * the process limit, kept for the kernel's lifetime. */
    size_t held_pages = (sizeof(struct process_t *) * KCORE_QUEUE_CAPACITY + PMM_PAGE_SIZE - 1) /
                        PMM_PAGE_SIZE;
    for (uint32_t i = 0; i < n; i++) {
        g_kcore_queues[i].kcore_idx = (uint8_t)i;
        if (!g_amp.cores[i].is_kcore) continue;
        void *held_phys = pmm_alloc_zero(held_pages);
        if (!held_phys) {
            kprintf("[KCORE] FATAL: cannot allocate K-Core %u's held heap (%zu pages)\n",
                    i, held_pages);
            while (1) { __asm__ volatile("cli; hlt"); }
        }
        g_kcore_queues[i].held = (struct process_t **)vmm_phys_to_virt((uintptr_t)held_phys);
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
     * and not yet served": the consumer's sleep gate reads it under CLI
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

    /* The slot emptied before the head moves past it — two stores, which
     * x86 keeps in order; nothing here is read back. */
    q->slots[h] = NULL;
    __atomic_store_n(&q->head, (h + 1) & KCORE_QUEUE_MASK, __ATOMIC_RELEASE);
    /* `count` stays: the strand still waits, held now (kcore_hold), and is
     * counted off when it is served (kcore_next). */
    return proc;
}

/* ---- The held heap: who this K-Core serves next ---------------------------
 *
 * A strand's pockets cost it a Tip, the K-Core time spent serving them at a
 * core's price (sched_fare_for), and the strand that has tipped the least is
 * served next — so a light call is not kept behind a neighbour's heavy ones,
 * whose Tip they have already run up, and a strand wearing the Use Context,
 * which pays less for the same time, is served sooner still. One pocket a
 * turn stays the rule (RT-11): a strand with more is held again behind the
 * others, at what it has now tipped. */

static void kcore_held_swap(struct process_t** held, uint32_t a, uint32_t b)
{
    struct process_t* t = held[a];
    held[a] = held[b];
    held[b] = t;
}

/* A strand taken from this K-Core's ring is held: its Tip brought onto this
 * K-Core's Waterline — a newcomer at it, one that comes back no more than a
 * turn below it — and the strand settled into the heap. */
static void kcore_hold(KCorePocketQueue* q, struct process_t* proc)
{
    int16_t  from  = proc->tip_core;
    bool     known = from >= 0 && (uint32_t)from < g_amp.total_cores &&
                     g_amp.cores[from].is_kcore;
    uint64_t there = known ? __atomic_load_n(&g_kcore_queues[from].waterline, __ATOMIC_RELAXED) : 0;

    proc->tip      = sched_rebase(proc->tip, known, there, q->waterline);
    proc->tip_core = (int16_t)q->kcore_idx;

    uint32_t i = q->held_count++;
    q->held[i] = proc;
    while (i > 0 && q->held[(i - 1) / 2]->tip > q->held[i]->tip) {
        kcore_held_swap(q->held, i, (i - 1) / 2);
        i = (i - 1) / 2;
    }
}

/* Every strand published to this K-Core's ring, held. */
static void kcore_take_arrivals(KCorePocketQueue* q)
{
    struct process_t* proc;
    while ((proc = kcore_queue_pop(q)) != NULL)
        kcore_hold(q, proc);
}

/* The held strand that has tipped the least, counted off as served; the
 * Waterline rises to its Tip. NULL when nothing is held. */
static struct process_t* kcore_next(KCorePocketQueue* q)
{
    if (q->held_count == 0)
        return NULL;

    struct process_t* proc = q->held[0];
    uint32_t n = --q->held_count;
    q->held[0] = q->held[n];
    q->held[n] = NULL;
    for (uint32_t i = 0;;) {
        uint32_t least = i, l = 2 * i + 1, r = l + 1;
        if (l < n && q->held[l]->tip < q->held[least]->tip) least = l;
        if (r < n && q->held[r]->tip < q->held[least]->tip) least = r;
        if (least == i) break;
        kcore_held_swap(q->held, i, least);
        i = least;
    }

    if (proc->tip > q->waterline)
        __atomic_store_n(&q->waterline, proc->tip, __ATOMIC_RELAXED);
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

    /* The doorbell only for a K-Core asleep in HLT (see `asleep`, kcore.h).
     * One asleep in MWAIT watches the line the push above wrote and is awake
     * already; an awake one — serving, pumping batons, or on its way round
     * the loop — reaches the pocket on its next pop, and an interrupt would
     * only break into the work it is doing. Never to ourselves: a K-Core
     * submits only from the re-arm inside its own pop loop. The push above
     * ended in a lock xadd on `count` and an mfence, so this read cannot pass
     * the claim. */
    if (target != my_core &&
        __atomic_load_n(&g_kcore_queues[target].asleep, __ATOMIC_SEQ_CST) == KCORE_ASLEEP_DEAF)
        lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
    return OK;
}

/* The gate's store of `asleep` and its last look are both sequentially
 * consistent, and so is this load after the caller published its work: either
 * the gate sees the work, or this sees the core committed to its sleep. Work
 * that is not a pocket writes no line a K-Core watches, so the doorbell rings
 * for either way of sleeping: an interrupt ends MWAIT as it ends HLT. */
void kcore_wake(uint8_t core_idx)
{
    if (!g_kcore_queues || core_idx >= g_amp.total_cores || core_idx == amp_get_core_index())
        return;
    if (__atomic_load_n(&g_kcore_queues[core_idx].asleep, __ATOMIC_SEQ_CST) != KCORE_AWAKE)
        lapic_send_ipi(g_amp.cores[core_idx].lapic_id, IPI_WAKE_VECTOR);
}

/* How long a K-Core serves one strand's pockets on end while nobody else
 * waits for it (kcore_process_entry). */
#define KCORE_SERVE_ON_US 50u

/* How long pockets may keep a K-Core from a whole round of its services
 * (kcore_run_loop). */
#define KCORE_SERVICES_DUE_US 50u

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

    /* One pocket a turn (RT-11) is fairness among the strands waiting for
     * this K-Core. With nobody else waiting there is nobody to be fair to,
     * and the strand's next pocket is served here and now instead of after a
     * trip out through the queue and back — for as long as nobody else comes
     * and the turn lasts (KCORE_SERVE_ON_US: the loop below this serves
     * batons, buses and the volume between turns). `count` is what waits
     * besides this strand: kcore_next counted it off. */
    const uint64_t began = rdtsc();
    const uint64_t until = began + cpu_us_to_tsc(KCORE_SERVE_ON_US);
    for (;;) {
        KCallTaken(proc, q->woke_at);
        guide_process_one(proc);
        if (KPocketIsEmpty(proc) || kcore_queue_depth(core_idx) != 0 ||
            rdtsc() >= until || proc->magic != PROCESS_MAGIC)
            break;
    }
    proc->tip += sched_fare_for(proc, rdtsc() - began);

    /* Let go: the mirror first, so a gate that wins kcore_pending after the
     * store below sets it after this clears it; then the full barrier
     * between letting go and the look at the ring that follows — the
     * strand's own push, barrier and look at the mirror pair with it, so a
     * pocket pushed now is seen by one side or the other. The stores
     * themselves stay in order on x86 without one. */
    __atomic_store_n(&q->serving, NULL, __ATOMIC_RELEASE);
    KPocketRingInHand(proc, false);
    atomic_store_u8(&proc->kcore_pending, 0);
    mfence();

    /* The guide served the strand's turn — one pocket, or more while nobody
     * else waited — so a ring that still holds some is the common case here,
     * not a race: the strand goes back into a queue
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
            KPocketRingInHand(proc, true);
            kcore_submit(proc);
        }
    }

    process_ref_dec(proc);
}

/* The time since the mark counted, awake or asleep. This K-Core alone writes
 * it; a reader takes the three only between two even counts of count_seq. */
static void kcore_count(KCorePocketQueue* q, bool awake)
{
    const uint64_t now  = rdtsc();
    const uint64_t span = now > q->count_mark ? now - q->count_mark : 0;
    __atomic_store_n(&q->count_seq, q->count_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    q->count_total += span;
    if (awake) q->count_awake += span;
    q->count_mark = now;
    __atomic_store_n(&q->count_seq, q->count_seq + 1, __ATOMIC_RELEASE);
}

bool kcore_cpu_usage(uint8_t core_idx, uint64_t* busy_tsc, uint64_t* total_tsc)
{
    if (!g_kcore_queues || core_idx >= g_amp.total_cores || !g_amp.cores[core_idx].is_kcore)
        return false;
    const KCorePocketQueue* q = &g_kcore_queues[core_idx];
    uint64_t mark, awake, total;
    bool     sleeping;
    for (;;) {
        const uint32_t before = __atomic_load_n(&q->count_seq, __ATOMIC_ACQUIRE);
        if (before & 1u) { __asm__ volatile("pause"); continue; }
        mark     = q->count_mark;
        awake    = q->count_awake;
        total    = q->count_total;
        sleeping = __atomic_load_n(&q->asleep, __ATOMIC_RELAXED) != KCORE_AWAKE;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&q->count_seq, __ATOMIC_RELAXED) == before) break;
    }
    /* The span since the mark, as it goes: a K-Core busy for minutes on one
     * pocket reads as busy now, not when it next sleeps. */
    const uint64_t now = rdtsc();
    if (mark != 0 && now > mark) {
        total += now - mark;
        if (!sleeping) awake += now - mark;
    }
    *busy_tsc  = awake;
    *total_tsc = total;
    return true;
}

/* The services a K-Core keeps between pockets, in the order it looks at
 * them — each one load when it has nothing to do (kcore_services). */
static void (*const g_kcore_services[])(void) = {
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
    xhci_hub_service_if_pending,

    /* And USB devices that have been unplugged. Taking one down means
     * waiting for the controller to say it has let go of the device
     * context and for whatever was mid-transfer to come out — and it is
     * here for the same reason the hubs are, which had to be measured
     * twice: the first attempt hung this on cpu_idle, where the cores are
     * parked in MWAIT, and sixty-three devices came and went without a
     * single one of them being taken down. */
    xhci_slot_service_if_pending,

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
    xhci_recover_if_needed,

    /* And, in a build made with USBRECOVER=on and in no other, the one
     * proof that the repair above actually works: the hardware deck is
     * asked, by name, to put a controller back in service, once, on a
     * machine that has one and a volume to lose. Not a call at all when
     * the switch is off. */
    HardwareDeckUsbRecoverProof,

    /* And a read on a USB disk that nobody is standing over and that was
     * never answered: giving up on one means resetting the transport, and
     * a Bulk-Only Transport reset is three control transfers. Same
     * reasoning, same context; it compares a TSC deadline, so it does not
     * care how often it is asked. */
    xhci_msd_watchdog,

    /* And a medium that arrived after the room was called to order — a
     * stick pushed in while the machine runs, or the one it booted from
     * coming back after its controller was reset. Seating it means asking
     * it how large it is and whether it is ready, which is transfers, so
     * it belongs here for the same reason as the three above. */
    BoardroomAttendIfPending,

    /* And the volume: the work of a file's Helm listed for a service point
     * — a write's next window, a USB disk's next step — a step of the
     * Settle cycle this core runs or the start of one that is due, and a
     * volume that was shut when its medium left and could not be let go
     * of, because somebody was still inside it. It is here for the same
     * reason as everything above: none of it can happen where it was
     * noticed, and on a multi-core machine this loop is where the cores
     * actually are. One atomic load a duty when there is nothing to do. */
    TagFSServiceIfPending,

    /* And the Use Context's verdicts, when one went stale: the recount for
     * cabins that never enter the kernel. One load when none is owed. */
    UseContextServiceIfPending,
};

/* One round of the services. With `pockets_first`, a pocket that has come
 * meanwhile is served before the rest of the round, which the next round
 * begins again; false when the round is owed whole. True for a whole round. */
static bool kcore_services(uint8_t my_idx, bool pockets_first)
{
    for (uint32_t i = 0; i < sizeof(g_kcore_services) / sizeof(g_kcore_services[0]); i++) {
        if (pockets_first && kcore_queue_depth(my_idx) != 0)
            return false;
        g_kcore_services[i]();
    }
    return true;
}

void kcore_run_loop(void)
{
    uint8_t my_idx = amp_get_core_index();
    KCorePocketQueue* q = &g_kcore_queues[my_idx];
    uint32_t loop_count = 0;

    kprintf("[KCORE] Core %u guide loop started\n", (uint32_t)my_idx);
    q->count_mark = rdtsc();
    const uint64_t services_due = cpu_us_to_tsc(KCORE_SERVICES_DUE_US);
    uint64_t       whole_at     = rdtsc();   /* the last whole round of the services */

    __asm__ volatile("sti");

    for (;;) {
        /*
         * ‼ ONE POCKET A TURN, AND THEN THE BATONS AND THE SERVICES (RT-11).
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
         * halt's one-way pass after every pocket; the services, the volume's
         * Helm work and a step of its Settle cycle after every pocket that
         * finds no other waiting, and whole at least every
         * KCORE_SERVICES_DUE_US. Whether pockets still wait is what holds a
         * cycle Lull merely permits back (settle.h): it never stands before
         * work already handed in.
         */
        kcore_take_arrivals(q);
        struct process_t* proc = kcore_next(q);
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

        /* The services: a pocket that comes while they are looked at is
         * served first, and does not wait out a round begun just before it
         * came — and the round is whole at least every KCORE_SERVICES_DUE_US,
         * however the pockets come. The batons above are not passed over: a completion or a
         * deadline is work handed in, as a pocket is. */
        const bool owed_whole = rdtsc() - whole_at >= services_due;
        if (kcore_services(my_idx, !owed_whole)) {
            whole_at = rdtsc();
            if ((++loop_count % 10) == 0) {
                /* P5b: reclaim exited strands (PROC_DONE/CRASHED zombies)
                 * before draining the cleanup queue, so a reaped strand's
                 * process_destroy → cleanup-queue enqueue is freed in this
                 * same round. Without the reaper, std::thread-style strand
                 * churn would exhaust the process table. Serialised
                 * internally so multiple K-Cores cooperate. */
                process_reap_strands();
                process_cleanup_deferred();
            }
        }

        /* Pockets still wait: the next turn, with no look at sleeping. */
        if (kcore_queue_depth(my_idx) != 0)
            continue;

        /* Sleep race-free. kcore_submit() pushes a pocket THEN, to a K-Core
         * asleep in HLT, sends IPI_WAKE; since the K-Core LAPIC timer is masked
         * (App Cores carry preemption), that IPI — or, to one watching its
         * queue in MWAIT, the push itself — is the ONLY pocket wakeup — so a
         * bare sleep here has a classic
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
        /* Where MWAIT C1 is to be had, the K-Core sleeps watching its queue's
         * line: a push after the watch is armed ends the wait, and one before
         * it is seen by the last look, which comes after it — no doorbell, no
         * interrupt to enter and leave, for the wake every call to a quiet
         * K-Core pays. Elsewhere HLT, and the doorbell. The watch is armed
         * after this core's own last store to that line (`asleep`, and
         * kcore_count's beside it): a store of its own may end the watch. */
        const uint32_t way = g_cpu_caps.has_mwait_c1 ? KCORE_ASLEEP_WATCHING : KCORE_ASLEEP_DEAF;
        __asm__ volatile("cli");
        kcore_count(q, true);               /* awake until the sleep */
        __atomic_store_n(&q->asleep, way, __ATOMIC_SEQ_CST);  /* before the last look */
        if (way == KCORE_ASLEEP_WATCHING)
            cpu_monitor(&q->tail);
        if (kcore_queue_depth(my_idx) != 0 || BatonPending(my_idx) || TagFSStepOwedHere()) {
            __atomic_store_n(&q->asleep, KCORE_AWAKE, __ATOMIC_RELEASE);
            __asm__ volatile("sti");        /* raced submit — loop, don't sleep */
        } else {
            if (way == KCORE_ASLEEP_WATCHING)
                cpu_sti_mwait_c1();         /* atomic arm-and-sleep, as STI;HLT */
            else
                __asm__ volatile("sti; hlt");
            kcore_count(q, false);          /* asleep until now: counted before it says it woke */
            q->woke_at = rdtsc();
            __atomic_store_n(&q->asleep, KCORE_AWAKE, __ATOMIC_RELEASE);
        }
    }
}
