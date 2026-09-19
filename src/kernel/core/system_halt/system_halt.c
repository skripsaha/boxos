#include "serial.h"      /* WireDrain — the last words are heard */
#include "system_halt.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"
#include "process.h"
#include "scheduler.h"
#include "tagfs.h"
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
#include "kernel_config.h"
#include "nameplate.h"        /* nameplate_name_at_kernel — name a stuck continuation */
#include "nameplate_format.h" /* NameplateHeader — the table's own minimum size */

/* The only bound in the drain below, and it judges a DEVICE, not a shutdown.
 * CONFIG_AHCI_IO_TIMEOUT_MS is this kernel's already-written answer to "how
 * long may one storage command legitimately take before it is declared
 * non-completing" — the industry disk deadline the AHCI async watchdog uses.
 * Reused rather than invented, so the kernel holds one answer to that question
 * instead of two that can drift apart. */
#define HALT_STALLED_DEVICE_US ((uint64_t)CONFIG_AHCI_IO_TIMEOUT_MS * 1000ULL)

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

static inline uint32_t halt_bits_set(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24;
}

/* How much async storage work the machine still owes, counted the only two
 * ways it can owe any:
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
 * Both terms are non-negative and fall as the machine finishes work — which is
 * exactly what lets the drain below run without a deadline while they fall. */
static uint64_t halt_async_work_owed(void)
{
    uint64_t work = 0;

    for (uint8_t i = 0; i < g_amp.total_cores; i++)
        work += BatonDepth(i);

    if (ahci_is_initialized()) {
        uint32_t mask = ahci_get_active_port_mask();
        for (uint8_t p = 0; p < 32 && mask; p++) {
            if (!(mask & (1u << p))) continue;
            mask &= ~(1u << p);
            volatile ahci_port_regs_t *regs = ahci_get_port_regs_pub(p);
            if (!regs) continue;
            uint32_t ci   = regs->ci;
            uint32_t sact = regs->sact;
            work += halt_bits_set(ci | sact);
        }
    }

    return work;
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
 * stops falling, and then what it judges is not the shutdown but one device
 * that has stopped answering: HALT_STALLED_DEVICE_US. The glass is put away
 * again the moment progress resumes, so a slow-but-working disk is never cut
 * off no matter how long the queue is.
 *
 * "New low" rather than "lower than last pass" on purpose: a measure that
 * oscillates (a commit continuation issuing a fresh command as another
 * completes) would satisfy "fell since last pass" forever and the loop would
 * never terminate. Against the running minimum it cannot.
 *
 * Other cores are still live here — cli has not run yet — so their guide loops
 * keep pumping continuations naturally.
 */
static void halt_drain_async_writes(void)
{
    kprintf("[HALT] Draining async writes...\n");

    uint8_t   me    = amp_get_core_index();
    uint64_t  best  = UINT64_MAX;   /* lowest work owed ever seen — the witness */
    bool      stuck = false;        /* the last pass failed to beat `best`      */
    bool      ruled = false;        /* the stall glass turned on a real ruler   */
    bool      said  = false;        /* the no-ruler line is said exactly once   */
    HourGlass stall;

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

        uint64_t work = halt_async_work_owed();
        if (work == 0)
            break;

        if (work < best) {
            best  = work;
            stuck = false;          /* progress — the clock goes back in the pocket */
        } else if (!stuck) {
            stuck = true;
            ruled = HourGlassTurn(&stall, HALT_STALLED_DEVICE_US);
            if (!ruled && !said) {
                /* No ruler on this machine at this point in boot. A missing
                 * ruler never becomes an invented one: we wait on the fact
                 * itself — the queues and the port registers going quiet — and
                 * never on a guessed spin count. A genuine wedge here is what
                 * Nightwatch convicts, by facts, without a clock. */
                kprintf("[HALT] no time ruler — draining against the queues "
                        "themselves, unbounded\n");
                said = true;
            }
        } else if (ruled && HourGlassRunOut(&stall)) {
            kprintf("[HALT] storage work has not moved for %lu us (%s) — "
                    "%lu unit(s) still owed\n",
                    HourGlassElapsedUs(&stall),
                    HourGlassSourceDied(&stall) ? "ruler died"
                                                : HourGlassRulerName(&stall),
                    work);

            /* WHICH core, HOW MANY, and WHOSE work — the three things the old
             * line could not say. A stuck queue names the continuation it is
             * standing in, so the next reader starts at a function instead of
             * at a number. */
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
 * legitimate keeps a core from answering for seconds. DECLARED POLICY, reused
 * so the kernel has one answer to "a core is not responding", not two. */
#define HALT_AP_ARRIVAL_US ((uint64_t)CONFIG_TLB_SHOOTDOWN_PANIC_MS * 1000ULL)

static void halt_all_ap_cores(void)
{
    if (!g_amp.multicore_active)
        return;

    kprintf("[HALT] Stopping all AP cores...\n");

    uint8_t my_index = amp_get_core_index();

    /* Who is being asked, sampled BEFORE the broadcast. A core that was already
     * offline never had to answer, and holding it to this roll call would mean
     * waiting on a core that is not running. Each asked core's arrival word is
     * cleared first, so an answer left over from an earlier panic broadcast
     * cannot be read as an answer to this one. 256 bytes of stack against a
     * 16 KB kernel stack — the array is the cheap part. */
    bool asked[MAX_CORES];
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        asked[c] = (c != my_index) && amp_core_online(&g_amp.cores[c]);
        if (asked[c])
            __atomic_store_n(&g_amp.cores[c].halted, (uint8_t)0, __ATOMIC_RELAXED);
    }

    lapic_send_ipi_all_excluding_self(IPI_PANIC_VECTOR);

    /* Wait for the FACT, not for a guessed five milliseconds. Every asked core
     * publishes its own arrival from the IPI_PANIC branch of the interrupt
     * handler with a release store, which the acquire load below pairs with.
     * The old code slept 5 ms and then ASSUMED — so a core that answered a
     * moment late was treated exactly like a core that answered, and a core
     * that never answered at all was never named. */
    HourGlass roll;
    bool ruled = HourGlassTurn(&roll, HALT_AP_ARRIVAL_US);
    if (!ruled)
        kprintf("[HALT] no time ruler — waiting for every AP to publish its "
                "halt, unbounded\n");

    for (;;)
    {
        bool all_in = true;
        for (uint8_t c = 0; c < g_amp.total_cores; c++)
        {
            if (!asked[c]) continue;
            if (!__atomic_load_n(&g_amp.cores[c].halted, __ATOMIC_ACQUIRE))
            {
                all_in = false;
                break;
            }
        }
        if (all_in) break;
        if (ruled && HourGlassRunOut(&roll)) break;
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
         * core leave the "online" set with proper cross-CPU visibility. */
        __atomic_store_n(&g_amp.cores[c].online, (uint8_t)0, __ATOMIC_RELEASE);
    }

    if (absent == 0)
    {
        /* Nothing to force, and that is a proof rather than a hope. A core that
         * published its arrival cannot have been inside a spin_lock critical
         * section: spin_lock does `cli` before it acquires and restores RFLAGS
         * only in spin_unlock, so IPI_PANIC is undeliverable for the whole of
         * any critical section — it waits in that core's LAPIC IRR and is taken
         * at the instant the lock is released. A complete roll call therefore
         * proves process_lock and the cleanup queue's lock are unheld by any AP,
         * and force-releasing them here would be theatre. */
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
    // Clear current_process on every core's scheduler.
    // AP cores are already in cli;hlt — they will never touch these pointers again.
    // This prevents process_destroy() from refusing to destroy "current" processes.
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        scheduler_state_t *s = scheduler_get_core(c);
        s->current_process = NULL;
    }
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
 * ‼ AND THE ANSWER THAT LOOKS LIKE "TRY AGAIN IN A MOMENT" MUST NOT BE
 * TREATED AS ONE HERE, BECAUSE OF WHERE THIS STANDS.
 *
 * This runs after `cli` and after halt_all_ap_cores(), so there is exactly
 * one core executing on this machine and it is the one inside this function.
 * ERR_BUSY means another core holds the port's whole-port door — and every
 * other core has already been stopped, so that door would never be given
 * back. A retry loop here, however it were bounded, would be a wait for an
 * event that cannot happen, ending by abandoning the work. That is the
 * defect, not the repair.
 *
 * What it gets instead is a fact, and the fact is taken care of one level
 * down: ahci_port_take_whole records WHICH CORE holds the door and asks
 * amp_core_online() about it, so a door held by a core that is no longer
 * running is not "busy" at all — it is abandoned, and once the device owes
 * nothing it is taken over on the spot. So the single call below is the
 * whole of the asking, and an ERR_BUSY that survives it is a genuine
 * surprise: a door held by a core this shutdown believes is still running.
 * It is said out loud like any other refusal rather than waited on.
 *
 * Either way the line that follows says what actually happened. A shutdown
 * that could not make the volume durable is not a failed shutdown — the
 * machine still has to power off — but it is a shutdown that owes the
 * operator the truth about what may not be on the disk when it comes back.
 */
static void halt_sync_storage(void)
{
    kprintf("[HALT] Syncing TagFS...\n");
    tagfs_shutdown();

    kprintf("[HALT] Flushing disk cache...\n");

    error_t flushed = tagfs_flush_cache();

    if (flushed == OK) {
        kprintf("[HALT] Storage sync complete\n");
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

    kprintf("[HALT] Masking all IRQs...\n");
    for (uint8_t irq = 0; irq < irqchip_max_irqs(); irq++)
    {
        irqchip_disable_irq(irq);
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

void system_halt(bool reboot)
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

    halt_all_ap_cores();
    halt_terminate_all_processes();
    halt_sync_storage();
    halt_stop_hardware();

    kprintf("[HALT] Cleanup complete.\n");
    kprintf(reboot ? "[HALT] Rebooting...\n" : "[HALT] Powering off...\n");

    /* The last words onto the wire before the machine goes: interrupts are
     * off, so the line cannot drive itself from here. */
    WireDrain();

    if (reboot) acpi_reboot();
    else        acpi_shutdown();

    __builtin_unreachable();
}
