#include "serial.h"      /* WireForceRelease / WireDrain — every rung is heard */
#include "klib_logring.h" /* LogKeepFlush — the last words into memory that outlives the reset */
#include "system_halt.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"
#include "ioapic.h"   /* IoapicMaskEveryPin — the chip's own count of its pins */
#include "process.h"
#include "scheduler.h"
#include "tagfs.h"
#include "settle.h"      /* the volume's last Settle cycle, run here before cli */
#include "scribe.h"      /* VolumeWritesAnswered — the medium still answering the work owed */
#include "helm.h"        /* the Helm work still owed, and run by the drain */
#include "boardroom.h"    /* the seat, so a lost commit can be named */
#include "ata.h"
#include "acpi.h"
#include "klib.h"
#include "io.h"
#include "atomics.h"
#include "xhci.h"
#include "xhci_port.h"
#include "touch.h"
#include "baton.h"
#include "ahci.h"
#include "amp.h"
#include "hourglass.h"
#include "cpu_calibrate.h"    /* cpu_tsc_to_us — how long an owed stop took */
#include "kernel_config.h"
#include "nameplate.h"        /* nameplate_name_at_kernel — name a stuck continuation */
#include "nameplate_format.h" /* NameplateHeader — the table's own minimum size */
#include "vmm.h"              /* vmm_tlb_shootdown_poll — the roll call answers by hand */

/* WHOSE deadline may judge a drain that has stopped moving — decided on the
 * machine in front of us, from what is actually holding the un-drained work.
 *
 * CONFIG_AHCI_IO_TIMEOUT_MS is this kernel's written answer to "how long may
 * ONE storage command legitimately take before it is declared non-completing":
 * the deadline the AHCI async watchdog enforces (ahci.c). It is the right ruler
 * for a command sitting in PxCI (AHCI 1.3.1 §3.3.14) or PxSACT (§3.3.13) and for
 * nothing else. Turned unconditionally it bounded the shutdown of a machine
 * with NO AHCI CONTROLLER AT ALL — the owner's B365M boots from a USB stick and
 * its board log names no AHCI — so a permanent off-by-one in a queue count was
 * paid for with thirty seconds of screen, waiting out the patience owed to a
 * device that is not present.
 *
 * A baton nobody consumes is a different fact with a different owner. Each
 * baton queue is single-consumer and falls ONLY when its own core pumps it
 * (baton.h), so a depth that stops falling says that CORE has stopped pumping
 * — and the kernel has one written answer to "a core is not answering" too:
 * CONFIG_TLB_SHOOTDOWN_PANIC_MS, the ceiling shootdown_wait_acks uses before it
 * calls a missing ack a cross-core deadlock (vmm.c), and the same ceiling
 * halt_all_ap_cores gives an AP to answer the halt IPI below. Both macros here
 * are unit conversions of a constant this kernel already decided; neither
 * invents a number, and neither is applied to a medium it does not describe. */
#define HALT_STALLED_AHCI_US ((uint64_t)CONFIG_AHCI_IO_TIMEOUT_MS * 1000ULL)
#define HALT_STALLED_CORE_US ((uint64_t)CONFIG_TLB_SHOOTDOWN_PANIC_MS * 1000ULL)

/* Set bits in a 32-bit word, counted here rather than by __builtin_popcount.
 *
 * The builtin is not free-standing: unless the target is compiled with POPCNT
 * available, GCC lowers it to a call into libgcc (__popcountsi2/__popcountdi2),
 * and this kernel links no libgcc — the reference is unresolved at link time.
 * Requiring POPCNT instead would mean refusing to shut down cleanly on a part
 * that lacks SSE4.2, which is a worse trade for a path that runs once, at
 * poweroff, over at most 32 ports. The classic SWAR fold is branch-free,
 * constant-time and needs nothing from the CPU beyond shifts and adds. */
/* The kernel's own name table, and the one question asked of it here: given a
 * continuation's address, what was it going to run? The panic path does the
 * same lookup for the same reason (idt.c) — a machine in trouble must name
 * what it is holding rather than print a number and leave the reader guessing
 * which of a dozen subsystems owns it. Never fails loudly: a table that is
 * absent or too small simply yields the honest placeholder. */
extern const char __nameplate_start[];
extern const char __nameplate_end[];

static const char *halt_name_of(void (*run)(void *ctx), NameplateName *scratch)
{
    if (!run) return "nothing";

    uint64_t bytes = (uint64_t)(__nameplate_end - __nameplate_start);
    if (bytes >= sizeof(NameplateHeader) &&
        nameplate_name_at_kernel((uintptr_t)__nameplate_start, bytes,
                                 (uintptr_t)run, scratch))
        return scratch->Text;

    return "an unnamed continuation";
}

/* What the machine still owes, and WHOSE it is — because the amount alone
 * cannot say which deadline is allowed to judge it.
 *
 * Counted the ways storage work can be owed:
 *   1. Never-drop baton passes posted but not yet consumed — the read/write
 *      state-machine continuations the AHCI IRQ hands on, plus every other
 *      pass an interrupt made (SCI/GPE, AHCI recovery, the Touch IRQ ring's
 *      knock). A pending WRITE completion still owes its tagfs commit, so one
 *      of these dropped is user data lost. This is a real DEPTH: BatonDepth
 *      subtracts the queue's own counters, so a core holding a thousand passes
 *      contributes a thousand. It used to contribute 1, because presence was
 *      all that was asked for — which is how a board came to report "1 unit(s)
 *      of storage work stalled" while saying nothing about how much that was,
 *      and why a measure meant to show PROGRESS could sit still while a queue
 *      genuinely drained from nine hundred passes to one.
 *   2. AHCI command issue, which is a real depth: one bit per occupied slot in
 *      PxCI (AHCI 1.3.1 §3.3.14) and PxSACT (§3.3.13), each cleared by the
 *      controller when that command completes.
 *   3. The volume's changes no Settle cycle has settled yet (inked_gen −
 *      settled_gen), and whether another core holds the Settle — the halt
 *      runs the last cycle itself, once nothing else is owed (RT-12). A
 *      cycle settles nothing until its last step, so the steps the cycles
 *      take are counted beside the terms (Steps), not in them: while they
 *      are taken the work is moving, however long the cycle is.
 *   4. The holders and waiters of every file's Helm: jobs that still owe a
 *      program its write, span or defrag. A waiter handed the Helm to run at a
 *      service point is run by the drain itself too. A job lowers the term
 *      only when it ends, so the writes of the volume the medium answers are
 *      counted beside the terms too (Writes): a window's, as a step's.
 * Every term is non-negative and falls as the machine finishes work — which is
 * exactly what lets the drain below run without a deadline while they fall.
 * And whether the newest settled cycle sealed the volume clean (Clean): the
 * halt owes the medium that seal once no change is in flight.
 *
 * AhciPresent is asked separately from Ahci because the two answer different
 * questions and the console line says which one it is looking at: a controller
 * that is initialised and idle owes nothing at this instant, while a machine
 * with no controller can never owe anything through that term at all. */
typedef struct HaltWorkOwed {
    uint64_t Total;       /* Baton + Ahci + Unsettled + Committer + Helm — the progress measure */
    uint64_t Baton;       /* passes handed to a core and not yet run        */
    uint64_t Ahci;        /* command slots the controller has not completed */
    uint64_t Unsettled;   /* the volume's changes no Settle cycle has settled */
    uint64_t Committer;   /* 1 while another running core holds the Settle  */
    uint64_t Helm;        /* holders and waiters of the files' Helms        */
    uint64_t Steps;       /* steps the Settle cycles took, on any core — not in Total */
    uint64_t Writes;      /* writes of the volume the medium answered — not in Total */
    bool     Clean;       /* the newest settled cycle sealed the volume clean */
    bool     AhciPresent; /* an AHCI controller was initialised at boot     */
} HaltWorkOwed;

static void halt_async_work_owed(HaltWorkOwed *owed)
{
    owed->Baton       = 0;
    owed->Ahci        = 0;
    owed->AhciPresent = ahci_is_initialized();

    for (uint8_t i = 0; i < g_amp.total_cores; i++)
        owed->Baton += BatonDepth(i);

    if (owed->AhciPresent) {
        uint32_t mask = ahci_get_active_port_mask();
        for (uint8_t p = 0; p < 32 && mask; p++) {
            if (!(mask & (1u << p))) continue;
            mask &= ~(1u << p);
            volatile ahci_port_regs_t *regs = ahci_get_port_regs_pub(p);
            if (!regs) continue;
            uint32_t ci   = regs->ci;
            uint32_t sact = regs->sact;
            owed->Ahci += CountSetBits(ci | sact);
        }
    }

    bool busy = false;
    SettleOwed(&owed->Unsettled, &busy, &owed->Clean);
    owed->Committer = busy ? 1u : 0u;
    owed->Helm      = HelmOwed();
    owed->Steps     = SettleStepsTaken();
    owed->Writes    = VolumeWritesAnswered();

    owed->Total = owed->Baton + owed->Ahci + owed->Unsettled + owed->Committer + owed->Helm;
}

/* The core carrying the most un-run passes, and the continuation most recently
 * handed to it. A stall that can name that continuation tells the next reader
 * WHAT the machine is waiting on, not only how long it has waited. False when
 * no core holds a pass — the depth is re-read here, so a queue that drained
 * between the measure and this look reports nothing rather than a stale count.
 */
static bool halt_deepest_baton(uint8_t *core_out, uint64_t *depth_out,
                               void (**run_out)(void *ctx))
{
    uint64_t deepest = 0;
    uint8_t  where   = 0;

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        uint64_t depth = BatonDepth(i);
        if (depth <= deepest) continue;
        deepest = depth;
        where   = i;
    }

    if (deepest == 0)
        return false;

    void (*handed_in)(void *ctx) = NULL;
    void (*last_run)(void *ctx)  = NULL;
    BatonLastSeen(where, &handed_in, &last_run);

    *core_out  = where;
    *depth_out = deepest;
    *run_out   = handed_in;
    return true;
}

/* The drain's glass ran out on work that stopped moving: said — how long, and
 * WHICH core, HOW MANY and WHOSE work, the three things the old line could not
 * say; a stuck queue names the continuation it is standing in, so the next
 * reader starts at a function instead of at a number — and then, when the
 * volume is still owed one, its last cycle run all the same. That cycle needs
 * none of what stopped moving: what the programs were told is written goes to
 * the medium before cli. A cycle another core holds is waited for to its end
 * first, as the roll call would wait for it. */
static void halt_drain_give_up(const HaltWorkOwed *owed, HourGlass *stall, uint64_t bound,
                               bool owes_cycle)
{
    kprintf("[HALT] storage work has not moved for %lu us of %lu (%s) — "
            "%lu unit(s) still owed\n",
            HourGlassElapsedUs(stall), bound,
            HourGlassSourceDied(stall) ? "ruler died" : HourGlassRulerName(stall),
            owed->Total);

    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        uint64_t depth = BatonDepth(i);
        if (depth == 0) continue;

        void (*handed_in)(void *ctx) = NULL;
        void (*last_run)(void *ctx)  = NULL;
        BatonLastSeen(i, &handed_in, &last_run);

        NameplateName in_name, run_name;
        kprintf("[HALT]   core %u holds %lu pass(es); last handed in "
                "%s, last run %s\n",
                i, depth,
                halt_name_of(handed_in, &in_name),
                halt_name_of(last_run, &run_name));
    }

    kprintf("[HALT] giving up; a pending write's commit may be lost\n");
    if (owes_cycle) {
        kprintf("[HALT] the volume's last Settle cycle runs without the work that "
                "stopped moving\n");
        (void)TagFSSettleForHalt();
    }
}

bool HaltDrainMoved(HaltDrainSeen *seen, uint64_t owed, uint64_t steps, uint64_t writes)
{
    bool moved = owed < seen->Best || steps != seen->Steps || writes != seen->Writes;
    if (owed < seen->Best)
        seen->Best = owed;
    seen->Steps  = steps;
    seen->Writes = writes;
    return moved;
}

/* Whether the halt owes the volume a cycle of its own: changes no cycle
 * settled, its first cycle not run yet, or — once no change is in flight — a
 * seal that is not clean, the halt's last word on the medium. Never after a
 * cycle the medium refused: nothing the halt can do then is a cycle. */
static bool halt_owes_cycle(const HaltWorkOwed *owed, bool settled, bool refused)
{
    return !refused && (owed->Unsettled > 0 || !settled || (owed->Helm == 0 && !owed->Clean));
}

/* A stall of the drain: the glass, the deadline it was turned on, and what
 * has been said of it. Each bound announces itself once. A stall episode
 * begins every time a pass that moved is followed by one that did not, and on
 * a queue of a thousand passes that can happen a thousand times — one line per
 * episode would bury the log it exists to clarify. There are two bounds, so
 * there are at most two lines. */
typedef struct HaltDrainStall {
    HourGlass Glass;
    uint64_t  Bound;      /* the deadline the glass was turned on */
    bool      Stuck;      /* the last pass did not move           */
    bool      Ruled;      /* the glass turned on a real ruler     */
    bool      Said;       /* the no-ruler line, said exactly once */
    bool      ToldAhci;
    bool      ToldCore;
} HaltDrainStall;

/* The first pass that did not move: the glass turned on the deadline of
 * whatever holds the work, and that said. When both a port and a queue hold
 * work, the LONGER of the two applies: the passes may be the completions that
 * command is going to hand on, and cutting them off at the core's ceiling
 * would abandon a disk that is still answering within its own. */
static void halt_drain_stall_begins(HaltDrainStall *s, const HaltWorkOwed *owed)
{
    bool by_ahci = (owed->Ahci > 0) &&
                   (owed->Baton == 0 || HALT_STALLED_AHCI_US >= HALT_STALLED_CORE_US);
    s->Stuck = true;
    s->Bound = by_ahci ? HALT_STALLED_AHCI_US : HALT_STALLED_CORE_US;

    if (by_ahci && !s->ToldAhci) {
        s->ToldAhci = true;
        kprintf("[HALT] work owed stopped falling at %lu (%lu pass(es), "
                "%lu AHCI command(s) still issued) — judged by the AHCI "
                "command deadline, %lu us, because a controller that is "
                "here owes them\n",
                owed->Total, owed->Baton, owed->Ahci, s->Bound);
    } else if (!by_ahci && !s->ToldCore) {
        s->ToldCore = true;
        kprintf("[HALT] work owed stopped falling at %lu (%lu pass(es), "
                "%lu unsettled change(s), %lu Helm job(s), %s, %s) — judged "
                "by the core deadline, %lu us: a baton queue falls only when "
                "its own core pumps it\n",
                owed->Total, owed->Baton, owed->Unsettled, owed->Helm,
                owed->Committer ? "a Settle cycle running on another core"
                                : "no Settle cycle running elsewhere",
                owed->AhciPresent ? "no AHCI command issued"
                                  : "no AHCI controller on this machine",
                s->Bound);

        uint8_t  core  = 0;
        uint64_t depth = 0;
        void (*handed_in)(void *ctx) = NULL;
        if (halt_deepest_baton(&core, &depth, &handed_in)) {
            NameplateName in_name;
            kprintf("[HALT]   waiting on core %u: %lu pass(es), last "
                    "handed in %s\n",
                    core, depth, halt_name_of(handed_in, &in_name));
        }
    }

    s->Ruled = HourGlassTurn(&s->Glass, s->Bound);
    if (!s->Ruled && !s->Said) {
        /* No ruler on this machine at this point in boot. A missing ruler
         * never becomes an invented one: we wait on the fact itself — the
         * queues and the port registers going quiet — and never on a guessed
         * spin count. A genuine wedge here is what Nightwatch convicts, by
         * facts, without a clock. */
        kprintf("[HALT] no time ruler — draining against the queues "
                "themselves, unbounded\n");
        s->Said = true;
    }
}

/*
 * Drain in-flight async storage I/O before we kill cores — bounded by PROGRESS
 * rather than by a duration.
 *
 * This used to wait three seconds and then print "drained" whether or not it
 * was true, which on a real disk means abandoning a WRITE completion that still
 * owed its tagfs commit. The wrong question was being asked: how long a queue
 * of writes takes is a property of the disk and of how much is queued, and no
 * number written here can know either of those.
 *
 * The right question is whether the machine is still getting somewhere. Each
 * pass measures the work owed. While that measure reaches a new LOW, the loop
 * runs with no deadline at all — a non-negative integer can set a new low only
 * finitely many times, so the loop terminates by construction and there is
 * nothing a clock could usefully judge. A clock is needed only once the measure
 * stops falling, and then what it judges is not the shutdown but ONE THING THAT
 * HAS STOPPED ANSWERING — so the glass is turned on the deadline of whatever is
 * actually holding the work (HALT_STALLED_AHCI_US / HALT_STALLED_CORE_US), and
 * the console is told which one and why. The glass is put away again the moment
 * progress resumes, so a slow-but-working disk is never cut off no matter how
 * long the queue is.
 *
 * "New low" rather than "lower than last pass" on purpose: a measure that
 * oscillates (a commit continuation issuing a fresh command as another
 * completes) would satisfy "fell since last pass" forever and the loop would
 * never terminate. Against the running minimum it cannot.
 *
 * ‼ A SETTLE CYCLE AND A WRITE MOVE WITHOUT LOWERING ANYTHING. A cycle
 * settles its changes in its last step, and one that writes a pool of
 * thousands of blocks to a cheap stick takes longer than any core's ceiling;
 * a job that holds a file's Helm lowers its term only when it ends, its
 * window a command or two the medium answers in its own time. Judged by the
 * measure alone, both were given up on in the middle — on four cores,
 * measured: the glass turned as the committer's cycle ended ran on through
 * the halt's own two cycles, three seconds of steps nobody counted, and ran
 * out while a writer's window was taking the medium's answers. So a step any
 * cycle takes, the halt's own too, and a write of the volume the medium
 * answers put the glass away as a new low does (HaltDrainMoved); only work
 * that stopped taking either is judged. That ends too: no cycle begins on its
 * own once the drain has begun — the cadence is held for good here — no change
 * begins either (TagFSShutForHalt), and a cycle has finitely many steps, a
 * change begun finitely many writes.
 *
 * Other cores are still live here — cli has not run yet — so their guide loops
 * keep pumping continuations naturally.
 *
 * ‼ AND THE VOLUME'S LAST SETTLE CYCLE IS RUN HERE, BY THE HALT ITSELF (RT-12).
 * Once no pass, no command and no other committer is owed, whatever the volume
 * inked and no cycle settled is settled by this core — before cli, before any
 * core is stopped: a committer frozen in the middle of the medium could never
 * be finished, and a halt after cli could wait on nobody. Programs still run
 * until the roll call, but from here the volume takes no change they have not
 * begun: one begun runs to its end and asks for another cycle, and the cycle
 * after the last of them — the first no change is in flight under — seals the
 * volume clean. A change begun after the last cycle was on nobody's books, and
 * a write in place among them landed under the clean seal with the digest of
 * the bytes before it — rot on the next mount, measured on four cores. A drain
 * that gives up on work that stopped moving still runs a cycle before it
 * goes, unclean while a change is in flight: the cycle needs none of the
 * passes another core never pumped, and without it every change since the
 * last cycle stays off the medium.
 */
static void halt_drain_async_writes(void)
{
    kprintf("[HALT] Draining async writes...\n");

    /* From here the halt's own cycles are the volume's last: a cycle the
     * cadence began after them would seal the volume unclean and stamp a
     * Spare nothing names, and one begun meanwhile would hold the drain. And
     * no change begins: the work owed is what was begun before this line. */
    SettleCadenceHold(true);
    TagFSShutForHalt();

    uint8_t        me        = amp_get_core_index();
    HaltDrainSeen  seen      = { .Best = UINT64_MAX, .Steps = UINT64_MAX, .Writes = UINT64_MAX };
    HaltDrainStall stall     = { .Stuck = false };
    bool           told_step = false;    /* a cycle on another core, said once */
    bool           settled   = false;    /* the halt's own Settle cycle has run once */
    bool           refused   = false;    /* ... and the last one was refused */

    for (;;) {
        /* Pump THIS core's own queues before measuring. system_halt runs in
         * guide-loop context on whichever K-Core picked up the reboot/poweroff
         * pocket — and that can be the BSP, which is the SOLE consumer of the
         * never-drop storage completion queue (all completions route there).
         * While we spin here the BSP is no longer in kcore_run_loop, so nothing
         * else can drain its queue: a completion that lands during the drain
         * (interrupts are still on before the cli below) would be held forever
         * and its write's tagfs-commit continuation lost. Draining our own
         * index each pass is a valid single-consumer pump (me == this core) and
         * a harmless empty early-out on a non-drain core; other cores keep
         * pumping their own queues from their guide loops. */
        BatonPump(me);
        /* And the Helm work handed to a service point: this core is standing
         * in the drain, not at its loop's service point, and on a machine with
         * one K-Core no other core would ever run it. */
        HelmRunReady();

        HaltWorkOwed owed;
        halt_async_work_owed(&owed);
        if (owed.Baton == 0 && owed.Ahci == 0 && owed.Committer == 0 &&
            halt_owes_cycle(&owed, settled, refused)) {
            refused = TagFSSettleForHalt() != OK;
            settled = true;
            halt_async_work_owed(&owed);
        }
        /* A cycle the medium refused leaves nothing the halt can do for what
         * it did not settle: tagfs_shutdown says how much that is. */
        if (refused)
            owed.Total -= owed.Unsettled;
        if (owed.Total == 0 && !halt_owes_cycle(&owed, settled, refused))
            break;

        if (owed.Committer && !told_step) {
            told_step = true;
            kprintf("[HALT] a Settle cycle runs on another core — the drain waits for it as "
                    "long as it takes steps\n");
        }
        if (HaltDrainMoved(&seen, owed.Total, owed.Steps, owed.Writes)) {
            stall.Stuck = false;    /* progress — the clock goes back in the pocket */
        } else if (!stall.Stuck) {
            halt_drain_stall_begins(&stall, &owed);
        } else if (stall.Ruled && HourGlassRunOut(&stall.Glass)) {
            halt_drain_give_up(&owed, &stall.Glass, stall.Bound,
                               halt_owes_cycle(&owed, settled, refused));
            return;                 /* do not claim a drain that did not happen */
        }

        __asm__ volatile("pause");
    }

    kprintf("[HALT] Async writes drained\n");
}

/* How long a core may take to answer an IPI before it is a fault — and this
 * kernel has already written that number down once, for exactly this question:
 * CONFIG_TLB_SHOOTDOWN_PANIC_MS, the ceiling shootdown_wait_acks uses before it
 * declares a cross-core deadlock. An IPI is delivered in microseconds and waits
 * in the target's LAPIC IRR until that core re-enables interrupts, so nothing
 * legitimate keeps a core from ANSWERING for seconds. DECLARED POLICY, reused
 * so the kernel has one answer to "a core is not responding", not two.
 *
 * It bounds the answer and nothing after it. A core that answered by owing its
 * stop is running a held span to its end — a Scribe pass, a command it stands
 * over — and that may legitimately take as long as its medium's own patience;
 * the roll call waits for that end however long it is, because the core is
 * neither silent nor stuck, and forcing its locks open or calling it offline
 * while it still runs would tear the structures its span exists to keep
 * whole. */
#define HALT_AP_ARRIVAL_US ((uint64_t)CONFIG_TLB_SHOOTDOWN_PANIC_MS * 1000ULL)

volatile bool g_halt_orderly = false;

void HaltFreezeHere(void)
{
    __asm__ volatile("cli" ::: "memory");
    /* No longer running, said at once rather than when the roll call ends: a
     * door or a turn a running core would wait for an owner to give back
     * (ahci_roadstead.c), and a shootdown that would wait for its ACK, both
     * ask amp_core_online — and a core that owes its stop may still be
     * running its span beside this one for as long as that takes. */
    uint8_t me = amp_get_core_index();
    __atomic_store_n(&g_amp.cores[me].online, (uint8_t)0, __ATOMIC_RELEASE);
    /* The core's own word that it arrived, and the last thing it does: the
     * release store pairs with the roll call's acquire load, and nothing of
     * this core's is in flight after it. */
    __atomic_store_n(&g_amp.cores[me].halted, (uint8_t)1, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile("cli; hlt");
}

void HaltStopIfOwedHere(const char *after)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");

    uint8_t core = amp_get_core_index();
    if (__atomic_load_n(&g_amp.cores[core].freeze_owed, __ATOMIC_RELAXED) &&
        !SchedulerHeldHere())
    {
        uint64_t waited = rdtsc() - g_amp.cores[core].freeze_asked_tsc;
        kprintf("[HALT] core %u stops after %s, %lu us after the halt asked it "
                "to stop\n", (unsigned)core, after,
                (unsigned long)cpu_tsc_to_us(waited));
        HaltFreezeHere();
    }

    if (flags & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
}

void halt_all_ap_cores(bool orderly)
{
    if (!g_amp.multicore_active)
        return;

    kprintf("[HALT] Stopping all AP cores...\n");

    uint8_t my_index = amp_get_core_index();

    /* Who is being asked, sampled BEFORE the broadcast. A core that was already
     * offline never had to answer, and holding it to this roll call would mean
     * waiting on a core that is not running. Each asked core's arrival word is
     * cleared first, so an answer left over from an earlier halt broadcast
     * cannot be read as an answer to this one. 256 bytes of stack against a
     * 16 KB kernel stack — the array is the cheap part. */
    bool asked[MAX_CORES];
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        asked[c] = (c != my_index) && amp_core_online(&g_amp.cores[c]);
        if (asked[c])
            __atomic_store_n(&g_amp.cores[c].halted, (uint8_t)0, __ATOMIC_RELAXED);
    }

    /* Raised before the IPI can land anywhere: a core inside a held span — a
     * Scribe pass, a command it stands over on a medium — must already know,
     * when the IPI reaches it, that this stop may wait for the end of it. The
     * roll call below waits for that end without the arrival bound: that core
     * answered. */
    if (orderly)
        __atomic_store_n(&g_halt_orderly, true, __ATOMIC_RELEASE);

    lapic_send_ipi_all_excluding_self(IPI_HALT_VECTOR);

    /* Wait for the FACT, not for a guessed five milliseconds. Every asked core
     * publishes its own arrival from the IPI_HALT branch of the interrupt
     * handler with a release store, which the acquire load below pairs with.
     * The old code slept 5 ms and then ASSUMED — so a core that answered a
     * moment late was treated exactly like a core that answered, and a core
     * that never answered at all was never named. */
    HourGlass roll;
    bool ruled = HourGlassTurn(&roll, HALT_AP_ARRIVAL_US);
    if (!ruled)
        kprintf("[HALT] no time ruler — waiting for every AP to publish its "
                "halt, unbounded\n");

    /* A core that answered by owing its stop (freeze_owed) is running its
     * span to its end, and is waited for past the bound; only a core that
     * never answered at all is held to it. While one is owed, this core — when
     * it is the drain core, as it is for the power button's halt and for any
     * halt on one K-Core — runs the continuations such a span may be waiting
     * for (an async USB job's steps run nowhere else). Interrupts are off
     * here, so a shootdown a running span starts is answered by hand. */
    for (;;)
    {
        bool silent = false;
        bool owing  = false;
        for (uint8_t c = 0; c < g_amp.total_cores; c++)
        {
            if (!asked[c]) continue;
            if (__atomic_load_n(&g_amp.cores[c].halted, __ATOMIC_ACQUIRE)) continue;
            if (__atomic_load_n(&g_amp.cores[c].freeze_owed, __ATOMIC_ACQUIRE))
                owing = true;
            else
                silent = true;
        }
        if (!silent && !owing) break;
        if (!owing && ruled && HourGlassRunOut(&roll)) break;
        if (owing && my_index == g_amp.bsp_index)
            BatonPump(my_index);
        vmm_tlb_shootdown_poll();
        __asm__ volatile("pause");
    }

    uint32_t absent = 0;
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        if (!asked[c]) continue;
        if (__atomic_load_n(&g_amp.cores[c].halted, __ATOMIC_ACQUIRE)) continue;
        absent++;
        kprintf("[HALT] core %u (lapic 0x%x) never answered the halt IPI "
                "(%lu us, %s)\n",
                (uint32_t)c, g_amp.cores[c].lapic_id, HourGlassElapsedUs(&roll),
                HourGlassSourceDied(&roll) ? "ruler died"
                                           : HourGlassRulerName(&roll));
    }

    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        if (c == my_index)
            continue;
        /* Release store pairs with amp_core_online's acquire load on the
         * other side, so peers that still iterate g_amp.cores[] see this
         * core leave the "online" set with proper cross-CPU visibility. A core
         * that stopped already said so itself (HaltFreezeHere); this is for
         * the ones that never answered. */
        __atomic_store_n(&g_amp.cores[c].online, (uint8_t)0, __ATOMIC_RELEASE);
    }

    if (absent == 0)
    {
        /* Nothing to force. A core that published its arrival was not inside a
         * stretch that kept interrupts off: spin_lock does `cli` before it
         * acquires and restores RFLAGS only in spin_unlock, so IPI_HALT waits
         * in that core's LAPIC IRR until the lock is released — for every
         * section that does not turn interrupts back on itself. process_lock
         * and the cleanup queue's lock are such sections, so a complete roll
         * call proves them unheld by any AP. So are the ones a legacy-IDE DMA
         * wait runs under: the wait takes the drive's answer itself
         * (BmideAnswerHere, drivers/disk/ata_async.c) and never turns
         * interrupts on for the BSP's, so a core in it answers only after its
         * caller's lock is let go. */
        kprintf("[HALT] All AP cores stopped (roll call complete)\n");
        return;
    }

    /* A deliberate last resort, and nothing makes it safe.
     *
     * The tempting repair is to INIT-IPI the stragglers and call their locks
     * reclaimable afterwards. It is not a repair. An INIT IPI stops a core
     * EXECUTING; it does nothing about the half-finished mutation that core was
     * in the middle of. A torn process record stays torn, and a lock taken away
     * from a core stopped mid-write is exactly as unsound after the INIT as
     * before it. So this path does not pretend: the cores that did not answer
     * are named above, and the shutdown walk below may now read a structure one
     * of them was midway through changing. We force anyway because the only
     * alternative is to spin forever on a lock whose holder will never return,
     * and this machine is being powered off either way. */
    kprintf("[HALT] %u AP core(s) unaccounted for — forcing their locks open; "
            "structures they were mutating may be torn\n", absent);
    process_force_release_locks_for_shutdown();

    kprintf("[HALT] All AP cores stopped\n");
}

static void halt_detach_all_schedulers(void)
{
    // Clear current_process on every core's scheduler, and the claim (on_cpu)
    // every core holds on the strand it runs or was dispatching.
    // AP cores are already in cli;hlt — they will never touch these again.
    // This prevents process_destroy() from refusing to destroy a strand a
    // halted core still counts as its own: it refuses one that is current
    // anywhere, and one that is claimed (see process_destroy).
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        scheduler_state_t *s = scheduler_get_core(c);
        s->current_process = NULL;
    }

    process_list_lock();
    for (process_t *p = process_get_first(); p; p = p->next)
        __atomic_store_n(&p->on_cpu, (int16_t)-1, __ATOMIC_RELEASE);
    process_list_unlock();
}

/* The pid of the process at `index` in the list, or false when the list is
 * shorter than that. The walk holds process_list_lock and the caller's work
 * does not: process_find and process_destroy both take that same lock, so the
 * list may only be held while it is being READ. Re-finding by pid afterwards is
 * the price, and it is also the only safe handle — process_destroy may have
 * freed the struct the pointer referred to. */
static bool halt_pid_at(uint32_t index, uint32_t *pid_out)
{
    process_list_lock();
    process_t *p = process_get_first();
    for (uint32_t skip = 0; p && skip < index; skip++)
        p = p->next;
    bool any = (p != NULL);
    if (any)
        *pid_out = p->pid;
    process_list_unlock();
    return any;
}

static void halt_terminate_all_processes(void)
{
    kprintf("[HALT] Terminating all processes...\n");

    // Detach all processes from scheduler before destruction
    halt_detach_all_schedulers();

    process_cleanup_queue_flush();

    /*
     * Every process, not the first 256 of a list whose real bound is
     * PROCESS_MAX_COUNT. The old sweep snapshotted 256 pids and killed those;
     * on a machine carrying more, the rest were simply never told to stop.
     *
     * The obvious repair — destroy process_get_first() until the list is empty
     * — is worse than the truncation it replaces. process_destroy REFUSES some
     * processes (an out-of-range home_core, a strand a core still counts as
     * current), and on one of those the repair re-reads the same head forever
     * and the machine never powers off.
     *
     * So: sweep in PASSES that must make progress. A pass walks the whole list,
     * acts on each entry, and counts how many entries actually LEFT the list.
     * Entries that refused stay put, and because nothing is inserted they
     * accumulate at the front of the not-yet-visited region — so `stayed` is
     * exactly how far the next look must skip. A pass that removes nothing ends
     * the sweep, and the survivors are then named.
     *
     * Both loops terminate by construction: `stayed` only grows and the list
     * only shrinks, so the inner loop advances every iteration; and each outer
     * pass either removes at least one process or is the last. The cost is
     * O(list) walks per pass, up to PROCESS_MAX_COUNT of them — microseconds on
     * a list of this size, and it buys a sweep with no snapshot array, no
     * arbitrary batch size, and no truncation.
     *
     * The cursor arithmetic assumes ONE mutator — nobody inserts while we walk.
     * halt_all_ap_cores ran first, so that holds for every AP that answered the
     * halt IPI. It does NOT hold for an AP that did not answer; that core was
     * named out loud there, and a process it creates mid-sweep is one this
     * sweep may miss — the same exposure its torn locks already are.
     */
    uint32_t killed = 0;
    for (;;)
    {
        uint32_t removed = 0;
        uint32_t stayed  = 0;
        uint32_t pid     = 0;

        while (halt_pid_at(stayed, &pid))
        {
            process_t *victim = process_find(pid);
            if (victim)
            {
                process_set_state(victim, PROC_DONE);
                process_destroy(victim);
            }

            /* Did it leave the LIST? Look at the same position again: the slot
             * still holding the same pid means the entry refused and the cursor
             * must step over it. Asking the hash (process_find) instead would
             * answer a DIFFERENT question — a process the hash has lost but the
             * list still carries would read as "gone", the cursor would never
             * advance past it, and this loop would re-read that slot forever. */
            uint32_t still = 0;
            if (halt_pid_at(stayed, &still) && still == pid) stayed++;
            else                                             removed++;
        }

        killed += removed;
        if (removed == 0)
            break;
    }

    process_cleanup_queue_flush();

    kprintf("[HALT] %u processes terminated\n", killed);

    /* Whatever would not go, BY NAME. A count would say the sweep was
     * incomplete; the pids say which ones, which is what anyone reading this
     * line afterwards can actually act on. Printed outside the list lock, one
     * entry per look, because kprintf takes a lock of its own. */
    uint32_t at   = 0;
    uint32_t left = 0;
    bool     open = false;
    while (halt_pid_at(at, &left))
    {
        if (!open)
        {
            kprintf("[HALT] processes that would not stop:");
            open = true;
        }
        kprintf(" pid %u", left);
        at++;
    }
    if (open)
        kprintf("\n");
}

/*
 * ‼ THE LAST THING THIS MACHINE DOES FOR ITS DATA, AND IT USED TO SAY
 * "COMPLETE" WHETHER OR NOT IT WAS.
 *
 * "Flushing disk cache...", the answer dropped, "Storage sync complete", and
 * then the power goes. A drive that had refused the flush — or one whose port
 * was being flushed for somebody else at that instant and so was never asked
 * — went dark with the commit still in its cache, and the last line on the
 * screen said the opposite. On a machine that is read by photographing its
 * screen, that line was the only evidence there was, and it was false.
 *
 * ‼ AND WHAT IT WAITS FOR HERE HAS TO BE ABLE TO HAPPEN, BECAUSE OF WHERE
 * THIS STANDS.
 *
 * This runs after `cli` and after halt_all_ap_cores(), so there is exactly
 * one core executing on this machine and it is the one inside this function.
 * The flush below waits in the port's roadstead for the door, and nothing
 * else on this machine will ever run to open one — so every door and every
 * slot it waits behind must be one that gets given back from here:
 *
 *   - a core the halt's IPI found standing over a command owed the halt its
 *     stop, finished the command and gave its slot or its door back before it
 *     stopped (idt.c, HaltStopIfOwedHere);
 *   - a door still held by a core that is no longer running, over a device
 *     that owes nothing, is opened by the roadstead's own call-in;
 *   - completions whose interrupt cannot land now — this core has them shut —
 *     and a recovery a fault left owed are brought in and run by the waiting
 *     caller itself (ahci_roadstead.c, AhciTurn).
 *
 * So the single call below is the whole of the asking, and its answer is the
 * medium's own.
 *
 * Either way the line that follows says what actually happened. A shutdown
 * that could not make the volume durable is not a failed shutdown — the
 * machine still has to power off — but it is a shutdown that owes the
 * operator the truth about what may not be on the disk when it comes back.
 */
static void halt_sync_storage(void)
{
    kprintf("[HALT] Closing TagFS...\n");
    /*
     * ‼ AND THE ANSWER IS HEARD. Two different things can go wrong here and
     * only one of them used to be reported.
     *
     * tagfs_shutdown was void, so a shutdown that wrote NOT ONE BYTE of
     * metadata — because the volume's door was already shut, or because the
     * bitmap write was refused — returned exactly like one that wrote
     * everything, and the flush below then reported a perfectly successful
     * flush OF NOTHING. "Storage sync complete" was printed over it. It now
     * carries the answer of the halt's last Settle cycle, run before cli
     * (halt_drain_async_writes), and this is the caller that has to hear it,
     * because this is the last place anybody can be told.
     *
     * The two answers are kept apart on purpose: the volume's own records are
     * one question ("were they written at all?") and the medium's cache is
     * another ("did what was written reach the platter?"). A machine that
     * failed the first and passed the second is not a machine that synced.
     */
    bool    glass    = TagFSIsUnderGlass();
    error_t recorded = tagfs_shutdown();
    if (recorded != OK) {
        kprintf("[HALT] ‼ THE VOLUME ON SEAT %u (%s) DID NOT WRITE ITS RECORDS "
                "(error %u) — what follows flushes a medium that was not given "
                "them\n",
                tagfs_get_seat(), BoardroomSeatName(tagfs_get_seat()),
                (unsigned)recorded);
    }

    /* No volume named — none was found, the one found was refused at its
     * Deed, or the one mounted was let go of when its medium left (and its
     * waiters were told then). There is no medium here to commit, and asking
     * the flush anyway answered with the alarm for a volume this machine never
     * wrote. */
    if (tagfs_get_seat() == BOARDROOM_NO_SEAT) {
        kprintf("[HALT] no volume is named on this machine — there is no "
                "medium to make durable\n");
        return;
    }

    if (glass && recorded == OK) {
        TagFSGlassHaltSaid(tagfs_get_seat());
        return;
    }

    kprintf("[HALT] Flushing disk cache...\n");

    error_t flushed = tagfs_flush_cache();

    if (flushed == OK && recorded == OK) {
        kprintf("[HALT] Storage sync complete\n");
        return;
    }
    if (flushed == OK) {
        /* The medium took what it was given, and it was not given everything.
         * Said as its own sentence rather than folded into the line below,
         * which is about a flush that failed. */
        kprintf("[HALT] the medium on seat %u took what it was given, and the "
                "volume's own records were not part of it\n",
                tagfs_get_seat());
        return;
    }

    kprintf("[HALT] ‼ THE VOLUME ON SEAT %u (%s) WAS NOT MADE DURABLE "
            "(error %u) — everything committed to it since its last "
            "successful flush may not be there when this machine comes "
            "back\n",
            tagfs_get_seat(), BoardroomSeatName(tagfs_get_seat()),
            (unsigned)flushed);
}

static void halt_stop_hardware(void)
{
    for (uint8_t ci = 0; ci < xhci_controller_count(); ci++)
    {
        xhci_controller_t *ctrl = xhci_controller_at(ci);
        if (!ctrl || !ctrl->initialized)
        {
            continue;
        }
        kprintf("[HALT] Stopping USB controller %u...\n", ci);
        for (uint8_t p = 1; p <= ctrl->max_ports; p++)
        {
            xhci_disable_port(ctrl, p);
        }
    }

    /* Every pin the chip has, asked of the chip.
     *
     * This used to count to irqchip_max_irqs() and call irqchip_disable_irq
     * on each number — which answers with the width of a vector window and
     * routes each number through the ISA override table on the way in. On the
     * board in front of the owner that meant twenty-four of a hundred and
     * twenty inputs were quieted, and after a line's vector stopped being its
     * pin number it would have quieted twenty-four pins nobody asked about
     * while leaving the armed ones live through a poweroff. */
    kprintf("[HALT] Masking every interrupt input the chip has...\n");
    if (irqchip_get_type() == IRQCHIP_APIC)
    {
        IoapicMaskEveryPin();
    }
    else
    {
        /* An 8259 has sixteen inputs and always has: two chips of eight, the
         * second cascaded into the first's input 2. */
        for (uint8_t input = 0; input < ISA_INTERRUPT_REQUEST_LINES; input++)
        {
            irqchip_disable_irq(input);
        }
    }
}

/* The courtesy window userspace gets at poweroff. DECLARED POLICY, and said
 * plainly rather than dressed up as a measurement.
 *
 * It is policy because the honest-looking alternative is not available here:
 * waiting until every claimant has replied "ready" makes POWEROFF depend on
 * unprivileged userspace making progress, and one wedged daemon would then keep
 * the machine powered on forever. The machine does not owe userspace an
 * arbitrary amount of time at shutdown; it owes it a chance to be SCHEDULED.
 * At the configured tick rate (CONFIG_SCHED_DEFAULT_TICK_HZ, 250 Hz — a tick
 * every 4 ms) this window is a dozen ticks: a dozen chances for a woken
 * subscriber to be picked on its App-Core. A daemon that needs more than that
 * must checkpoint on every state change, not on shutdown alone — and whatever
 * it did manage to issue is not at the mercy of this number anyway, because the
 * drain that follows waits on the work itself and not on a clock. */
#define HALT_GRACE_MS 50u

/* The halt's own stack.
 *
 * ‼ THE HALT USED TO RUN ON WHATEVER STACK CALLED IT, AND ON ONE CORE THAT IS
 * THE STACK IT FREES. A single core answers a system call on the caller's own
 * kernel stack (idt.c, sync_syscall_dispatch), so `bye` ran this whole file on
 * bye's stack — and halt_terminate_all_processes destroys bye with every other
 * process, hands that stack to the PMM and goes on running on it. MEASURED on
 * b6f36153 by a probe in process_free_kernel_stack: "freeing the kernel stack
 * this core runs on: pid 5". Whether the machine then powered off or jumped to
 * address 0 depended only on whether anything reused those frames before the
 * next `ret`; the display's teardown did, and a `bye` ended in a #PF at RIP 0.
 *
 * So the halt stands on a stack nobody owns and nothing frees: here, in the
 * image, so that having it cannot fail and asks neither the allocator nor a
 * shootdown of the very cores the halt is stopping. Twice the 16 KiB a K-Core
 * runs this path on. No guard page, for the same reason: making one is
 * page-table work answered by those cores. */
static uint8_t g_halt_stack[32u * 1024u] __attribute__((aligned(64)));

static __attribute__((noreturn, used)) void HaltMachine(bool reboot);

/* Onto g_halt_stack and into HaltMachine, never to come back: the stack this
 * leaves may be freed under it. */
static __attribute__((noreturn)) void HaltOnOwnStack(bool reboot)
{
    __asm__ volatile("mov %[top], %%rsp\n\t"
                     "xor %%ebp, %%ebp\n\t"
                     "call %P[run]\n\t"
                     "ud2"
                     :
                     : [top] "r"(g_halt_stack + sizeof(g_halt_stack)),
                       [run] "i"(HaltMachine),
                       "D"((uint64_t)reboot)
                     : "memory");
    __builtin_unreachable();
}

void system_halt(bool reboot)
{
    /* One halt. A second — the power button's continuation on one core while
     * `bye` is answered on another — would share the one stack, so it stands
     * here the way the halt's roll call would stand that core. */
    static uint32_t halting;
    if (__atomic_exchange_n(&halting, 1u, __ATOMIC_ACQ_REL))
        HaltFreezeHere();
    HaltOnOwnStack(reboot);
}

static void HaltMachine(bool reboot)
{
    /* Publish system:shutdown/system:reboot BEFORE we cli, so subscribers
     * (e.g. user daemons holding open files / dirty caches) get woken on
     * their App-Cores while interrupts and the scheduler still work. The
     * grace_ms field carries HALT_GRACE_MS itself, so the window a subscriber
     * is told about is the window it actually gets. */
    {
        struct __attribute__((packed)) {
            uint32_t reason;
            uint32_t grace_ms;
        } hint = { 0u, HALT_GRACE_MS };
        TouchPublish(reboot ? "system:reboot" : "system:shutdown",
                     &hint, sizeof(hint));
    }

    /* The courtesy window, turned as real time instead of spun as a TSC count
     * that is only as real as the last calibration. */
    {
        HourGlass grace;
        if (!HourGlassTurn(&grace, (uint64_t)HALT_GRACE_MS * 1000ULL)) {
            /* No ruler on this machine: there is no way to grant a window of
             * real time, and the one fact we might have waited on instead is
             * userspace making progress — which this path must never wait on,
             * bounded or not (see HALT_GRACE_MS). So the event is published,
             * the window is skipped, and it is said out loud rather than
             * approximated with a guessed iteration count. */
            kprintf("[HALT] no time ruler — shutdown grace window skipped\n");
        } else {
            while (!HourGlassRunOut(&grace))
                __asm__ volatile("pause");
        }
    }

    /* Drain BEFORE cli — other cores need their guide loops alive to
     * pump pending continuations from the AHCI IRQ. */
    halt_drain_async_writes();

    /* What the interrupts said this session and what of it was delivered —
     * the one place the account is complete, because nothing knocks after
     * the drain above and the reader is done. */
    TouchIrqRingAccount();

    __asm__ volatile("cli");

    kprintf("%s initiated\n", reboot ? "reboot" : "shutdown");

    /* From here on, every rung of the ladder gets its own
     * WireForceRelease()+WireDrain() pair instead of one at the very end.
     * This is a one-way path: halt_stop_hardware() never returns to a
     * caller that could drain for it, and acpi_reboot()/acpi_shutdown()
     * below may reset or park the CPU before the next rung ever runs. A
     * message queued behind a rung that never finishes is a message nobody
     * reads. ForceRelease always comes before Drain: halt_all_ap_cores()
     * can freeze another core with IPI_HALT while it is mid-kprintf,
     * still holding g_serial_lock, and that core is never coming back to
     * release it — WireDrain() alone would spin forever on a lock its
     * holder abandoned. */
    WireForceRelease();
    WireDrain();

    halt_all_ap_cores(true);
    WireForceRelease();
    WireDrain();

    halt_terminate_all_processes();
    WireForceRelease();
    WireDrain();

    halt_sync_storage();
    WireForceRelease();
    WireDrain();

    halt_stop_hardware();
    WireForceRelease();
    WireDrain();

    kprintf("[HALT] Cleanup complete.\n");
    kprintf(reboot ? "[HALT] Rebooting...\n" : "[HALT] Powering off...\n");

    /* The last words onto the wire before the machine goes into firmware —
     * interrupts are off, so the line cannot drive itself from here. */
    WireForceRelease();
    WireDrain();

    /* And the same last words into memory that outlives the reset. The wire
     * reaches a cable; this reaches the next boot, which is the only reader a
     * machine on a bench has. It must be the LAST thing said, because
     * everything after this line happens inside firmware and is not ours to
     * record. Does nothing in a build without CONFIG_PRINTTOFILE. */
    LogKeepFlush();

    if (reboot) acpi_reboot();
    else        acpi_shutdown();

    __builtin_unreachable();
}
