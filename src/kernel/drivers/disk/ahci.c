#include "klib.h"
#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "io.h"
#include "irqchip.h"
#include "atomics.h"
#include "error.h"
#include "pic.h"
#include "cpu_calibrate.h"
#include "boxos_memory.h"
#include "idt.h"
#include "amp.h"
#include "hourglass.h"
#include "boardroom.h"   /* a port that does not come back gives up its seat */

#define AHCI_TIMEOUT_MS CONFIG_AHCI_CMD_TIMEOUT_MS

static ahci_controller_t ahci_ctrl;

/* This controller's link on its INTx line, for the fallback path. One node,
 * because there is one controller record; a shared wire chains it to the
 * nodes of whatever else sits on the same pin. */
static SummonsResponder ahci_intx_responder;

/* The node this controller answers its message on. MSI has no pin and no
 * sharing, so it needs no Summons — only a vector and a link. */
static SummonsResponder ahci_msi_responder;

static inline ahci_port_regs_t* ahci_get_port_regs(uint8_t port_num) {
    uintptr_t port_base = (uintptr_t)ahci_ctrl.hba_mem + 0x100 + (port_num * 0x80);
    return (ahci_port_regs_t*)port_base;
}

static ahci_port_t* ahci_get_port(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS || !ahci_ctrl.initialized)
        return NULL;
    ahci_port_t* p = &ahci_ctrl.ports[port_num];
    return p->active ? p : NULL;
}

ahci_port_t* ahci_get_port_state(uint8_t port_num) {
    return ahci_get_port(port_num);
}

volatile ahci_port_regs_t* ahci_get_port_regs_pub(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return NULL;
    return ahci_get_port_regs(port_num);
}

static int ahci_port_stop(ahci_port_t* port) {
    if (!port || !port->regs) {
        return -1;
    }

    ahci_port_regs_t* regs = port->regs;

    regs->cmd &= ~AHCI_PCMD_ST;

    /* The exit is the fact — PxCMD.CR clearing — and the 500 ms is not a
     * number picked here: AHCI 1.3.1 §10.4.2 names it exactly, "if PxCMD.CR
     * does not clear within a reasonable time (500 milliseconds), it may
     * assume the interface is in a hung condition". The clock cannot be
     * replaced by an interrupt, because CR clearing raises none — it is a
     * status bit the engine drops on its own schedule, not a completion the
     * HBA signals — and this function is called from contexts with nothing
     * else to wait on anyway: port bring-up before this port's PxIE is armed,
     * the K-Core COMRESET path, and (since AHCI 1.3.1 §3.3.14 makes a clean
     * stop the only proof the HBA has let go of a DMA buffer) the two places
     * in this file that must know before freeing a page the controller might
     * still be writing to. */
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(500);
    while (rdtsc() < deadline) {
        if ((regs->cmd & AHCI_PCMD_CR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (regs->cmd & AHCI_PCMD_CR) {
        debug_printf("[AHCI] Port %u: Failed to stop (CR still set)\n", port->port_num);
        return -1;
    }

    regs->cmd &= ~AHCI_PCMD_FRE;

    /* Same shape as the CR wait above, on PxCMD.FR — AHCI 1.3.1 §10.3.2:
     * "software must first wait for PxCMD.FR to clear to '0'" before PxFB may
     * be touched. §10.3.2 names no separate number for FR the way §10.4.2
     * does for CR, so the same 500 ms bound is reused rather than invented: a
     * DMA engine that will not idle within the time the specification itself
     * calls unreasonable for the command-list engine is unreasonable here
     * too, and FR raises no interrupt of its own to wait on instead. */
    deadline = rdtsc() + cpu_ms_to_tsc(500);
    while (rdtsc() < deadline) {
        if ((regs->cmd & AHCI_PCMD_FR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (regs->cmd & AHCI_PCMD_FR) {
        debug_printf("[AHCI] Port %u: Failed to stop FIS receive (FR still set)\n", port->port_num);
        return -1;
    }

    return 0;
}

static int ahci_port_start(ahci_port_t* port) {
    if (!port || !port->regs) {
        return -1;
    }

    ahci_port_regs_t* regs = port->regs;

    /* Same register, same 500 ms and the same reason as ahci_port_stop's CR
     * wait above (AHCI 1.3.1 §10.4.2): PxCMD.ST must not be set while
     * PxCMD.CR is still 1 (§10.1.2), CR clearing is a status bit rather than
     * an interrupt, and starting only ever follows a stop or a COMRESET —
     * contexts with no armed interrupt of this port's own to wait on instead. */
    uint64_t start_deadline = rdtsc() + cpu_ms_to_tsc(500);
    while (rdtsc() < start_deadline) {
        if ((regs->cmd & AHCI_PCMD_CR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (regs->cmd & AHCI_PCMD_CR) {
        debug_printf("[AHCI] Port %u: Cannot start - CR still set\n", port->port_num);
        return -1;
    }

    regs->cmd |= AHCI_PCMD_FRE;
    regs->cmd |= AHCI_PCMD_ST;

    return 0;
}

static void ahci_port_readmit(ahci_port_t* port);

/* K-Core bottom-half: full port recovery (COMRESET) after a fatal error.
 * Posted to a K-Core via the port's embedded never-drop recovery node
 * (BatonPass) because COMRESET busy-waits for hundreds of ms,
 * far too long for interrupt context. The IRQ path already failed the
 * port's outstanding async slots with ERR_IO before scheduling this.
 *
 * ‼ AND THE SAME NODE CARRIES THE OTHER LONG JOB THIS PORT CAN NEED: being
 * BUILT AGAIN after it left the machine. One node and one `recovering` claim
 * for both, deliberately — a port cannot be recovered and re-admitted at the
 * same time, and giving the second job a node of its own would allow exactly
 * that ordering. Which of the two this is is not remembered when the baton is
 * posted but READ HERE, from the port's standing at the moment it runs, so a
 * port that changed its mind in between gets the job it actually needs. */
static void ahci_deferred_recover(void* ctx) {
    ahci_port_t* port = (ahci_port_t*)ctx;
    if (__atomic_load_n(&port->standing, __ATOMIC_ACQUIRE) == AHCI_STANDING_LEFT) {
        ahci_port_readmit(port);
    } else {
        ahci_port_recover(port);
    }
    __atomic_store_n(&port->recovering, 0, __ATOMIC_RELEASE);
}

/* Fire the completion callback and return the slot to the pool for each bit
 * in `mask`. Async slots always carry a callback (installed before arming);
 * sync slots are never tracked in issued_mask, so they never reach here and
 * free themselves from their own poll loop. The slot is freed BEFORE the
 * callback runs so a callback that chains another async op can reuse it. */
static inline void ahci_retire_slots(ahci_port_t* state, uint8_t port_idx,
                                     uint32_t mask, error_t status) {
    while (mask) {
        uint8_t slot = (uint8_t)__builtin_ctz(mask);
        mask &= mask - 1;
        void (*cb)(uint8_t, uint8_t, error_t, void*) = state->cb[slot];
        void *ctx = state->cb_ctx[slot];
        state->cb[slot]     = NULL;
        state->cb_ctx[slot] = NULL;
        ahci_free_slot(port_idx, slot);
        if (cb) {
            cb(port_idx, slot, status, ctx);
        }
    }
}

/* Claim-checked retire arbitration (Ф26 M1). issued_mask is the single arbiter
 * of which async slots are still ours to retire. A retiring actor offers a
 * `candidate` set and gets back only the bits its own atomic transition actually
 * cleared (1->0); it must retire ONLY those. With one retire actor (the BSP MSI
 * handler) this is a plain wrapper — won always equals candidate. It becomes
 * load-bearing once ahci_watchdog_scan can also retire from the PIT tick: two
 * actors that decide the same slot completed in the same instant then retire it
 * exactly once, and only the winner reads/clears cb[slot]. */
static inline uint32_t ahci_claim(ahci_port_t* state, uint32_t candidate) {
    return __sync_fetch_and_and(&state->issued_mask, ~candidate) & candidate;
}

void ahci_irq_handler(void) {
    // Called from the IRQ dispatcher with interrupts disabled. Do NOT cli/sti
    // or send EOI here — the dispatcher EOIs after this returns.
    if (!ahci_ctrl.initialized) {
        return;
    }

    uint32_t is = __atomic_load_n(&ahci_ctrl.hba_mem->is, __ATOMIC_ACQUIRE);
    if (is == 0) {
        return;
    }

    __sync_fetch_and_add(&ahci_ctrl.total_interrupts, 1);

    uint32_t ports_pending = is & ahci_ctrl.port_implemented;
    for (uint8_t i = 0; i < AHCI_MAX_PORTS && ports_pending; i++) {
        if (!(ports_pending & (1U << i))) continue;
        ports_pending &= ~(1U << i);

        ahci_port_t* state = &ahci_ctrl.ports[i];
        if (!state->active) continue;

        volatile ahci_port_regs_t* port = ahci_get_port_regs(i);
        uint32_t port_is = port->is;

        /* Clear PxIS FIRST (write-1-to-clear), BEFORE scanning for completed
         * slots. AHCI completion-interrupt bits are per-FIS-type — DHRS for a
         * plain Reg-D2H, SDBS for an NCQ Set-Device-Bits — NOT per-command, so
         * several commands retiring close together collapse onto ONE bit. If
         * PxIS were cleared only AFTER processing, a command that completes
         * DURING this handler (the async-read state machine arms its next block
         * inline from a retire callback, and under fast media that block
         * finishes before we return) would have its shared status bit cleared
         * with no fresh edge AND be absent from the one `completed` snapshot —
         * so it would never be retired: its slot stays in issued_mask forever
         * with a live callback, hanging the waiter. Clearing first means any
         * late completion re-asserts a fresh PxIS edge -> a new MSI we service.
         * The fatal-error check below uses the pre-clear `port_is` snapshot. */
        port->is = port_is;

        /* Drain EVERY completed slot, re-reading the completion register on each
         * pass. A retired slot's callback may arm the next command of a chained
         * async op (multi-block read); that command can finish before we exit,
         * so a single pass loses it. Loop until no issued slot is complete.
         *
         * Per pass read issued_mask BEFORE the completion register: the
         * submitter writes the register then issued_mask (IRQs off), so on x86
         * TSO a slot present in the snapshot is still set in the register read,
         * and `snapshot & ~outstanding` never falsely retires a just-armed slot.
         * NCQ commands clear PxSACT on completion (PxCI clears early when the
         * FIS is sent); non-queued commands clear PxCI.
         *
         * AHCI_DRAIN_MAX bounds a pathological self-arming callback; legitimate
         * chains drain in a handful of passes. If ever hit, slots left
         * outstanding re-raise a fresh PxIS edge (cleared above) and the next
         * MSI continues the drain — correctness holds either way. */
        enum { AHCI_DRAIN_MAX = 4096 };
        for (uint32_t pass = 0; pass < AHCI_DRAIN_MAX; pass++) {
            uint32_t snapshot    = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
            uint32_t outstanding = state->ncq
                ? __atomic_load_n(&port->sact, __ATOMIC_ACQUIRE)
                : __atomic_load_n(&port->ci,   __ATOMIC_ACQUIRE);
            uint32_t completed   = snapshot & ~outstanding;
            if (!completed) break;
            /* Claim-checked retire: clear our candidates from issued_mask and
             * retire ONLY the bits we actually won, so a concurrent watchdog
             * retire (Ф26 M1) can never double-fire a slot. Single actor today
             * => won == completed (behaviour-identical). */
            uint32_t won = ahci_claim(state, completed);
            if (!won) continue;
            __sync_fetch_and_or(&state->completed_slots, won);
            ahci_retire_slots(state, i, won, OK);
        }

        /* Fatal port errors (TFES/HBFS/IFS). On a task-file error the failing
         * NCQ tag's PxSACT bit is NOT cleared (so it never appears in
         * `completed`) and the HBA halts PxCMD.ST. Snapshot diagnostics, then
         * fail every still-outstanding async slot with ERR_IO so waiters do
         * not hang. Port re-start is handled by the deferred recovery path. */
        if (port_is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
            __sync_fetch_and_add(&state->ncq_errors, 1);
            __atomic_fetch_add(&state->stats.tfes_count, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&state->stats.last_tfd,       port->tfd,  __ATOMIC_RELAXED);
            __atomic_store_n(&state->stats.last_serr,      port->serr, __ATOMIC_RELAXED);
            __atomic_store_n(&state->stats.last_error_tsc, rdtsc(),    __ATOMIC_RELAXED);

            uint32_t stuck = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
            if (stuck) {
                uint32_t won = ahci_claim(state, stuck);
                if (won) ahci_retire_slots(state, i, won, ERR_IO);
            }

            /* The HBA halts PxCMD.ST on a fatal error, so without a restart
             * every later command on this port would stall. COMRESET busy-
             * waits, so post it to a K-Core via the port's never-drop
             * recovery node (allocation-free, IRQ-safe, undroppable). The CAS
             * coalesces a storm of error IRQs into one post. */
            /* Only multi-core schedules deferred recovery: the COMRESET runs
             * on a K-Core pump loop, which exists only when total_cores > 1.
             * On a single core async I/O is disabled and the sync path
             * recovers the port inline, so deferring here would just enqueue
             * to a ring nothing drains. */
            if (g_amp.total_cores > 1 &&
                __sync_bool_compare_and_swap(&state->recovering, 0, 1)) {
                BatonPass(&state->recover_node);   /* never-drop */
            }
        }
    }

    ahci_ctrl.hba_mem->is = is;
}

/* Ф26 M1 — AHCI async-completion watchdog: a safety BACKSTOP, not the delivery
 * path. The MSI edge remains the normal completion mechanism; this scan is the
 * harbour-master's overdue-ship register, glanced at once per PIT tick on the
 * BSP — the same core the AHCI MSI targets, so this scan and ahci_irq_handler
 * are mutually exclusive, and ahci_claim keeps retire correct even if MSI
 * routing is ever spread across cores. It NEVER writes PxIS (the MSI handler
 * owns that W1C); it only READS the PxSACT/PxCI completion level.
 *
 * It has a second caller. The boot-time proof of a read nobody stands over
 * (BoardroomProveUnattendedRead) turns it by hand, on the BSP, before that
 * core has opened interrupts — the completion MSI this would otherwise be a
 * backstop for is waiting in the LAPIC until then, and Tier 1 is how that
 * read's completion comes in at all. Same core, interrupts closed: the
 * exclusion above holds there too.
 *
 *   TIER 0 — a port that LEFT the machine, asking to come back. Its
 *     PxSERR.DIAG.X latch says a COMINIT arrived since it was retired, which
 *     is the same event bring-up reads to decide a port is occupied at all.
 *     The rebuild is posted to a K-Core; see ahci_port_readmit.
 *
 *   TIER 1 — lost-edge reconcile. Re-read the completion LEVEL (PxSACT for NCQ,
 *     PxCI otherwise — AHCI 1.3.1 §5.5.3 / §5.3.x). A slot whose level bit has
 *     already cleared completed on the device but its MSI edge was lost or
 *     coalesced; retire it as SUCCESS. A merely-lost interrupt then costs one
 *     tick of latency, never an I/O failure or a port reset. (A TFES-failed NCQ
 *     tag keeps its PxSACT bit set, so Tier 1 never mistakes a failure for
 *     success — that case falls to Tier 2 / the MSI TFES path.)
 *
 *   TIER 2 — genuine wedge. A slot still outstanding past CONFIG_AHCI_IO_TIMEOUT_MS
 *     is a device that stopped both signalling and completing. Fail every
 *     in-flight slot ERR_IO (as the TFES path does — a port that stopped
 *     completing one 4 KiB command has stopped completing all) and post a
 *     COMRESET to a K-Core via the port's never-drop recovery node, so slots
 *     return to the pool instead of leaking until ahci_alloc_slot wedges the port.
 *
 * Gated on multi-core: async I/O only exists when total_cores > 1 (single core
 * takes the synchronous path with its own bounded poll), and COMRESET recovery
 * needs a K-Core pump that only exists there. */
void ahci_watchdog_scan(void) {
    if (!ahci_ctrl.initialized || g_amp.total_cores <= 1) return;

    uint64_t now         = rdtsc();
    uint64_t overdue_tsc = cpu_ms_to_tsc(CONFIG_AHCI_IO_TIMEOUT_MS);

    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        ahci_port_t* state = &ahci_ctrl.ports[i];
        if (!state->active) {
            /* ‼ TIER 0 — A PORT THAT LEFT, ASKING TO COME BACK.
             *
             * The only port this looks at is one that was IN SERVICE and was
             * retired (AHCI_STANDING_LEFT); a port that never came up at boot
             * is not a port waiting for a disk. The question asked of it is
             * the same event bring-up asks: has a COMINIT arrived? Its
             * interrupt is disarmed, so nothing will tell us — but
             * PxSERR.DIAG.X is a LATCH (AHCI 1.3.1 §3.3.12) and was cleared
             * on the way out, so one glance a tick cannot miss it and no
             * clock is involved anywhere.
             *
             * The work itself is a COMRESET, an IDENTIFY and a mount, so it
             * goes where every other long job from this scan goes: the port's
             * own never-drop node, on a K-Core. The same `recovering` claim
             * keeps one in flight at a time. */
            if (__atomic_load_n(&state->standing, __ATOMIC_ACQUIRE)
                    == AHCI_STANDING_LEFT &&
                (ahci_get_port_regs(i)->serr & AHCI_SERR_DIAG_X) != 0 &&
                __sync_bool_compare_and_swap(&state->recovering, 0, 1)) {
                BatonPass(&state->recover_node);   /* never-drop */
            }
            continue;
        }

        uint32_t snapshot = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
        if (snapshot == 0) continue;   /* free early-out — the common (idle) case */

        volatile ahci_port_regs_t* regs = ahci_get_port_regs(i);

        /* A pending fatal error (task-file error, host-bus / interface fatal)
         * means a command may have completed WITH an error — and on a non-NCQ
         * port PxCI can clear on error, so a bare completion-level check could
         * mis-read a failure as success. When an error is pending, skip Tier-1
         * and treat the port as wedged (Tier-2): fail its slots ERR_IO +
         * COMRESET. This doubles as the backstop for a LOST *error* MSI — PxIS
         * stays set until a handler clears it, and COMRESET's PxIS W1C clears it
         * (ahci_port_comreset). Mirrors ahci_irq_handler's fatal-bit test. */
        uint32_t pxis = __atomic_load_n(&regs->is, __ATOMIC_ACQUIRE);
        bool port_error = (pxis & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) != 0;

        /* TIER 1 — reconcile a lost completion edge against the register level,
         * on a HEALTHY port only (a failed command is left to Tier-2). */
        if (!port_error) {
            uint32_t outstanding = state->ncq
                ? __atomic_load_n(&regs->sact, __ATOMIC_ACQUIRE)
                : __atomic_load_n(&regs->ci,   __ATOMIC_ACQUIRE);
            uint32_t completed = snapshot & ~outstanding;
            if (completed) {
                uint32_t won = ahci_claim(state, completed);
                if (won) {
                    __sync_fetch_and_or(&state->completed_slots, won);
                    debug_printf("[AHCI] watchdog: reconciled lost completion, port %u slots 0x%08x\n",
                                 i, won);
                    ahci_retire_slots(state, i, won, OK);
                }
            }
        }

        /* TIER 2 — genuine wedge: any in-flight slot past the deadline. Read
         * submit_tsc + claim under the lock, consistent with ahci_arm_slot's
         * timestamp+issued_mask write, so a slot re-armed since the snapshot
         * (fresh submit_tsc) is never mis-failed (ABA-safe). issued_mask is
         * stable under the lock: the only other writers are the BSP MSI handler
         * (serialized with this BSP tick) and a cross-core arm (blocked on the
         * lock). */
        spin_lock(&state->lock);
        uint32_t still   = __atomic_load_n(&state->issued_mask, __ATOMIC_ACQUIRE);
        uint32_t overdue = 0;
        for (uint32_t m = still; m; m &= m - 1) {
            uint8_t s = (uint8_t)__builtin_ctz(m);
            if (now - state->submit_tsc[s] > overdue_tsc) overdue |= (1U << s);
        }
        uint32_t won = (overdue || port_error) ? ahci_claim(state, still) : 0;
        spin_unlock(&state->lock);

        if (won) {
            __atomic_store_n(&state->stats.last_error_tsc, now, __ATOMIC_RELAXED);
            kprintf("[AHCI] port %u %s — failing slots 0x%08x and resetting "
                    "the link\n", i,
                    port_error ? "reported a fatal error (lost MSI)"
                               : "went past its deadline", won);
            ahci_retire_slots(state, i, won, ERR_IO);
            if (__sync_bool_compare_and_swap(&state->recovering, 0, 1)) {
                BatonPass(&state->recover_node);   /* never-drop */
            }
        }
    }
}

void ahci_port_enable_irq(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    volatile ahci_port_regs_t* regs = port->regs;

    regs->ie = AHCI_PIS_DHRS |   // Device-to-Host Register FIS
               AHCI_PIS_PSS  |   // PIO Setup FIS
               AHCI_PIS_DSS  |   // DMA Setup FIS
               AHCI_PIS_SDBS |   // Set Device Bits (NCQ completion)
               AHCI_PIS_HBFS |   // Host Bus Fatal Error
               AHCI_PIS_TFES;    // Task File Error

    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_IE;   // global HBA interrupt enable

    debug_printf("[AHCI] Port %u: Interrupts enabled (IE=0x%08x)\n", port_num, regs->ie);
}

void ahci_init_irq(void) {
    if (!ahci_ctrl.initialized) {
        return;
    }

    /* Prefer MSI: edge-triggered and delivered point-to-point to the LAPIC,
     * so it sidesteps legacy INTx routing. The PCI 0x3C "interrupt line" is
     * frequently stale/wrong on APIC systems, and PCI INTx needs level /
     * active-low IOAPIC programming the firmware may not have arranged. MSI
     * is universal on AHCI controllers. Route it to the BSP's LAPIC.
     *
     * The vector is asked for rather than compiled in. It used to be the
     * constant AHCI_MSI_VECTOR, which is to say this controller owned 0x70
     * whether it existed or not, and a second AHCI controller had nowhere to
     * go. A completion that does not arrive is the difference between a disk
     * and a wedge, so the line is asked for on those terms. */
    uint8_t msi_vector = VectorPoolTake(RIGHT_OF_WAY_WORK_IS_REDONE, "AHCI MSI");
    if (msi_vector == 0) return;   /* refused, and the pool said why */

    int msi = pci_msi_enable(ahci_ctrl.pci_dev.bus,
                             ahci_ctrl.pci_dev.device,
                             ahci_ctrl.pci_dev.function,
                             msi_vector,
                             g_amp.bsp_lapic_id);
    if (msi == 0) {
        IrqAnswerMessage(msi_vector, &ahci_msi_responder, ahci_irq_handler,
                         "the AHCI controller");
        ahci_ctrl.irq_vector  = msi_vector;
        ahci_ctrl.irq_enabled = true;
        /* Said out loud, like the INTx fallback below it and like xHCI says
         * the same thing twice on the same boot. How this controller reaches
         * a core is not a detail on a board where a completion that never
         * arrives is the difference between a disk and a wedge — and it was
         * the one branch of this decision that could not be read from the
         * log, because the successful one was a debug_printf. */
        kprintf("[AHCI] this controller signals with MSI, vector 0x%02x "
                "delivered straight to LAPIC %u — no INTx wire is shared with "
                "anyone and no routing table decides where a completion "
                "lands\n",
                msi_vector, g_amp.bsp_lapic_id);
        return;
    }

    /* MSI was offered and the device refused it. The number goes back, or the
     * next controller to ask finds the pool one short for no reason. */
    VectorPoolGiveBack(msi_vector, "AHCI MSI");

    /* Fallback: INTx, and it arrives DESCRIBED.
     *
     * What used to happen here was the defect the comment above warns about,
     * committed rather than avoided: a byte read from config offset 0x3C went
     * straight into irq_register_handler and irqchip_enable_irq, which armed
     * an open-drain level-sensitive wire (PCI 3.0 §2.2.6) as an edge, active
     * high, with room for exactly one driver on a line the same specification
     * requires every driver to share. PciSummons applies the bus rule, asks
     * the Interrupt Pin register whether there is a wire at all, and marks
     * where the GSI came from so _PRT can be seen to beat it later. */
    Summons line;
    if (!PciSummons(ahci_ctrl.pci_dev.bus, ahci_ctrl.pci_dev.device,
                    ahci_ctrl.pci_dev.function, &line)) {
        kprintf("[AHCI] no MSI capability and no INTx line to arm — this "
                "controller's completions are found by the overdue sweep, "
                "not by an interrupt\n");
        ahci_ctrl.irq_vector  = 0xFF;
        ahci_ctrl.irq_enabled = false;
        return;
    }

    if (!SummonsAnswer(&line, &ahci_intx_responder, ahci_irq_handler, "AHCI")) {
        kprintf("[AHCI] INTx line refused — this controller's completions are "
                "found by the overdue sweep, not by an interrupt\n");
        ahci_ctrl.irq_vector  = 0xFF;
        ahci_ctrl.irq_enabled = false;
        return;
    }

    if (!SummonsArm(&line)) {
        /* Answered and then not armed. Answering settles the line, and
         * settling is what gives it a vector, so leaving without taking the
         * answerer back off would hold a number on a line that will never
         * deliver — invisible to everyone except the next device to ask. */
        SummonsUnanswer(&line, &ahci_intx_responder, "AHCI");
        kprintf("[AHCI] INTx line refused — this controller's completions are "
                "found by the overdue sweep, not by an interrupt\n");
        ahci_ctrl.irq_vector  = 0xFF;
        ahci_ctrl.irq_enabled = false;
        return;
    }

    /* SummonsArm has replaced `line` with what was actually programmed —
     * the pin may already have been carrying a vector and a wiring of its
     * own, and this driver joined them. So the wire is PRINTED FROM THE
     * DESCRIPTION rather than asserted from the PCI bus rule: a link's _CRS
     * may state active high (QEMU's q35 GSIA..GSID do), and a sentence
     * claiming "level-triggered and active low" on a pin programmed active
     * high is the operator's only evidence, saying the opposite of the
     * truth. */
    ahci_ctrl.irq_vector  = line.vector;
    ahci_ctrl.irq_enabled = true;
    kprintf("[AHCI] INTx INT%c# on GSI %u (vector %u), %s and %s, %s "
            "(MSI unavailable)\n",
            'A' + (line.intx_pin - 1), line.gsi, line.vector,
            line.trigger == SUMMONS_LEVEL ? "level-triggered" : "edge-triggered",
            line.polarity == SUMMONS_ACTIVE_LOW ? "active low" : "active high",
            line.shared ? "shared with whatever else is on the wire"
                        : "and this kernel holds it alone");
}

int ahci_alloc_slot(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return -1;

    spin_lock(&port->lock);

    if (!port->active) {
        spin_unlock(&port->lock);
        return -1;
    }

    int slot = -1;
    for (uint8_t s = 0; s < AHCI_MAX_SLOTS; s++) {
        if (port->slot_bitmap & (1U << s)) {
            port->slot_bitmap &= ~(1U << s);
            /* Defensive reset — we never want to inherit a stale
             * async callback from a freed slot. ahci_free_slot is
             * supposed to leave these NULL too, but reset on alloc
             * keeps the invariant locally enforced. */
            port->cb[s]     = NULL;
            port->cb_ctx[s] = NULL;
            slot = s;
            break;
        }
    }

    spin_unlock(&port->lock);
    return slot;
}

void ahci_free_slot(uint8_t port_num, uint8_t slot) {
    if (slot >= AHCI_MAX_SLOTS) return;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    spin_lock(&port->lock);
    if (!port->active) {
        spin_unlock(&port->lock);
        return;
    }
    port->slot_bitmap |= (1U << slot);
    spin_unlock(&port->lock);
}

bool ahci_can_submit_port(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return false;
    /* Advisory hint — actual allocation re-checks under port->lock. */
    return __atomic_load_n(&port->slot_bitmap, __ATOMIC_RELAXED) != 0;
}

/* Take the port WHOLE, for the one command class that cannot share it.
 *
 * AHCI 1.3.1 §1.7: "System software is responsible to ensure that queued and
 * non-queued commands are not mixed in the command list for the same device
 * with the exception of the NCQ Unload command." FLUSH CACHE EXT is not that
 * exception, so on a port running FPDMA QUEUED it may only be issued with the
 * queue empty — and with nothing able to refill it while it runs.
 *
 * There is no new queue or flag invented for that. The port's slot pool
 * already is the door every submission goes through, so closing it IS the
 * hold: the door is shut only at the instant the port is found completely
 * idle, and it is shut for exactly as long as that one command needs — its
 * retries, and the port recovery between them, being the same command still
 * and needing the same empty list. Waiting
 * for that instant costs nobody anything — while this spins, every other flow
 * allocates and completes as usual, and only the flush waits.
 *
 * The condition is a fact and the whole fact: every legal slot free — which is
 * also the proof that no submitter is sitting between ahci_alloc_slot and its
 * PxCI store — with PxSACT and PxCI agreeing that the HBA has nothing
 * outstanding either.
 *
 * ‼ AND THE TWO WAYS OF NOT GETTING IT ARE DIFFERENT FACTS.
 *
 * They were one answer — -1 — and everything above turned that into ERR_IO,
 * so a machine whose disk was perfectly healthy reported a durability failure
 * because two flushes of the same medium overlapped by a microsecond.
 *
 *   THE DOOR IS HELD. Another whole-port command has it right now. That is
 *   knowable in one look under the port's own lock, it is true or false at
 *   this instant, and no amount of clock makes it truer — so no clock is
 *   turned: AHCI_TAKE_HELD goes back immediately and the caller, who still
 *   holds its bytes, decides whether to come again.
 *
 *   THE DEVICE OWES WORK. Slots are out with commands that have not been
 *   retired. That is the one thing here a clock may judge, because what is
 *   being waited on is a device answering, and a device that never answers is
 *   a device that has failed: AHCI_TAKE_SILENT, said out loud.
 *
 *   AND A THIRD, WHICH LOOKS LIKE THE FIRST AND IS NOT: THE DOOR IS HELD BY
 *   A CORE THAT IS NO LONGER RUNNING. "Come back later" is only an answer
 *   when somebody is left to open the door, and at shutdown nobody is —
 *   halt_sync_storage() flushes after `cli` and after every AP has been
 *   stopped, so a door held there would never be given back and a caller
 *   told to come again would spin out a whole patience window for nothing.
 *   Who holds it is recorded when it is taken (port->door_holder) and
 *   amp_core_online() is this kernel's own answer to whether that core still
 *   runs, so the abandoned door is a FACT too and no clock decides it. Once
 *   the device also owes nothing — the same condition a first take is
 *   granted on, PxSACT and PxCI both clear — the door is simply taken over.
 *   If the device DOES still owe work, that is the second case above and the
 *   clock judges it as it judges any unanswered command.
 *
 * Waiting for the second costs nobody anything — while this spins, every
 * other flow allocates and completes as usual, and only the flush waits. */
AhciTake ahci_port_take_whole(uint8_t port_num, uint32_t patience_ms,
                              uint8_t* slot_out) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port || !slot_out) return AHCI_TAKE_NO_PORT;

    volatile ahci_port_regs_t* regs = port->regs;

    HourGlass glass;
    bool      timing = false;

    uint8_t me = amp_get_core_index();

    /* Carried between passes: the previous pass found the door held by a core
     * that is gone AND the device owing nothing, so this pass may take it
     * over. Established outside the lock, one MMIO read, and only ever while
     * a door is actually orphaned — the ordinary spin below reads no register
     * it did not read before. It cannot go stale in the wrong direction: with
     * the door shut nobody can allocate a slot, so a port that is quiet under
     * a held door stays quiet. */
    bool orphan_and_quiet = false;

    for (;;) {
        bool    held      = false;
        bool    orphan    = false;
        bool    abandoned = false;
        bool    shut      = false;
        uint8_t was_held_by = 0;

        spin_lock(&port->lock);
        if (port->slot_held) {
            /* ‼ TWO DOORS THAT LOOK IDENTICAL AND ARE NOT.
             *
             * One is held by a core that is running and will give it back.
             * The other is held by a core this machine has already stopped —
             * every AP once halt_all_ap_cores() has swept, and any core that
             * died holding it. Waiting on the second is waiting for an event
             * that cannot happen, and the shutdown path is exactly where it
             * is reached: halt_sync_storage() flushes after `cli` and after
             * the APs are down, so "come back later" there means "never".
             *
             * The question is not how long to wait. It is WHO, and
             * amp_core_online() already answers it. */
            if (port->door_holder == me ||
                amp_core_online(&g_amp.cores[port->door_holder])) {
                held = true;
            } else if (orphan_and_quiet) {
                /* The holder is gone and the device owes nothing, so there is
                 * nothing outstanding for a new command to be issued beside —
                 * the same fact a first-time take is granted on. The door is
                 * taken over rather than waited on. */
                was_held_by       = port->door_holder;
                port->door_holder = me;
                port->slot_bitmap = 0;
                abandoned = true;
                shut      = true;
            } else {
                orphan = true;
            }
        } else if (port->slot_bitmap == port->slot_mask) {
            port->slot_held   = true;
            port->door_holder = me;
            port->slot_bitmap = 0;
            shut = true;
        }
        spin_unlock(&port->lock);

        /* Not a wait and not a failure: somebody else's whole-port command is
         * running on this port at this moment, and it will end. Said back as
         * itself, without a syllable on the console — two flushes of the same
         * medium meeting is ordinary, and a line per meeting is how a log
         * stops being read. */
        if (held) return AHCI_TAKE_HELD;

        /* An orphaned door: its holder is not running. Whether it may be
         * taken over is the same question a first take asks — does the device
         * owe anything — and that is one register read, taken here rather
         * than under the port's lock, and acted on by the next pass. If the
         * device DOES still owe work, nothing is taken and the clock below
         * judges it as it judges any device that was given a command and has
         * not answered. */
        orphan_and_quiet = orphan &&
                           (regs->sact & port->slot_mask) == 0 &&
                           (regs->ci   & port->slot_mask) == 0;

        if (abandoned) {
            kprintf("[AHCI] port %u: its whole-port door was held by core %u, "
                    "which is no longer running, and the device owes nothing "
                    "— the door is taken over rather than waited for\n",
                    port_num, was_held_by);
        }

        if (shut) {
            if ((regs->sact & port->slot_mask) == 0 &&
                (regs->ci   & port->slot_mask) == 0) {
                *slot_out = (uint8_t)__builtin_ctz(port->slot_mask);
                return AHCI_TAKE_GOT;
            }
            /* Every slot was free and the HBA still shows work outstanding:
             * open the door again rather than issue beside it. */
            ahci_port_give_back(port_num);
        }

        /* A port can also LEAVE while this waits — its own recovery, or the
         * watchdog's on another core, retires it and empties its slot pool.
         * That looks from here exactly like a port that never goes quiet, and
         * spinning out the whole of `patience_ms` on a port that is not in
         * the machine any more is a wait with no possible end. One look, no
         * clock. */
        if (!ahci_get_port(port_num)) return AHCI_TAKE_NO_PORT;

        /* From here the port is busy with the DEVICE, and only now is there
         * anything a clock is entitled to judge. */
        if (!timing) {
            if (!HourGlassTurn(&glass, (uint64_t)patience_ms * 1000ULL)) {
                kprintf("[AHCI] no time source — there is no clock to wait out "
                        "port %u's outstanding commands on, and issuing a "
                        "non-queued command beside queued ones is what AHCI "
                        "1.3.1 §1.7 forbids; not issuing it\n", port_num);
                return AHCI_TAKE_SILENT;
            }
            timing = true;
        }

        if (HourGlassRunOut(&glass)) {
            kprintf("[AHCI] port %u: %lu us on the %s and the commands it was "
                    "given never came back (SACT=0x%08x CI=0x%08x) — the "
                    "device has stopped answering, so nothing more is issued "
                    "to it\n",
                    port_num, (unsigned long)HourGlassElapsedUs(&glass),
                    HourGlassRulerName(&glass), regs->sact, regs->ci);
            return AHCI_TAKE_SILENT;
        }
        cpu_pause();
    }
}

/* Open the door again. A hold is only ever taken on a port with every slot
 * free, so this restores exactly what it took. */
void ahci_port_give_back(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    spin_lock(&port->lock);
    if (port->slot_held) {
        port->slot_bitmap |= port->slot_mask;
        port->slot_held    = false;
    }
    spin_unlock(&port->lock);
}

error_t ahci_build_io(uint8_t port_num, uint8_t slot, uint64_t lba,
                      uint16_t sector_count, void* buffer_phys, bool write) {
    if (slot >= AHCI_MAX_SLOTS) return ERR_INVALID_ARGUMENT;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return ERR_DEVICE_NOT_READY;

    /* S64A guard: a controller without 64-bit addressing can only DMA below
     * 4 GiB. Every driver buffer uses PHYS_TAG_DMA32 (<1 GiB) so this never
     * trips today, but reject a high address rather than silently truncate
     * it into dba (which would DMA to the wrong physical page). */
    if (!ahci_ctrl.s64a_support && ((uintptr_t)buffer_phys >> 32)) {
        return ERR_INVALID_ADDRESS;
    }

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    cmdheader[slot].cfl   = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w     = write ? 1 : 0;
    cmdheader[slot].prdtl = 1;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    cmdtbl->prdt[0].dba  = (uint32_t)(uintptr_t)buffer_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)((uintptr_t)buffer_phys >> 32);
    cmdtbl->prdt[0].dbc  = (sector_count * 512u) - 1u;
    cmdtbl->prdt[0].i    = 1;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c        = 1;
    cmdfis->device   = (1 << 6);   // LBA mode

    cmdfis->lba0 = (lba >>  0) & 0xFF;
    cmdfis->lba1 = (lba >>  8) & 0xFF;
    cmdfis->lba2 = (lba >> 16) & 0xFF;
    cmdfis->lba3 = (lba >> 24) & 0xFF;
    cmdfis->lba4 = (lba >> 32) & 0xFF;
    cmdfis->lba5 = (lba >> 40) & 0xFF;

    if (port->ncq) {
        /* FPDMA QUEUED: sector count goes in the features field, the NCQ TAG
         * in count[7:3]. Completion is signalled by PxSACT clearing (SDB
         * FIS) — NOT PxCI, which clears once the FIS is merely sent. */
        cmdfis->command  = write ? ATA_CMD_WRITE_FPDMA_QUEUED
                                 : ATA_CMD_READ_FPDMA_QUEUED;
        cmdfis->featurel = sector_count & 0xFF;
        cmdfis->featureh = (sector_count >> 8) & 0xFF;
        cmdfis->countl   = (uint8_t)(slot << 3);
        cmdfis->counth   = 0;
    } else {
        /* READ/WRITE DMA EXT: sector count in the count field; completion is
         * signalled by PxCI clearing. Only one may be outstanding per port. */
        cmdfis->command  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        cmdfis->countl   = sector_count & 0xFF;
        cmdfis->counth   = (sector_count >> 8) & 0xFF;
    }

    return OK;
}

void ahci_arm_slot(uint8_t port_num, uint8_t slot) {
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port || slot >= AHCI_MAX_SLOTS) return;
    volatile ahci_port_regs_t* regs = port->regs;

    /* PxCI / PxSACT are write-1-to-set (writing 0 to a bit has no effect, and
     * hardware clears bits independently as commands retire). A direct
     * single-bit store is therefore correct; an RMW `|=` could re-arm a slot
     * the HBA just cleared between the read and the write. For NCQ, PxSACT
     * must be set before PxCI (AHCI 1.3.1 §5.5.3). The spin_lock disables
     * IRQs so the completion handler cannot observe issued_mask mid-update. */
    spin_lock(&port->lock);
    if (port->ncq) {
        regs->sact = (1U << slot);
    }
    regs->ci = (1U << slot);
    /* Ф26 M1: stamp the submit timestamp under the lock, right where the slot
     * enters issued_mask, so ahci_watchdog_scan's overdue check reads a
     * submit_tsc consistent with the arming (no torn arm-vs-scan). Only the
     * async path calls ahci_arm_slot, so only async slots get a submit_tsc —
     * exactly the set the watchdog scans. */
    port->submit_tsc[slot] = rdtsc();
    __sync_fetch_and_or(&port->issued_mask, (1U << slot));
    spin_unlock(&port->lock);
}

bool ahci_port_is_ncq(uint8_t port_num) {
    ahci_port_t* port = ahci_get_port(port_num);
    return port ? port->ncq : false;
}

/* Issue ATA IDENTIFY DEVICE (non-queued, via PxCI) into a DMA32 bounce
 * buffer and copy the 256-word result to id_out. Run once per port at
 * bring-up to learn sector size, NCQ support, LBA48 and capacity. The port
 * engine (PxCMD.ST) must already be running and no other command may be
 * outstanding (true during init), so command slot 0 is used directly. */
static error_t ahci_port_identify(ahci_port_t* port, uint16_t* id_out) {
    volatile ahci_port_regs_t* regs = port->regs;

    void* buf_page = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!buf_page) {
        return ERR_NO_MEMORY;
    }
    uintptr_t buf_phys = (uintptr_t)buf_page;
    void* buf_virt = vmm_phys_to_virt(buf_phys);
    memset(buf_virt, 0, 512);

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    memset(&cmdheader[0], 0, sizeof(ahci_cmd_header_t));
    cmdheader[0].cfl   = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[0].w     = 0;
    cmdheader[0].prdtl = 1;
    cmdheader[0].ctba  = (uint32_t)port->ctba_phys[0];
    cmdheader[0].ctbau = (uint32_t)(port->ctba_phys[0] >> 32);

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[0];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));
    cmdtbl->prdt[0].dba  = (uint32_t)buf_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    cmdtbl->prdt[0].dbc  = 512 - 1;        // 0-based byte count
    cmdtbl->prdt[0].i    = 0;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c        = 1;
    cmdfis->command  = ATA_CMD_IDENTIFY_DEVICE;
    cmdfis->device   = 0;

    mfence();
    regs->ci = (1U << 0);                   // non-queued: PxCI only
    mfence();

    /* The exit is PxCI clearing (AHCI 1.3.1 §5.3.2.1, the non-queued
     * completion signal this command uses) or PxIS naming a task-file /
     * host-bus / interface fatal error — both are the HBA's own word, not
     * this driver's guess. The deadline stays only because there is nothing
     * else to fall back on here: this probe runs from ahci_port_init, before
     * ahci_init_irq has given this controller a vector and before
     * ahci_port_enable_irq has armed this port's PxIE — main.c does both only
     * after ahci_init returns. No MSI can land for this command however long
     * it is waited for, so the clock is not a backstop beside an interrupt,
     * it is the only exit a wedged device leaves open. */
    error_t result = ERR_TIMEOUT;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_CMD_DEFAULT);
    while (rdtsc() < deadline) {
        if ((regs->ci & (1U << 0)) == 0) {
            result = OK;
            break;
        }
        if (regs->is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
            result = ERR_IO;
            break;
        }
        cpu_pause();
    }

    if (result == OK && (regs->tfd & AHCI_PTFD_STS_ERR)) {
        result = ERR_IO;
    }
    if (result == OK) {
        memcpy(id_out, buf_virt, 512);
    }

    uint32_t isr = regs->is;               // W1C: clear any status the probe raised
    regs->is = isr;

    /* ‼ A TIMED-OUT COMMAND DOES NOT HAND THE PAGE BACK — THE HBA DOES.
     *
     * result == ERR_TIMEOUT means the loop above ran out of clock with PxCI
     * bit 0 STILL SET: the HBA never signalled that it was done with this
     * command, so for all this driver knows the DMA engine is this instant
     * writing IDENTIFY's 512 bytes into buf_page. Freeing it here handed a
     * page the controller still owned straight back to pmm_alloc, and
     * whatever the next caller put there — a TagFS buffer, a page table — was
     * the controller's to overwrite whenever it got around to finishing.
     *
     * The only proof that the HBA has let go is the one AHCI 1.3.1 offers:
     * §3.3.14, writing PxCMD.ST from '1' to '0' clears PxCI itself as part of
     * that transition. ahci_port_stop() is exactly that write, walked out to
     * PxCMD.CR and PxCMD.FR/PxCMD.FRE clearing too (§10.3.2), so a successful
     * return from it is the controller saying, in its own register, that it
     * is no longer touching this port's command list or this buffer. A port
     * that will not stop is a port still mid-transfer by every fact available
     * here, and the page is held rather than freed — one held page against a
     * corrupted heap is not a choice this driver gets to make twice. */
    if (result == ERR_TIMEOUT) {
        if (ahci_port_stop(port) == 0) {
            pmm_free(buf_page, 1);
        } else {
            kprintf("[AHCI] port %u: IDENTIFY timed out and the port will not "
                    "stop (CMD=0x%08x) — its DMA page is held, not freed, "
                    "because the controller may still be writing to it\n",
                    port->port_num, regs->cmd);
        }
    } else {
        pmm_free(buf_page, 1);
    }

    return result;
}

/* ONE NAME FOR A SERIAL ATA GENERATION, and the same one for both registers
 * that report it. AHCI 1.3.1 §3.3.10 gives PxSSTS.SPD as 1h/2h/3h = Gen1
 * 1.5 Gb/s, Gen2 3 Gb/s, Gen3 6 Gb/s, and §3.1.1 gives CAP.ISS the identical
 * encoding for what the HBA can do — which is what lets a negotiated speed be
 * compared against the controller's ceiling and a degraded link be named as
 * one instead of merely printed. 0h is the register's own "not negotiated",
 * and anything above 3h is a generation newer than the specification this
 * driver was written against: it is said as a number rather than guessed at. */
static const char* ahci_link_gen_name(uint8_t gen) {
    switch (gen) {
        case 0x1: return "Gen1 (1.5 Gb/s)";
        case 0x2: return "Gen2 (3.0 Gb/s)";
        case 0x3: return "Gen3 (6.0 Gb/s)";
        default:  return NULL;
    }
}

/* AN IDENTIFY DEVICE TEXT FIELD, TURNED INTO SOMETHING A CONSOLE CAN PRINT.
 *
 * ACS-3 §3.3.10 ("ATA string convention"): the field is a run of 16-bit words
 * in which the FIRST character of each pair is the high byte, and the whole
 * field is padded with spaces (20h) rather than terminated. So the bytes come
 * out of memcpy in the wrong order within every word, the tail is blanks, and
 * some drives pad the head as well.
 *
 * `out` must hold words*2 + 1 bytes. Anything outside printable ASCII is
 * replaced rather than passed through: this string reaches a serial console
 * and a framebuffer, and a drive that answers a control byte must not be able
 * to move the cursor of the machine reading it. */
static void ahci_id_string(const uint16_t* id, unsigned first_word,
                           unsigned words, char* out) {
    unsigned n = words * 2u;
    for (unsigned i = 0; i < words; i++) {
        uint16_t w = id[first_word + i];
        char hi = (char)(w >> 8);
        char lo = (char)(w & 0xFFu);
        out[i * 2u]      = (hi >= 0x20 && hi < 0x7F) ? hi : ' ';
        out[i * 2u + 1u] = (lo >= 0x20 && lo < 0x7F) ? lo : ' ';
    }
    out[n] = '\0';

    while (n > 0 && out[n - 1u] == ' ') out[--n] = '\0';

    unsigned head = 0;
    while (head < n && out[head] == ' ') head++;
    if (head != 0) {
        for (unsigned i = 0; head + i <= n; i++) out[i] = out[head + i];
    }
}

static int ahci_port_init(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        return -1;
    }

    ahci_port_t* port = &ahci_ctrl.ports[port_num];

    /*
     * ‼ A PORT CAN BE BUILT MORE THAN ONCE, AND ITS DMA PAGES OUTLIVE EVERY
     * BUILD BUT THE FIRST.
     *
     * A port that lost its link leaves the machine (ahci_port_retire) and
     * comes back through this same function when a COMINIT says a device is
     * there again — which is the only honest way back, because it is the only
     * path that re-reads IDENTIFY: the disk on the cable after a reseat may
     * not be the disk that was on it before, and its size, its sector
     * geometry and its queue depth are all its own.
     *
     * What does NOT go round again is the memory. The command list and the
     * command tables are freed only by a port that never entered service;
     * once one has, its pages stay its own for the life of the machine. That
     * is not thrift, it is the absence of a race: a submitter that read the
     * port an instant before it was retired can still be writing into those
     * pages, and there is no moment at which this code can prove otherwise.
     * Thirty-three pages held per port is the price of never handing the PMM
     * a page somebody is still writing to.
     */
    void*     held_clb_virt = port->clb_virt;
    uintptr_t held_clb_phys = port->clb_phys;
    void*     held_ctba_virt[AHCI_MAX_SLOTS];
    uintptr_t held_ctba_phys[AHCI_MAX_SLOTS];
    memcpy(held_ctba_virt, port->ctba_virt, sizeof(held_ctba_virt));
    memcpy(held_ctba_phys, port->ctba_phys, sizeof(held_ctba_phys));

    /* True for a port coming BACK, false for one being built the first time.
     * The only thing it decides is who owns the pages on the failure path. */
    bool rebuilt = (held_clb_virt != NULL);

    /* And the recovery claim, which is HELD BY THE CALLER while a re-admission
     * runs: ahci_deferred_recover took it before it got here and gives it back
     * when it returns. Zeroing it in the middle would let a second deferred
     * job be posted onto a port that is half built. */
    uint32_t held_claim = port->recovering;

    memset(port, 0, sizeof(ahci_port_t));

    port->recovering = held_claim;

    port->clb_virt = held_clb_virt;
    port->clb_phys = held_clb_phys;
    port->fis_virt = held_clb_virt ? (uint8_t*)held_clb_virt + 1024 : NULL;
    port->fis_phys = held_clb_phys ? held_clb_phys + 1024 : 0;
    memcpy(port->ctba_virt, held_ctba_virt, sizeof(held_ctba_virt));
    memcpy(port->ctba_phys, held_ctba_phys, sizeof(held_ctba_phys));

    port->port_num = port_num;
    port->regs = ahci_get_port_regs(port_num);
    spinlock_init(&port->lock);

    /* Never-drop recovery node: COMRESET is posted through this embedded
     * node (BatonPass), so a wedged-port recovery can never be
     * dropped for want of a defer slot. */
    port->recover_node.run = ahci_deferred_recover;
    port->recover_node.ctx = port;

    volatile ahci_port_regs_t* regs = port->regs;

    if (ahci_port_stop(port) != 0) {
        kprintf("[AHCI] port %u: would not stop, so it cannot be set up — "
                "no disk on it\n", port_num);
        return -1;
    }

    // CLB (1KB) + received-FIS (256B) share one DMA32 page; both are DMA
    // targets that must sit below 4GB (PHYS_TAG_DMA32 == [0,1GB)).
    // Allocated only if this port does not already hold one — see the note on
    // rebuilt ports above. Wiped either way: a rebuilt port must not inherit
    // a command header or a received FIS from the disk that used to be here.
    if (!port->clb_virt) {
        void* clb_page = pmm_alloc(1, PHYS_TAG_DMA32);
        if (!clb_page) {
            kprintf("[AHCI] port %u: no DMA32 page for its command list — "
                    "no disk on it\n", port_num);
            return -1;
        }
        port->clb_phys = (uintptr_t)clb_page;
        port->clb_virt = vmm_phys_to_virt(port->clb_phys);
        port->fis_virt = (uint8_t*)port->clb_virt + 1024;
        port->fis_phys = port->clb_phys + 1024;
    }
    memset(port->clb_virt, 0, 4096);

    for (uint8_t slot = 0; slot < AHCI_MAX_SLOTS; slot++) {
        if (!port->ctba_virt[slot]) {
            void* ctba_page = pmm_alloc(1, PHYS_TAG_DMA32);
            if (!ctba_page) {
                debug_printf("[AHCI] Port %u: Failed to allocate CTBA for slot %u\n", port_num, slot);
                /* Whatever this port already holds, it goes on holding: the
                 * pages are not the controller's to be surprised by, and the
                 * next attempt at this port starts from what is here. */
                return -1;
            }
            port->ctba_phys[slot] = (uintptr_t)ctba_page;
            port->ctba_virt[slot] = vmm_phys_to_virt(port->ctba_phys[slot]);
        }
        memset(port->ctba_virt[slot], 0, 4096);

        ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
        cmdheader[slot].prdtl = 0;
        cmdheader[slot].prdbc = 0;
        cmdheader[slot].ctba  = (uint32_t)port->ctba_phys[slot];
        cmdheader[slot].ctbau = (uint32_t)(port->ctba_phys[slot] >> 32);
    }

    regs->clb  = (uint32_t)port->clb_phys;
    regs->clbu = (uint32_t)(port->clb_phys >> 32);
    regs->fb   = (uint32_t)port->fis_phys;
    regs->fbu  = (uint32_t)(port->fis_phys >> 32);

    /* Clear stale interrupt state before bring-up.
     *
     * ‼ PxSERR IS NOT CLEARED HERE, AND THAT IS THE SPEC'S OWN ORDER.
     * AHCI 1.3.1 §10.1.1 puts "Clear the PxSERR register, by writing '1s' to
     * each implemented bit location" at step 6 — AFTER step 5, "Wait for a
     * positive indication that a device is attached to the port". It used to
     * be done here, at step 3/4, which wiped PxSERR.DIAG.X — the LATCH that
     * says a COMINIT arrived — microseconds before the one wait that reads
     * it. On an ordinary CAP.SSS=0 chipset, where the COMRESET was emitted by
     * ahci_init's GHC.HR long before this line, that left the presence test
     * with nothing but a live poll of PxSSTS.DET and no latch at all. The
     * clear now stands where the specification puts it, below.
     *
     * Nothing stale survives that: GHC.HR resets every port register field
     * that is not HwInit and not PxCLB/PxFB (§10.4.3), PxSERR among them, so
     * a bit standing here was set after the reset this driver performed.
     *
     * PxIS may still be cleared now, and is: §3.3.5 makes PxIS.PCS a
     * reflection of PxSERR.DIAG.X that "is only cleared when PxSERR.DIAG.X is
     * cleared", so a W1C of the whole of PxIS cannot take the latch with it. */
    regs->is   = 0xFFFFFFFF;

    // Enable FIS receive so the device's initial D2H register FIS lands.
    regs->cmd |= AHCI_PCMD_FRE;

    /* Staggered spin-up (AHCI 1.3.1 §10.1.1): when CAP.SSS is set the drive
     * stays spun down until commanded, so this port asks for it — SUD +
     * power-on + ICC=active.
     *
     * ‼ THE WRITE BELOW IS NOT MADE "HARMLESSLY" ON A NON-SSS HBA — IT IS NOT
     * MADE AT ALL, AND THAT IS THE CORRECT READING OF THE REGISTER. AHCI
     * 1.3.1 §3.3.7 gives PxCMD.SUD's own definition: "If CAP.SSS is '0', this
     * bit is read-only and should be set to '1' by hardware." A comment used
     * to stand here claiming the write happens and does no harm; on a
     * CAP.SSS='0' HBA there is no write to be harmless, because the `if`
     * below is exactly what stops one from being attempted. Skipping the
     * write and attempting a no-op write are the same fact under §3.3.7 — the
     * bit is hardware's to set either way — but only one of those sentences
     * is true of what this driver does. */
    if (ahci_ctrl.cap & AHCI_CAP_SSS) {
        uint32_t cmd = regs->cmd;

        /* Whether the drive was already spinning before this write is a
         * question this register can only answer once. SUD reading 1 here
         * means something upstream of this driver — firmware, or a warm boot
         * that never cleared it — already commanded the spin-up; SUD reading
         * 0 means the write below is the one that starts it. Nothing else in
         * the machine remembers which of the two happened, and the console is
         * the only place left to ask. */
        kprintf("[AHCI] port %u: PxCMD.SUD reads %u before this write — the "
                "drive was %s\n",
                port_num, (cmd & AHCI_PCMD_SUD) ? 1u : 0u,
                (cmd & AHCI_PCMD_SUD)
                    ? "already spun up, by firmware or an earlier boot"
                    : "spun up by this kernel, not before it");

        cmd |= AHCI_PCMD_SUD | AHCI_PCMD_POD;
        cmd  = (cmd & ~((uint32_t)AHCI_PCMD_ICC_MASK << AHCI_PCMD_ICC_SHIFT))
               | ((uint32_t)AHCI_PCMD_ICC_ACTIVE << AHCI_PCMD_ICC_SHIFT);
        regs->cmd = cmd;
        mfence();
    }

    /* Wait for the link — on real time, and on the register's own fact.
     *
     * Every implemented port reaches this wait, because the value PxSSTS.DET
     * held before it is not evidence of anything. AHCI 1.3.1 §10.4.3: "If the
     * HBA does not support staggered spin-up, the HBA reset shall cause a
     * COMRESET to be sent on the port" — and a COMRESET is exactly what wipes
     * this register: §3.3.10, "When the HBA transmits a COMRESET to the
     * device, this register is updated to its reset values" (DET's reset value
     * is 0), and §7.3, "When a COMRESET is sent to the device the PxSSTS.DET
     * field shall be cleared to 0h". So on a CAP.SSS=0 HBA — the ordinary
     * consumer chipset — DET reads 0h for the whole of the OOB handshake that
     * ahci_init's GHC.HR just started, and any port judged by a single read
     * taken at that instant is a port with a disk on it, thrown away.
     *
     * The exit is the fact and only the fact: DET = 3h, "device presence
     * detected and Phy communication established" (§3.3.10) — which is also
     * precisely what §10.1.2 requires before PxCMD.ST may be set to '1'. 1h,
     * "device presence detected but Phy communication not established", is the
     * middle of that handshake and never ends this wait, in either direction.
     * And 0h does not end it by standing still for any length of time: what
     * separates an empty port from one still negotiating is whether a COMINIT
     * ever arrived — see AHCI_PRESENCE_WINDOW_MS. */
    {
        uint8_t det = 0;
        HourGlass link;
        if (!HourGlassTurn(&link, (uint64_t)AHCI_COMRESET_LINK_MS * 1000ULL)) {
            /* Telling an empty port from a link still coming up is a question
             * only a clock answers: waiting on the fact would wait forever on
             * a port with nothing attached. So this one refuses instead of
             * waiting unbounded, and says which of the two it could not do. */
            kprintf("[AHCI] no time source — port %u's link cannot be told from "
                    "an empty one; not using it\n", port_num);
            goto fail_free;
        }
        /* THE ARRIVAL, AND THE TWO PLACES IT SHOWS.
         *
         * PxSSTS.DET leaving 0h is the event live — AHCI 1.3.1 §7.3, "When a
         * COMINIT is received, the PxSSTS.DET field shall be set to 1h" — and
         * PxSERR.DIAG.X is the same event LATCHED (§3.3.12), which matters
         * because DET is a state and states come back: an HBA that answers a
         * COMINIT with another COMRESET puts DET back to 0h, and a reader
         * watching DET alone would see nothing but zeroes on a port with a
         * disk on it.
         *
         * The latch is intact when this is reached. PxSERR is cleared at the
         * step the specification puts it at — §10.1.1 step 6, BELOW this
         * wait — and everything before it was wiped by ahci_init's GHC.HR
         * (§10.4.3). So DIAG.X standing here means: a COMINIT arrived on this
         * port since this driver reset the controller.
         *
         * The window is not this driver's: AHCI_PRESENCE_WINDOW_MS carries
         * the clause AHCI §10.1.1 step 5 defers to, quoted. */
        bool answered = false;
        bool empty    = false;
        for (;;) {
            uint32_t ssts = regs->ssts;
            det = (uint8_t)((ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK);
            if (det == AHCI_SSTS_DET_PRESENT) break;
            /* 4h is not a stage of the handshake and waiting does not move it:
             * §3.3.10, "Phy in offline mode as a result of the interface being
             * disabled or running in a BIST loopback mode". Nobody but
             * software puts a Phy there, and this driver never does — so it
             * was firmware, and the answer is already final. Spending the
             * whole window on it would buy nothing but a wrong reason on the
             * console: the port would time out and be reported as a device
             * that answered and never came up, when nothing answered at all. */
            if (det == AHCI_SSTS_DET_OFFLINE) break;

            if (!answered &&
                (det != AHCI_SSTS_DET_NONE ||
                 (regs->serr & AHCI_SERR_DIAG_X) != 0)) {
                /* A COMINIT reached this port. From here on the presence
                 * window has nothing left to say: whatever DET does next,
                 * this port has a device on it and gets the whole link
                 * window to finish negotiating. */
                answered = true;
            }

            bool out = HourGlassRunOut(&link);
            if (!answered &&
                HourGlassElapsedUs(&link)
                    >= (uint64_t)AHCI_PRESENCE_WINDOW_MS * 1000ULL) {
                empty = true;
                break;
            }
            if (out) break;
            cpu_pause();
        }
        if (det != AHCI_SSTS_DET_PRESENT) {
            if (empty) {
                /* Nothing is attached to this port. It used to be said with
                 * debug_printf, on the reasoning that an empty socket is not a
                 * refusal and belongs with the enumeration rather than on the
                 * console — but debug_printf compiles to nothing in a shipped
                 * build, and the operator standing at the board then cannot
                 * tell an empty cable from a port this kernel never looked at.
                 * Those are different facts and only one of them means "plug
                 * the disk in". Every implemented port now accounts for
                 * itself, in one line, whichever way it went. */
                kprintf("[AHCI] port %u: nothing on the link — no COMINIT "
                        "in the %u ms Serial ATA 2.6 §15.2.2.2 allows one "
                        "(SSTS=0x%08x SERR=0x%08x)\n",
                        port_num, AHCI_PRESENCE_WINDOW_MS,
                        regs->ssts, regs->serr);
            } else if (det == AHCI_SSTS_DET_OFFLINE) {
                /* ‼ THE REASON IS READ, NOT ASSERTED.
                 *
                 * This used to say "the interface is disabled in firmware or
                 * held in BIST loopback" — the §3.3.10 gloss on the value,
                 * printed as though it were a measurement. It is not one, and
                 * on the board of 2026-09-20 four ports carried it while
                 * nothing in this driver had ever read the register that
                 * decides between the two halves of the sentence.
                 *
                 * PxSCTL.DET is that register (§3.3.11, Type RW, reset 0h,
                 * "4h Disable the Serial ATA interface and put Phy in offline
                 * mode"), and it is the ONLY software-visible way to put a Phy
                 * offline. Reading it splits the case cleanly, because this
                 * driver's own GHC.HR a moment earlier resets every non-HwInit
                 * port register field (§10.4.3) and PxSCTL.DET is one of them:
                 *
                 *   PxSCTL.DET = 4h — somebody wrote this port off through the
                 *   AHCI register space and it survived, or was rewritten,
                 *   after our reset. That is a switch, and a switch has an
                 *   owner.
                 *
                 *   PxSCTL.DET = 0h with PxSSTS.DET still 4h — nothing in the
                 *   AHCI register space asked for this. The lane is not given
                 *   to SATA by the platform at all (on an Intel PCH, the PCS
                 *   enable in configuration space or a Flex-I/O lane handed to
                 *   PCIe or USB3), and §10.4.3 puts configuration space
                 *   explicitly outside what GHC.HR reaches. No AHCI setting
                 *   moves it, and telling the operator to look for a disabled
                 *   port in Setup would send him after a switch that is not
                 *   there.
                 *
                 * Nothing is written either way: taking a Phy off offline is
                 * the owner's decision, not this driver's, and it is not made
                 * here. What changes is that the log now carries the evidence
                 * to make it with. */
                uint32_t sctl = regs->sctl;
                if ((sctl & 0xFu) == AHCI_SSTS_DET_OFFLINE) {
                    kprintf("[AHCI] port %u: its Phy is offline and PxSCTL.DET "
                            "reads 4h — this port was switched off through the "
                            "AHCI registers and stayed off through this "
                            "kernel's own HBA reset (SCTL=0x%08x SSTS=0x%08x); "
                            "nothing here writes that field back, so the port "
                            "is not used\n",
                            port_num, sctl, regs->ssts);
                } else {
                    kprintf("[AHCI] port %u: its Phy is offline while PxSCTL.DET "
                            "reads %uh — nothing in the AHCI registers asked "
                            "for that, so the lane behind this port is not "
                            "given to SATA by the platform and no AHCI setting "
                            "reaches it (SCTL=0x%08x SSTS=0x%08x); not using "
                            "it\n",
                            port_num, (unsigned)(sctl & 0xFu), sctl, regs->ssts);
                }
            } else if (HourGlassSourceDied(&link)) {
                kprintf("[AHCI] port %u: the %s stopped ticking mid-wait, so an "
                        "empty link cannot be told from one still coming up "
                        "(DET=%uh); not using it\n",
                        port_num, HourGlassRulerName(&link), det);
            } else if (det == AHCI_SSTS_DET_COMINIT) {
                kprintf("[AHCI] port %u: a device answered but the link never "
                        "came up — %lu us on the %s (DET=1h, SSTS=0x%08x); "
                        "not using it\n", port_num,
                        (unsigned long)HourGlassElapsedUs(&link),
                        HourGlassRulerName(&link), regs->ssts);
            } else {
                kprintf("[AHCI] port %u: the link never settled — %lu us on the "
                        "%s and DET is still %uh (SSTS=0x%08x); not using it\n",
                        port_num, (unsigned long)HourGlassElapsedUs(&link),
                        HourGlassRulerName(&link), det, regs->ssts);
            }
            goto fail_free;
        }

        /* THE SPEED THE TWO ENDS AGREED ON, taken here because here is the
         * only place it means anything: AHCI 1.3.1 §3.3.10 defines PxSSTS.SPD
         * as "the negotiated interface communication speed", and a negotiation
         * has only happened once DET reads 3h. It is not a decision this
         * driver makes — every command works the same at any generation — it
         * is the one register that tells a disk running at a quarter of its
         * speed from a disk running at all of it, and nothing else in the
         * machine can be asked about it afterwards. */
        port->link_speed = (uint8_t)((regs->ssts >> AHCI_SSTS_SPD_SHIFT)
                                     & AHCI_SSTS_SPD_MASK);
    }

    /* AHCI 1.3.1 §10.1.1 step 6, in its place: "Clear the PxSERR register, by
     * writing '1s' to each implemented bit location." The link is up, so the
     * errors it raised on the way — and the presence latch this wait has now
     * finished reading — are spent. */
    regs->serr = 0xFFFFFFFF;

    /* Wait for the device to leave BSY/DRQ — AHCI 1.3.1 §10.1.1 step 7, for
     * the time ATA/ATAPI-6 §9.2 D0SR3 actually names (AHCI_DEVICE_READY_MS).
     *
     * The exit is the FACT, and only BSY and DRQ are in it. PxTFD.STS.ERR is
     * deliberately NOT: a device that presents a stale or benign ERR with BSY
     * and DRQ already clear passes bring-up today, and folding ERR into the
     * break would turn those drives into disks this kernel refuses to use. */
    {
        HourGlass glass;
        bool timed = HourGlassTurn(&glass, (uint64_t)AHCI_DEVICE_READY_MS * 1000ULL);
        if (!timed) {
            kprintf("[AHCI] no time source — waiting for port %u to leave "
                    "BSY/DRQ on the fact itself, with no bound\n", port_num);
            while (regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ))
                cpu_pause();
        } else {
            for (;;) {
                if ((regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ)) == 0)
                    break;
                if (HourGlassRunOut(&glass)) break;
                cpu_pause();
            }
        }
        if (regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ)) {
            kprintf("[AHCI] port %u: the device never left BSY/DRQ — %lu us on "
                    "the %s (TFD=0x%08x); not using it\n",
                    port_num, (unsigned long)HourGlassElapsedUs(&glass),
                    HourGlassRulerName(&glass), regs->tfd);
            goto fail_free;
        }
    }

    // Start the command-list engine; commands may now be issued.
    regs->cmd |= AHCI_PCMD_ST;
    mfence();

    port->signature = regs->sig;

    /* Only ATA disks are driven by this stack. ATAPI/PM/SEMB are skipped —
     * and skipped ALOUD, on the same rule as every other refusal here: a
     * device answered on this cable, and a port that disappears without a word
     * looks to the operator exactly like a port with nothing plugged into it.
     * What names it is AHCI 1.3.1 §3.3.9, "Offset 24h: PxSIG": the register
     * "contains the initial signature of an attached device when the first
     * D2H Register FIS is received from that device". (§3.3.6, cited here
     * before, is PxIE — the port's interrupt enables, which say nothing
     * about what is attached.) */
    if (port->signature != AHCI_SIG_ATA) {
        const char* what = "something whose signature this driver does not know";
        if (port->signature == AHCI_SIG_ATAPI) {
            what = "an ATAPI device — an optical or tape drive, which this "
                   "kernel does not drive";
        } else if (port->signature == AHCI_SIG_SEMB) {
            what = "an enclosure management bridge, which is not a disk";
        } else if (port->signature == AHCI_SIG_PM) {
            what = "a port multiplier, and this driver speaks to one device "
                   "per port";
        }
        kprintf("[AHCI] port %u: what answered here is %s (PxSIG=0x%08x); "
                "not using it\n", port_num, what, port->signature);
        goto fail_free;
    }

    // Learn geometry/capabilities via IDENTIFY DEVICE.
    uint16_t id[256];
    if (ahci_port_identify(port, id) != OK) {
        /* The link is up, the device left BSY/DRQ and it signed itself as an
         * ATA disk — and then would not describe itself. That is a drive this
         * kernel cannot use and an operator can act on, so it is said in the
         * shipped build and not only in a debug one. */
        kprintf("[AHCI] port %u: an ATA device is on the link and will not say "
                "what it is — IDENTIFY DEVICE never completed (TFD=0x%08x "
                "SERR=0x%08x); not using it\n",
                port_num, regs->tfd, regs->serr);
        goto fail_free;
    }

    /* Who it is, before what it can do: words 27-46, 10-19 and 23-26 of the
     * same answer (ACS-3 §7.12.7). Read here, kept in the port, and printed
     * below — until now they were the only part of IDENTIFY this driver threw
     * away, which is why a swapped disk left no trace anywhere. */
    ahci_id_string(id, 27, 20, port->model);
    ahci_id_string(id, 10, 10, port->serial);
    ahci_id_string(id, 23,  4, port->firmware);

    /* Logical sector size: IDENTIFY word 106 is valid when bit 14 is set and
     * bit 15 clear; bit 12 then says words 117-118 carry the logical sector
     * size in 16-bit words. Otherwise the logical sector is 256 words. */
    uint32_t logical = 512;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) && (id[106] & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words >= 256) {
            logical = words * 2u;
        }
    }
    port->logical_sector_size = logical;

    /* And the physical one. Same word: bit 13 says the drive has more than one
     * logical sector per physical, and bits 3:0 are the exponent — one
     * physical block holds 2^N logical ones (ATA8-ACS §7.16.7.74). This is
     * what a 512e drive uses to say "I am addressed in 512 and built from
     * 4096", which nothing in this kernel asked until now. */
    port->physical_sector_size = logical;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) && (id[106] & (1u << 13))) {
        uint32_t exponent = id[106] & 0x0Fu;
        if (exponent < 16 && logical <= (0xFFFFFFFFu >> exponent)) {
            port->physical_sector_size = logical << exponent;
        }
    }

    port->lba48 = (id[83] & (1u << 10)) != 0;
    if (port->lba48) {
        uint64_t s = 0;
        memcpy(&s, &id[100], sizeof(uint64_t));   // words 100-103
        port->total_sectors = s;
    } else {
        uint32_t s = 0;
        memcpy(&s, &id[60], sizeof(uint32_t));     // words 60-61
        port->total_sectors = s;
    }

    /* And from the same word, the other thing it settles: bit 13, "FLUSH CACHE
     * EXT command supported" (ACS-3 §7.12.7). The drive's write cache answers
     * a write the moment it lands in the drive's own DRAM, so what turns
     * "written" into "still there after the power went out" is that one
     * command and nothing else — and a drive that does not have it cannot be
     * asked for durability by any means this kernel possesses. Read here so
     * that the port can say which of the two it is. */
    port->flush_ext = (id[83] & (1u << 13)) != 0;

    /* NCQ usable only if both the HBA (CAP.SNCQ) and the device (IDENTIFY
     * word 76 bit 8) support it. Word 76 reads 0x0000/0xFFFF on non-SATA. */
    bool dev_ncq = (id[76] != 0x0000 && id[76] != 0xFFFF) && (id[76] & (1u << 8));
    port->ncq = ahci_ctrl.ncq_support && dev_ncq;

    /* The storage stack (TagFS, block layer) is built on 512-byte sectors.
     * A 4Kn drive would silently corrupt every LBA computation — refuse the
     * port loudly instead. 512e drives (512 logical / 4096 physical) report
     * logical 512 and work unchanged. */
    if (port->logical_sector_size != 512) {
        /* Said with kprintf, not debug_printf. This removes a disk from the
         * machine, and debug_printf compiles to nothing in a shipped build —
         * so the comment above promised "loudly" while the operator saw a
         * board that simply had no disk on it and no reason given. Every
         * refusal below is on the same rule. */
        kprintf("[AHCI] port %u: %u-byte logical sectors, and this stack is "
                "built on 512 — not using it\n",
                port_num, port->logical_sector_size);
        goto fail_free;
    }

    /* Command slots: the LOWER BOUND of what the HBA offers and what the drive
     * will queue, which is what AHCI 1.3.1 §5.6.4.1 asks for in as many words:
     * "System software must determine the maximum tag allowed by the device
     * and the HBA and it must use the lower bound of the two. For example, if
     * the HBA has 8 entries in its command list, and the SATA device only has
     * 4, only tags 0 - 3 in the device may be used, and only command list
     * entries 0 - 3 may be used in the HBA."
     *
     * The HBA's half was here already (CAP.NCS + 1). The drive's half was
     * never asked for, and on this driver the two are the same question: the
     * same section requires that "software must match the tag number it uses
     * to the slot it is placing the command in", and ahci_build_io does —
     * `cmdfis->countl = slot << 3`. So a slot the drive has no queue entry for
     * is issued as a tag it has no queue entry for, the drive aborts that
     * command, and a task-file error on an NCQ port takes the whole queue with
     * it: every other command in flight on this port fails and the link is
     * reset.
     *
     * The drive's answer is IDENTIFY DEVICE word 75 bits 4:0, "maximum queue
     * depth minus one" (ACS-3 §7.12.7.33) — the same word Linux reads in
     * ata_id_queue_depth(). For non-NCQ ports only one command may be
     * outstanding at all. slot_bitmap: 1 = free. */
    uint32_t usable = (ahci_ctrl.num_slots >= 32)
                      ? 0xFFFFFFFFu
                      : ((1u << ahci_ctrl.num_slots) - 1u);
    if (port->ncq) {
        uint32_t dev_depth = (uint32_t)(id[75] & 0x1Fu) + 1u;
        if (dev_depth < ahci_ctrl.num_slots) {
            kprintf("[AHCI] port %u: the drive queues %u commands and this HBA "
                    "has %u slots — tags %u and above are the drive's to "
                    "refuse, so only %u are used\n",
                    port_num, dev_depth, ahci_ctrl.num_slots,
                    dev_depth, dev_depth);
            usable = (1u << dev_depth) - 1u;
        }
    }
    port->slot_mask   = port->ncq ? usable : 0x1u;
    port->slot_bitmap = port->slot_mask;

    port->active = 1;

    /*
     * ‼ AND HERE THE PORT SAYS WHAT IS ON IT.
     *
     * This was one debug_printf, which klib.h compiles into nothing whenever
     * CONFIG_DEBUG_ENABLED is not set — that is, in every build that has ever
     * been carried to a board. So on the run of 2026-09-20 two SATA drives
     * were brought up, identified, seated in the Boardroom and read from, and
     * the entire 719-line log said of them: "AHCI port 4" and "AHCI port 5".
     * Every refusal on the way here was already loud; SUCCESS was the one
     * thing that was not, and there is no second channel — nothing outside
     * this driver ever repeats what IDENTIFY said.
     *
     * Four lines, and each of them is one of the questions an operator asks
     * standing in front of the machine: is that my disk and is it still the
     * same one, is all of it there and on what grid, did the link come up at
     * the speed I paid for, and will a write on it survive the power going
     * out. Nothing below is inferred: every value was read off this drive or
     * this HBA within the last few milliseconds.
     */
    if (port->model[0] != '\0') {
        kprintf("[AHCI port %u] %s, firmware %s, serial %s\n",
                port_num, port->model,
                port->firmware[0] != '\0' ? port->firmware : "not given",
                port->serial[0]   != '\0' ? port->serial   : "not given");
    } else {
        kprintf("[AHCI port %u] the drive answered IDENTIFY with a blank model "
                "field, so this port has nothing to be told apart from the "
                "next disk by\n", port_num);
    }

    {
        uint64_t mib = (port->total_sectors
                        * (uint64_t)port->logical_sector_size) / (1024ULL * 1024ULL);
        if (port->physical_sector_size != port->logical_sector_size) {
            kprintf("[AHCI port %u] %llu sectors, %llu MiB, addressed in "
                    "%u-byte sectors and built from %u-byte ones — a write "
                    "that does not cover a whole physical sector costs this "
                    "drive a read and a rewrite\n",
                    port_num, (unsigned long long)port->total_sectors,
                    (unsigned long long)mib, port->logical_sector_size,
                    port->physical_sector_size);
        } else {
            kprintf("[AHCI port %u] %llu sectors, %llu MiB, addressed in "
                    "%u-byte sectors and built from them too\n",
                    port_num, (unsigned long long)port->total_sectors,
                    (unsigned long long)mib, port->logical_sector_size);
        }
    }

    {
        const char* gen     = ahci_link_gen_name(port->link_speed);
        uint8_t     hba_gen = (uint8_t)((ahci_ctrl.cap >> AHCI_CAP_ISS_SHIFT)
                                        & AHCI_CAP_ISS_MASK);
        char queueing[192];

        if (port->ncq) {
            ksnprintf(queueing, sizeof(queueing),
                      "NCQ on with %u tag(s)",
                      CountSetBits(port->slot_mask));
        } else if (!ahci_ctrl.ncq_support) {
            ksnprintf(queueing, sizeof(queueing),
                      "no NCQ, because this HBA has none to offer (CAP.SNCQ=0) "
                      "— one command at a time on this port");
        } else {
            ksnprintf(queueing, sizeof(queueing),
                      "no NCQ, because the drive refuses it while this HBA "
                      "offers it (IDENTIFY word 76 = 0x%04x) — one command at "
                      "a time on this port", (unsigned)id[76]);
        }

        if (gen != NULL) {
            kprintf("[AHCI port %u] link %s, LBA%s addressing, %s\n",
                    port_num, gen, port->lba48 ? "48" : "28", queueing);
        } else {
            kprintf("[AHCI port %u] the link is up and names no negotiated "
                    "speed (PxSSTS.SPD=%uh), LBA%s addressing, %s\n",
                    port_num, port->link_speed,
                    port->lba48 ? "48" : "28", queueing);
        }

        /* THE ONE THAT COSTS MONEY QUIETLY. A link that came up a generation
         * or two below what the controller can drive works perfectly and is
         * two or four times slower than the machine the owner built, and
         * nothing else anywhere ever mentions it. Both fields are the same
         * encoding — AHCI 1.3.1 §3.1.1 for CAP.ISS, §3.3.10 for PxSSTS.SPD —
         * so the comparison is the register's own, not this driver's idea of
         * one. */
        if (gen != NULL && hba_gen != 0 && port->link_speed < hba_gen) {
            const char* ceiling = ahci_link_gen_name(hba_gen);
            kprintf("[AHCI port %u] this HBA drives a port at %s and this link "
                    "settled at %s — the cable, the connector or the drive is "
                    "holding it down, and every transfer on this port pays the "
                    "difference (SSTS=0x%08x)\n",
                    port_num,
                    ceiling != NULL ? ceiling : "a generation this driver "
                                                "does not name",
                    gen, regs->ssts);
        }
    }

    if (port->flush_ext) {
        kprintf("[AHCI port %u] the drive supports FLUSH CACHE EXT, so a write "
                "on this port can be made to survive the power going out\n",
                port_num);
    } else {
        kprintf("[AHCI port %u] the drive does not support FLUSH CACHE EXT "
                "(IDENTIFY word 83 = 0x%04x) — nothing this kernel can issue "
                "makes a write on this port survive the power going out\n",
                port_num, (unsigned)id[83]);
    }

    return 0;

fail_free:
    /* The controller is made to let go of this memory BEFORE the PMM is told
     * to take it back.
     *
     * Everything that jumps here has already run `regs->cmd |= AHCI_PCMD_FRE`
     * with PxFB holding the physical address of clb_page + 1024, and AHCI
     * 1.3.1 §3.3.7 makes that bit exactly the controller's permission to write
     * there: "FIS Receive Enable (FRE): When set, the HBA may post received
     * FISes into the FIS receive area pointed to by PxFB (and for 64-bit HBAs,
     * PxFBU)." A drive that finishes its OOB handshake a moment after this
     * driver stopped waiting for it sends its D2H Register FIS anyway, and
     * nothing ever reprograms this port's registers again — so the page would
     * be written by the controller long after the PMM handed it to somebody
     * else. That is not a leak, it is memory corruption arriving over SATA.
     *
     * The order is the spec's, §10.3.2: "Software shall not clear this bit
     * while PxCMD.ST or PxCMD.CR is set to '1'", and "When this bit is cleared
     * to '0', software must first wait for PxCMD.FR to clear to '0', indicating
     * that the DMA engine for FIS reception is in an idle condition. When
     * PxCMD.FR and PxCMD.FRE are both cleared to '0', software may update the
     * values of PxFB and PxFBU." ahci_port_stop() is precisely that walk — ST
     * down, CR observed at 0, FRE down, FR observed at 0 — which is why the
     * addresses below may be erased only after it returns, and never before.
     *
     * And if the port will not stop, the pages are not returned at all. A page
     * the controller may still write to is not a page this kernel owns; the
     * cheap half of that trade is holding AHCI_MAX_SLOTS+1 of them.
     *
     * ‼ NOR ARE THEY RETURNED BY A PORT THAT IS BEING BUILT AGAIN. Only a
     * port that never entered service gives its memory back here: a port that
     * once was in service may still be named by a submitter that read it an
     * instant before it was retired, and the pages it is writing into must
     * not be handed to anybody else. See the note at the head of this
     * function. The engine is still stopped either way — that is what keeps
     * the controller out of them. */
    if (ahci_port_stop(port) != 0) {
        kprintf("[AHCI] port %u: will not stop receiving FIS (CMD=0x%08x), so "
                "its command list and FIS page stay with the controller — "
                "%u pages held, not freed\n",
                port_num, regs->cmd, (unsigned)(AHCI_MAX_SLOTS + 1));
        return -1;
    }

    if (rebuilt) {
        return -1;
    }

    regs->clb  = 0;
    regs->clbu = 0;
    regs->fb   = 0;
    regs->fbu  = 0;
    mfence();

    for (uint8_t i = 0; i < AHCI_MAX_SLOTS; i++) {
        if (port->ctba_phys[i]) {
            pmm_free((void*)port->ctba_phys[i], 1);
            port->ctba_phys[i] = 0;
            port->ctba_virt[i] = NULL;
        }
    }
    if (port->clb_phys) {
        pmm_free((void*)port->clb_phys, 1);
    }
    port->clb_virt = NULL;
    port->clb_phys = 0;
    port->fis_virt = NULL;
    port->fis_phys = 0;
    return -1;
}

static int ahci_init_ports(void) {
    if (!ahci_ctrl.initialized) {
        return -1;
    }

    uint32_t pi = ahci_ctrl.hba_mem->pi;
    ahci_ctrl.port_implemented = pi;

    /* WHICH SOCKETS EXIST, by name and not only as a bitmask. AHCI 1.3.1
     * §10.1.2 step 2 makes PI the answer to "which ports are implemented by
     * the HBA", and without it on the console a run of refusals cannot be
     * checked against anything: four ports saying "not using it" is a
     * different machine depending on whether this HBA has four ports or
     * eight. The register value is kept beside the list, because a board's
     * documentation is written in hexadecimal and its case is written in
     * numbers. */
    {
        char     list[AHCI_MAX_PORTS * 3 + 1];
        unsigned at = 0;
        for (unsigned i = 0; i < AHCI_MAX_PORTS; i++) {
            if (!(pi & (1U << i))) continue;
            at += (unsigned)ksnprintf(list + at, sizeof(list) - at,
                                      at == 0 ? "%u" : " %u", i);
        }
        if (at == 0) list[0] = '\0';

        kprintf("[AHCI] ports implemented: 0x%08x — %u of %u register sets, "
                "ports %s; every one of them is brought up in turn, because a "
                "port's DET says nothing until its link has been waited for\n",
                pi, CountSetBits(pi), (unsigned)AHCI_MAX_PORTS,
                at == 0 ? "none" : list);
    }

    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1U << i))) continue;

        /* Every implemented port is brought up. There is deliberately no
         * "is anything there?" pre-check here, and the one that used to stand
         * in this spot had the spec exactly backwards: it read PxSSTS.DET once,
         * with no delay, and skipped the port unless it already read 3h — and
         * it did that ONLY when CAP.SSS was clear, calling DET "already
         * meaningful" there. AHCI 1.3.1 §10.4.3 says the opposite: "If the HBA
         * does not support staggered spin-up, the HBA reset shall cause a
         * COMRESET to be sent on the port", and §7.3 says that COMRESET clears
         * PxSSTS.DET to 0h. So on every ordinary desktop HBA that read landed
         * microseconds after ahci_init's GHC.HR, on a register the reset had
         * just zeroed, and skipped every port on the controller — the machine
         * came up with no disk and (debug_printf compiling to nothing) no
         * reason given. QEMU cannot show this: its SCR_STAT read handler
         * synthesises DET=3h whenever a drive is attached, whatever the link
         * is doing. The link is judged where the waiting is done, in
         * ahci_port_init, against a clock. */
        debug_printf("[AHCI] Port %u: bringing up...\n", i);

        if (ahci_port_init(i) == 0) {
            ahci_ctrl.num_active_ports++;
            debug_printf("[AHCI] Port %u: Active\n", i);
        } else {
            debug_printf("[AHCI] Port %u: not active\n", i);
        }
    }

    return ahci_ctrl.num_active_ports > 0 ? 0 : -1;
}

error_t ahci_port_comreset(ahci_port_t* port) {
    if (!port || !port->regs) {
        return ERR_NULL_POINTER;
    }

    volatile ahci_port_regs_t* regs = port->regs;

    debug_printf("[AHCI] Port %u: Performing COMRESET...\n", port->port_num);

    __atomic_fetch_add(&port->stats.comreset_count, 1, __ATOMIC_RELAXED);

    /* A command engine that will not stop is not a reason to abandon the
     * reset — it is the case the reset exists for. AHCI 1.3.1 §10.4.2: "It
     * must first clear PxCMD.ST, and wait for PxCMD.CR to clear to '0' before
     * re-initializing communication. However, if PxCMD.CR does not clear
     * within a reasonable time (500 milliseconds), it may assume the interface
     * is in a hung condition and may continue with issuing the port reset."
     *
     * This used to return ERR_IO here, before PxSCTL.DET had been written even
     * once — so the one action that clears a hung interface was never taken,
     * on the one port that needed it. And ahci_port_stop leaves PxCMD.ST at 0
     * on its way out while nothing outside ahci_port_init ever sets it again:
     * a transient wedge was turned into a port that could not run another
     * command for the rest of the boot, by the routine whose job was to
     * recover it. Every later command sat in PxCI with the engine stopped
     * until its own patience ran out.
     *
     * So the failure is said out loud — kprintf, because the debug_printf that
     * stood here compiles to nothing in every shipped build — and the reset
     * goes ahead. Whether the engine came back is not assumed either: it is
     * decided at the end by ahci_port_start, which returns a refusal of its
     * own if PxCMD.CR is still set. */
    if (ahci_port_stop(port) != 0) {
        kprintf("[AHCI] port %u: its command engine will not stop "
                "(CMD=0x%08x) — AHCI 1.3.1 §10.4.2 calls that a hung "
                "interface and resets the port through it\n",
                port->port_num, regs->cmd);
    }

    regs->serr = 0xFFFFFFFF;

    uint32_t sctl = regs->sctl;
    sctl = (sctl & ~0xF) | 0x1;
    regs->sctl = sctl;

    /* Hold DET=1h for the millisecond AHCI 1.3.1 §10.4.2 asks for. There is no
     * register to watch here — the hold IS the requirement — so the glass runs
     * out on purpose and nothing exits early. */
    {
        HourGlass hold;
        if (!HourGlassTurn(&hold, AHCI_COMRESET_HOLD_US)) {
            /* A hold has no hardware fact to wait on, so there is nothing to
             * wait for unbounded and nothing honest to substitute. The reset is
             * abandoned — and DET goes back to 0h on the way out, because a
             * port left at 1h is a port held in COMRESET for the rest of the
             * boot: the undo matters more here than the diagnosis. */
            regs->sctl = sctl & ~0xFu;
            kprintf("[AHCI] no time source — cannot hold PxSCTL.DET=1h for the "
                    "millisecond AHCI 1.3.1 §10.4.2 requires; port %u reset "
                    "abandoned and the port released\n", port->port_num);
            __atomic_fetch_add(&port->stats.comreset_fail_count, 1, __ATOMIC_RELAXED);
            return ERR_TIMEOUT;
        }
        while (!HourGlassRunOut(&hold)) cpu_pause();
    }

    sctl = regs->sctl;
    sctl = (sctl & ~0xF);
    regs->sctl = sctl;

    /* And now the other quantity entirely: how long the link gets to come back.
     * The exit is PxSSTS.DET reaching 3h and nothing else — no "DET has been 0
     * for a while" shortcut, because a guessed settling interval is exactly the
     * kind of number this change exists to remove. */
    HourGlass link;
    if (!HourGlassTurn(&link, (uint64_t)AHCI_COMRESET_LINK_MS * 1000ULL)) {
        kprintf("[AHCI] no time source — waiting for port %u's link on the "
                "fact itself, with no bound\n", port->port_num);
        for (;;) {
            uint32_t ssts = regs->ssts;
            if (((ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK)
                == AHCI_SSTS_DET_PRESENT) break;
            cpu_pause();
        }
    } else {
        for (;;) {
            uint32_t ssts = regs->ssts;
            if (((ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK)
                == AHCI_SSTS_DET_PRESENT) break;
            if (HourGlassRunOut(&link)) {
                kprintf("[AHCI] port %u: the link never came back — %lu us on "
                        "the %s (SSTS=0x%08x)\n", port->port_num,
                        (unsigned long)HourGlassElapsedUs(&link),
                        HourGlassRulerName(&link), regs->ssts);
                __atomic_fetch_add(&port->stats.comreset_fail_count, 1,
                                   __ATOMIC_RELAXED);
                return ERR_TIMEOUT;
            }
            cpu_pause();
        }
    }

    debug_printf("[AHCI] Port %u: COMRESET successful (SSTS=0x%08x)\n",
                 port->port_num, regs->ssts);
    regs->serr = 0xFFFFFFFF;
    regs->is = 0xFFFFFFFF;
    if (ahci_port_start(port) == 0) {
        return OK;
    }

    debug_printf("[AHCI] Port %u: COMRESET recovered the link but the engine "
                 "would not start\n", port->port_num);
    __atomic_fetch_add(&port->stats.comreset_fail_count, 1, __ATOMIC_RELAXED);
    return ERR_TIMEOUT;
}

/*
 * ‼ A PORT THAT DID NOT COME BACK STOPS PRETENDING TO BE ONE.
 *
 * What used to happen here was `port->status = AHCI_PORT_ERROR` — a field
 * with four writes in this file and NOT ONE READ anywhere in the tree. So a
 * port whose link was gone for good stayed `active`, ahci_get_port went on
 * handing it out, the Boardroom went on offering its seat, and the filesystem
 * on it went on issuing commands that could only fail: every read a full
 * patience window long, every write the same, for the rest of the boot.
 *
 * A medium leaving is not a new idea in this kernel and does not need a new
 * mechanism. A USB stick that is pulled out calls BoardroomNoteDeparture, the
 * room empties its chair and says "seat N is empty" on Touch, and TagFS is
 * already listening for exactly that — so a port that has died goes out the
 * same door, and everything that already knows how to lose a medium loses
 * this one too, unchanged.
 *
 * The order matters and is the order below:
 *
 *   1. The retirement is CLAIMED under the port's own lock, by clearing
 *      `active` and reading what it was. Two cores can reach here for the
 *      same port at the same instant — this one's recovery and the
 *      watchdog's — and only the core that found it in service goes on, so
 *      the room is told exactly once and the count below moves exactly once.
 *      From that store onwards every ahci_* entry point, all of which go
 *      through ahci_get_port, answers as it does for a port that was never
 *      there.
 *   2. Every slot still in flight is then FAILED, through the same
 *      claim-checked retire the IRQ path uses, so a waiter parked on a
 *      callback is woken with ERR_IO rather than left waiting on a port that
 *      no longer exists. ahci_claim makes that exactly-once even against the
 *      MSI handler retiring the same slots on the BSP. The ahci_free_slot
 *      inside it is a no-op by now, which costs nothing: the pool was emptied
 *      in step 1 and nothing may draw from it again.
 *   3. ‼ THE PORT'S INTERRUPT IS DISARMED, AND NOTHING ELSE IN THIS KERNEL
 *      WOULD EVER HAVE DONE IT AGAIN. Both readers of PxIS — ahci_irq_handler
 *      and ahci_watchdog_scan — skip a port that is not `active`, and the
 *      handler's last act is `GHC.IS = is`. A port that dies with PxIS.TFES
 *      standing and PxIE.TFES enabled therefore re-raises its bit in GHC.IS
 *      the instant it is cleared, for ever: AHCI 1.3.1 §10.7.2.1, "If
 *      software clears IS register only, leaving the level of the virtual
 *      wire from the PxIS register set, the resulting level of '1' shall
 *      cause the IS register bit to be re-set", and "any change to the IS
 *      register that doesn't result in the register being all cleared shall
 *      cause an MSI to be sent". That is an interrupt storm on MSI and a
 *      permanently asserted line on INTx — and it is reachable, because
 *      ahci_port_comreset only W1Cs PxIS on the path where it SUCCEEDED, and
 *      the only way here is the path where it did not. PxIE first, then PxIS,
 *      then this port's bit in GHC.IS: the level is taken down before the
 *      latch, so nothing can re-raise it behind us.
 *   4. The engine is stopped, which is both the end of the port's DMA and the
 *      one thing that makes a later rebuild possible (ahci_port_init begins
 *      by stopping it too). The command list and FIS pages are NOT given
 *      back; see the note at the head of ahci_port_init for why a port that
 *      has been in service keeps them for good.
 *   5. The room is told LAST, and outside the port lock: BoardroomNoteDeparture
 *      takes a lock of its own and publishes a Touch event. Nothing of this
 *      port is passed to it — only the kind and the index — so there is
 *      nothing for anybody downstream to dereference. It is said HERE rather
 *      than deferred to a K-Core on purpose: the Boardroom's own contract
 *      says a departure must not be deferred, because a seat holds a
 *      controller index and a deferred announcement can name a chair that
 *      has since been taken by something else. What makes saying it from a
 *      driver's own I/O path safe is the listener: seat:emptied reaches
 *      TagFSNoteMediumGone, which reads seat state, forgets the read-ahead
 *      and prints one line — no disk I/O and no TagFS lock. The half that
 *      DOES mount and read sectors hangs off seat:taken, and that one is
 *      already two-step by design (BoardroomNoteArrival raises a flag; the
 *      guide loop does the work). Anyone making that listener heavier is
 *      changing this, and should read this paragraph first.
 *
 * The port structure itself is never freed — it is a member of
 * ahci_ctrl.ports[] — so a pointer anybody still holds stays a pointer to
 * memory, and every function that takes one re-asks ahci_get_port.
 */
static void ahci_port_retire(ahci_port_t* port) {
    uint8_t idx = port->port_num;

    spin_lock(&port->lock);
    bool mine = port->active != 0;
    port->active      = 0;
    port->slot_bitmap = 0;
    port->slot_held   = false;
    spin_unlock(&port->lock);

    if (!mine) return;

    uint32_t stuck = __atomic_load_n(&port->issued_mask, __ATOMIC_ACQUIRE);
    if (stuck) {
        uint32_t won = ahci_claim(port, stuck);
        if (won) ahci_retire_slots(port, idx, won, ERR_IO);
    }

    /* Step 3 — the virtual wire is taken down before the latch it comes from,
     * and then this port's bit in GHC.IS (AHCI 1.3.1 §10.7.2.1, quoted
     * above). Nobody clears PxIS for an inactive port, so leaving either of
     * these standing is a bit that re-raises itself for the rest of the
     * boot. */
    volatile ahci_port_regs_t* regs = port->regs;
    regs->ie = 0;
    mfence();
    regs->is = 0xFFFFFFFF;
    ahci_ctrl.hba_mem->is = (1U << idx);

    /* And PxSERR with them, which is not tidiness: it ARMS THE WAY BACK. The
     * disconnect this port just died of has left PxSERR.DIAG.X standing, and
     * that latch is exactly what the watchdog reads to decide a device has
     * arrived. Left as it is, the very next tick would read the departure as
     * an arrival and spend a K-Core on a bring-up of a port with no cable in
     * it. Cleared, the next X on this register is a COMINIT that genuinely
     * happened after the port left. */
    regs->serr = 0xFFFFFFFF;

    /* Step 4 — the engine down, the pages kept. A port whose engine will not
     * stop is said out loud and left alone: ahci_port_init begins with the
     * same walk, so a rebuild of it refuses itself rather than issuing into
     * a controller that never let go. */
    if (ahci_port_stop(port) != 0) {
        kprintf("[AHCI] port %u: it is leaving with its command engine still "
                "running (CMD=0x%08x) — its pages stay with the controller\n",
                idx, regs->cmd);
    }

    /* One down, without a lock of its own: ahci_init_ports counts up on the
     * BSP before any of this can run, and from then on this is the only
     * writer — but there can be two of it at once, on two dying ports. */
    uint8_t seen = __atomic_load_n(&ahci_ctrl.num_active_ports, __ATOMIC_RELAXED);
    while (seen > 0 &&
           !__atomic_compare_exchange_n(&ahci_ctrl.num_active_ports, &seen,
                                        (uint8_t)(seen - 1), true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        /* seen was refreshed by the exchange; try again. */
    }

    BoardroomNoteDeparture(BOARD_AHCI, idx);

    /* LAST of all, and that order is the whole of it: this store is what
     * makes the port eligible to come back, and a port may not be seated
     * again before the room has heard that it left. Set any earlier, a
     * COMINIT landing in the microsecond between could have the watchdog
     * post a re-admission that announces an ARRIVAL into a room still
     * holding this port's old chair. */
    __atomic_store_n(&port->standing, (uint8_t)AHCI_STANDING_LEFT,
                     __ATOMIC_RELEASE);
}

/*
 * ‼ AND THE WAY BACK, WHICH IS THE SAME ROAD IN THE OTHER DIRECTION.
 *
 * A port leaving is only half a mechanism. A USB stick has both halves — it
 * goes out through BoardroomNoteDeparture and comes back through
 * BoardroomNoteArrival — and a SATA port with only the first half turns every
 * link glitch that outlasts AHCI_MAX_COMRESET_ATTEMPTS into a disk this
 * machine has lost until somebody power-cycles it. A yanked cable pushed back
 * in, a drive that browned out, a backplane reseated: all of those used to
 * come back, because the old code left the port in service and the next
 * command's recovery found the link again.
 *
 * WHAT SAYS IT CAME BACK IS THE SAME EVENT THAT SAID IT WAS THERE AT BOOT: a
 * COMINIT, read off the latch PxSERR.DIAG.X (AHCI 1.3.1 §3.3.12, "shall
 * always be set to one anytime a COMINIT signal is received"). The latch is
 * why nothing has to be armed or watched for: the port's interrupt was
 * disarmed when it left, and a bit that stays set until software clears it
 * cannot be missed by a glance once a tick.
 *
 * And the way back is ahci_port_init itself, not a shortcut through it. The
 * drive on that cable after a reseat may not be the drive that was on it
 * before, so its IDENTIFY — size, sector geometry, queue depth — has to be
 * asked again from the beginning. An attempt that fails costs nothing and
 * leaves the port where it was: the next COMINIT brings it back here.
 */
static void ahci_port_readmit(ahci_port_t* port) {
    uint8_t idx = port->port_num;

    kprintf("[AHCI] port %u: a COMINIT arrived on a port that had left the "
            "machine — trying to bring it back\n", idx);

    /* The latch is spent the moment it is acted on. Left standing it would
     * summon this again on the next tick, whatever the outcome below. */
    port->regs->serr = 0xFFFFFFFF;

    if (ahci_port_init(idx) != 0) {
        /* ahci_port_init zeroed the struct, so the port's standing has to be
         * said again — otherwise the scan that found this COMINIT would never
         * look at this port a second time. */
        __atomic_store_n(&port->standing, (uint8_t)AHCI_STANDING_LEFT,
                         __ATOMIC_RELEASE);
        kprintf("[AHCI] port %u: it did not come back up; it stays out of the "
                "machine until the link says something again\n", idx);
        return;
    }

    __atomic_fetch_add(&ahci_ctrl.num_active_ports, 1, __ATOMIC_RELAXED);

    /* Its interrupt is armed again — ahci_port_enable_irq goes through
     * ahci_get_port, so this could not have been done before `active`. */
    ahci_port_enable_irq(idx);

    kprintf("[AHCI] port %u: back in the machine, %llu sectors — the room is "
            "told so its seat can be taken again\n",
            idx, (unsigned long long)port->total_sectors);

    /* The room, the same way a USB stick announces itself: a flag here, the
     * seating itself in the guide loop where reading sectors is allowed. */
    BoardroomNoteArrival();
}

error_t ahci_port_recover(ahci_port_t* port) {
    if (!port || !port->regs) {
        return ERR_NULL_POINTER;
    }

    debug_printf("[AHCI] Port %u: Starting error recovery...\n", port->port_num);

    __atomic_fetch_add(&port->stats.error_count, 1, __ATOMIC_RELAXED);

    for (int attempt = 0; attempt < AHCI_MAX_COMRESET_ATTEMPTS; attempt++) {
        error_t result = ahci_port_comreset(port);
        if (result == OK) {
            debug_printf("[AHCI] Port %u: Recovery successful (attempt %d/%d)\n",
                         port->port_num, attempt + 1, AHCI_MAX_COMRESET_ATTEMPTS);
            return OK;
        }

        debug_printf("[AHCI] Port %u: COMRESET attempt %d/%d failed\n",
                     port->port_num, attempt + 1, AHCI_MAX_COMRESET_ATTEMPTS);
    }

    /* kprintf, on this file's own rule (see the 512-byte-sector refusal): a
     * port that failed every COMRESET it is given is a disk this boot has
     * effectively lost, and debug_printf compiles to nothing in a shipped
     * build — so on real hardware the operator saw commands fail one by one
     * with no reason ever named. */
    kprintf("[AHCI] port %u: its link did not come back after %d COMRESETs — "
            "it is leaving the machine; nothing more will be read from it or "
            "written to it until the link says a device is there again\n",
            port->port_num, AHCI_MAX_COMRESET_ATTEMPTS);
    ahci_port_retire(port);
    return ERR_IO;
}

const char* ahci_decode_serr(uint32_t serr) {
    if (serr & (1 << 26)) return "Exchanged";
    if (serr & (1 << 25)) return "UnrecognizedFIS";
    if (serr & (1 << 24)) return "TransportStateTrans";
    if (serr & (1 << 23)) return "LinkSeqError";
    if (serr & (1 << 22)) return "HandshakeError";
    if (serr & (1 << 21)) return "CRCError";
    if (serr & (1 << 20)) return "Disparity";
    if (serr & (1 << 19)) return "10bTo8bDecodeError";
    if (serr & (1 << 18)) return "CommWake";
    if (serr & (1 << 17)) return "PhyInternalError";
    if (serr & (1 << 16)) return "PhyRdyChange";
    if (serr & (1 << 11)) return "InternalError";
    if (serr & (1 << 10)) return "ProtocolError";
    if (serr & (1 << 9))  return "PersistentCommError";
    if (serr & (1 << 8))  return "TransientDataError";
    if (serr & (1 << 1))  return "RecoveredCommError";
    if (serr & (1 << 0))  return "RecoveredDataError";
    return "NoError";
}

void ahci_log_error(ahci_port_t* port, uint32_t pxis) {
    if (!port || !port->regs) {
        return;
    }

    volatile ahci_port_regs_t* regs = port->regs;
    uint32_t serr = regs->serr;
    uint32_t tfd = regs->tfd;

    __atomic_store_n(&port->stats.last_error_tsc, rdtsc(), __ATOMIC_RELAXED);
    __atomic_store_n(&port->stats.last_serr,      serr,    __ATOMIC_RELAXED);
    __atomic_store_n(&port->stats.last_tfd,       tfd,     __ATOMIC_RELAXED);

    if (pxis & AHCI_PIS_TFES) {
        __atomic_fetch_add(&port->stats.tfes_count, 1, __ATOMIC_RELAXED);
    }

    debug_printf("[AHCI] Port %u ERROR: IS=0x%08x SERR=0x%08x TFD=0x%08x (%s)\n",
                 port->port_num, pxis, serr, tfd, ahci_decode_serr(serr));
}

void ahci_get_port_stats(uint8_t port_num, ahci_port_stats_t* stats_out) {
    if (!stats_out) return;
    ahci_port_t* port = ahci_get_port(port_num);
    if (!port) return;

    /* Field-by-field atomic loads. Plain memcpy here would race with the IRQ
     * path that writes stats fields via __atomic_store_n. Tearing across
     * fields is acceptable since the snapshot is diagnostic. */
    stats_out->cmd_count           = __atomic_load_n(&port->stats.cmd_count,           __ATOMIC_RELAXED);
    stats_out->error_count         = __atomic_load_n(&port->stats.error_count,         __ATOMIC_RELAXED);
    stats_out->timeout_count       = __atomic_load_n(&port->stats.timeout_count,       __ATOMIC_RELAXED);
    stats_out->tfes_count          = __atomic_load_n(&port->stats.tfes_count,          __ATOMIC_RELAXED);
    stats_out->comreset_count      = __atomic_load_n(&port->stats.comreset_count,      __ATOMIC_RELAXED);
    stats_out->comreset_fail_count = __atomic_load_n(&port->stats.comreset_fail_count, __ATOMIC_RELAXED);
    stats_out->last_error_tsc      = __atomic_load_n(&port->stats.last_error_tsc,      __ATOMIC_RELAXED);
    stats_out->last_serr           = __atomic_load_n(&port->stats.last_serr,           __ATOMIC_RELAXED);
    stats_out->last_tfd            = __atomic_load_n(&port->stats.last_tfd,            __ATOMIC_RELAXED);
}

/* GHC.AE — the first register write this driver makes, and the permission
 * every other register access afterwards rests on.
 *
 * AHCI 1.3.1 §3.1.2, Offset 04h bit 31: "Software shall set this bit to '1'
 * before accessing other AHCI registers... The implementation of this bit is
 * dependent upon the value of the CAP.SAM bit. If CAP.SAM is '0', then GHC.AE
 * shall be read-write and shall have a reset value of '0'." §5.3.2.3 says what
 * happens if it is skipped: "In implementations where CAP.SAM is cleared to
 * '0', software shall set GHC.AE to '1' before accessing AHCI registers other
 * than the GHC register. If software accesses AHCI registers other than GHC
 * when GHC.AE is cleared to '0' the results are indeterminate." And §10.1.2
 * puts it first in an ordered list that reads CAP fourth: "1. Indicate that
 * system software is AHCI aware by setting GHC.AE to '1'... 4. Determine how
 * many command slots the HBA supports, by reading CAP.NCS."
 *
 * GHC itself is the one register that clause exempts, so reading it here to
 * find out whether AE is already set — CAP.SAM='1' makes AE read-only '1', and
 * firmware that already ran an AHCI driver leaves it set — is legal.
 *
 * The write is retried because the bit does not always take on the first
 * attempt: Linux carries the same loop in ahci_enable_ahci() with "Some
 * controllers need AHCI_EN to be written multiple times. Try a few times
 * before giving up." — five writes, a read-back after each, 10 ms apart. Where
 * this differs from Linux is the ending: Linux WARN_ONs and drives the
 * controller anyway, and this returns false, because a CAP read taken with AE
 * clear is not a small error. CAP.NCS decides the slot bitmap, CAP.S64A is the
 * only guard keeping a PRDT address above 4 GiB out of a 32-bit-only HBA, and
 * an indeterminate answer to either is a number nobody can act on. */
#define AHCI_AE_WRITE_ATTEMPTS  5
#define AHCI_AE_SETTLE_US       10000u

static bool ahci_enable_ae(void) {
    uint32_t ghc = ahci_ctrl.hba_mem->ghc;
    if (ghc & AHCI_GHC_AE) return true;

    bool said = false;
    for (int attempt = 0; attempt < AHCI_AE_WRITE_ATTEMPTS; attempt++) {
        ahci_ctrl.hba_mem->ghc = ghc | AHCI_GHC_AE;
        mfence();
        ghc = ahci_ctrl.hba_mem->ghc;
        if (ghc & AHCI_GHC_AE) return true;

        HourGlass settle;
        if (!HourGlassTurn(&settle, AHCI_AE_SETTLE_US)) {
            /* No ruler yet is no reason to stop asking, and no reason to
             * pretend the interval was spent either: the attempts go back to
             * back and the board is told that is what happened. */
            if (!said) {
                kprintf("[AHCI] no time source — the %u ms between AHCI-enable "
                        "attempts cannot be spent, so the %d writes go back to "
                        "back\n", AHCI_AE_SETTLE_US / 1000u,
                        AHCI_AE_WRITE_ATTEMPTS);
                said = true;
            }
            continue;
        }
        while (!HourGlassRunOut(&settle)) cpu_pause();
    }
    return false;
}

/* AHCI 1.3.1 §10.6.3 — BIOS/OS handoff, and the one question it exists to
 * answer: does this kernel own the HBA yet?
 *
 * On platforms that share the controller with SMM firmware (CAP2.BOH set —
 * common on real laptops and desktops) the OS asks for ownership before
 * touching anything else, otherwise BIOS SMI code and this driver drive the
 * same registers at the same time. A true no-op on QEMU, which never
 * advertises BOH. Requires hba_mem already mapped.
 *
 * What it used to do was ask and never listen: it returned void, so the caller
 * could not act; it spun one second — a number §10.6.3 never names — on
 * BOHC.BOS and then said "continuing" through a debug_printf that compiles to
 * nothing in every shipped build; it watched BOHC.BB afterwards, which §10.6.4
 * has the firmware clear at step 7, one step BEFORE it releases BOS at step 8,
 * so that wait could end with the firmware still owning the controller; and it
 * printed "handoff complete" either way. The §3.1.11 condition that decides
 * the whole matter — "Ownership is obtained when this bit [OOS] reads '1' and
 * the BOS bit reads '0'" — was never evaluated once.
 *
 * So it now evaluates that condition and reports it: true when this kernel may
 * drive the HBA with the specification behind it — either ownership as
 * §3.1.11 defines it, or the case §10.6.3 step 5 expressly permits taking
 * control in — and false with the reason already on the console otherwise.
 *
 * WHAT IT IS NOT IS A VETO. §10.6.3 step 4 calls two seconds "a minimum"; it
 * names no upper bound and nowhere says to abandon the controller, so firmware
 * that legitimately needs longer — spinning drives down — is inside the
 * specification, and giving up the HBA over it would cost this machine the
 * only disks it has, boot volume included, with nothing to fall back on. The
 * field agrees: mainline Linux performs no AHCI handoff at all and resets the
 * controller unconditionally, and on the analogous EHCI and xHCI BIOS handoffs
 * it warns, takes ownership by force and carries on. So this asks properly,
 * waits out the windows the specification does name, and hands the caller the
 * truth to say out loud. */
/* §3.1.11, verbatim: "Ownership is obtained when this bit [OOS] reads '1' and
 * the BOS bit reads '0'." One question, asked in one place. */
static bool ahci_boh_owned(uint32_t bohc) {
    return (bohc & (AHCI_BOHC_BOS | AHCI_BOHC_OOS)) == AHCI_BOHC_OOS;
}

static bool ahci_bios_handoff(uint32_t cap2) {
    if (!(cap2 & AHCI_CAP2_BOH)) {
        /* §10.6.1: BOS and OOS are '0' out of reset, and a controller with no
         * handoff mechanism has no BIOS semaphore to wait on. */
        return true;
    }

    debug_printf("[AHCI] BIOS/OS handoff: requesting OS ownership...\n");

    /* §10.6.3 step 1. Other bits preserved. */
    ahci_ctrl.hba_mem->bohc |= AHCI_BOHC_OOS;
    mfence();

    /* §10.6.3 steps 3-5: the firmware has 25 ms to either release the
     * semaphore or declare itself busy. Watching BOS here and not BB alone is
     * the point — BOS is the bit §3.1.11 defines ownership by. */
    HourGlass window;
    if (!HourGlassTurn(&window, (uint64_t)AHCI_BOH_BUSY_WINDOW_MS * 1000ULL)) {
        /* No clock yet means no window can be held open — it does not mean the
         * register cannot be read. One honest reading, said out loud, beats
         * both a silent assumption and an unbounded spin. */
        uint32_t once = ahci_ctrl.hba_mem->bohc;
        kprintf("[AHCI] no time source — the %u ms AHCI 1.3.1 §10.6.3 gives the "
                "firmware to answer cannot be held open, so the handoff is one "
                "reading and no wait (BOHC=0x%08x)\n",
                AHCI_BOH_BUSY_WINDOW_MS, once);
        return ahci_boh_owned(once);
    }

    uint32_t bohc  = 0;
    bool     busy  = false;
    for (;;) {
        bohc = ahci_ctrl.hba_mem->bohc;
        if (ahci_boh_owned(bohc)) {
            debug_printf("[AHCI] BIOS/OS handoff: owned (BOHC=0x%08x)\n", bohc);
            return true;
        }
        if (bohc & AHCI_BOHC_BB) {
            busy = true;
            break;
        }
        if (HourGlassRunOut(&window)) break;
        cpu_pause();
    }

    if (!busy) {
        if (!(bohc & AHCI_BOHC_OOS)) {
            /* The request did not stick: the controller advertises CAP2.BOH
             * and yet BOHC.OOS does not read back as written, so there is no
             * semaphore protocol running on this board to obtain anything
             * from. Nothing is gained by waiting on it and nothing is gained
             * by refusing the disks over it — no firmware is holding them.
             * It is said, and the driver goes on. */
            kprintf("[AHCI] the controller advertises the BIOS/OS handoff but "
                    "did not take the ownership request (BOHC=0x%08x) — there "
                    "is no semaphore here to obtain\n", bohc);
            return false;
        }
        /* §10.6.3 step 5, verbatim: "If after 25 milliseconds the BIOS Busy
         * (BOHC.BB) bit has not been set to '1', then the OS can assume
         * control and perform whatever cleanup is necessary." Said out loud
         * all the same: the machine is about to be reset underneath firmware
         * that still holds its semaphore, and that is worth knowing. */
        kprintf("[AHCI] the firmware still holds this controller "
                "(BOHC=0x%08x) and did not declare itself busy within %u ms — "
                "taking control, as AHCI 1.3.1 §10.6.3 step 5 allows\n",
                bohc, AHCI_BOH_BUSY_WINDOW_MS);
        return true;
    }

    /* §10.6.3 step 4: the firmware declared itself busy, so it gets the two
     * seconds the specification calls a minimum — and what is watched is BOS,
     * because BB goes down one step before it (§10.6.4 steps 7 and 8). */
    debug_printf("[AHCI] BIOS busy (BOHC=0x%08x); granting %u ms\n",
                 bohc, AHCI_BOH_GRANT_MS);
    HourGlass grant;
    if (!HourGlassTurn(&grant, (uint64_t)AHCI_BOH_GRANT_MS * 1000ULL)) {
        kprintf("[AHCI] no time source — the firmware declared itself busy on "
                "this controller and there is no clock to measure the %u ms "
                "AHCI 1.3.1 §10.6.3 step 4 owes it (BOHC=0x%08x)\n",
                AHCI_BOH_GRANT_MS, bohc);
        return false;
    }
    for (;;) {
        bohc = ahci_ctrl.hba_mem->bohc;
        if (ahci_boh_owned(bohc)) {
            debug_printf("[AHCI] BIOS/OS handoff: owned after the grant "
                         "(BOHC=0x%08x)\n", bohc);
            return true;
        }
        if (HourGlassRunOut(&grant)) break;
        cpu_pause();
    }

    kprintf("[AHCI] the firmware declared itself busy and still owns this "
            "controller %u ms later (BOHC=0x%08x) — ownership is OOS=1 with "
            "BOS=0 (AHCI 1.3.1 §3.1.11) and it was never obtained; those two "
            "seconds are a MINIMUM the specification names no upper bound for, "
            "so what ends here is the wait, not the boot\n",
            AHCI_BOH_GRANT_MS, bohc);
    return false;
}

int ahci_init(void) {
    debug_printf("[AHCI] Initializing AHCI driver...\n");

    memset(&ahci_ctrl, 0, sizeof(ahci_controller_t));

    if (pci_find_device_by_class(0x01, 0x06, 0x01, &ahci_ctrl.pci_dev) != 0) {
        debug_printf("[AHCI] No AHCI controller found (Class 01:06:01)\n");
        return -1;
    }

    debug_printf("[AHCI] Found controller: %04x:%04x at %02x:%02x.%x\n",
                 ahci_ctrl.pci_dev.vendor_id,
                 ahci_ctrl.pci_dev.device_id,
                 ahci_ctrl.pci_dev.bus,
                 ahci_ctrl.pci_dev.device,
                 ahci_ctrl.pci_dev.function);

    /* No interrupt yet, and no guess at one. The Interrupt Line byte used to
     * be read here and kept as though it were an answer; how this controller
     * signals is decided in ahci_init_irq, where MSI is tried first and an
     * INTx line, if it comes to that, arrives as a Summons. */
    ahci_ctrl.irq_vector = 0xFF;
    ahci_ctrl.irq_enabled = false;
    ahci_ctrl.total_interrupts = 0;

    uint16_t cmd = pci_config_read_word(ahci_ctrl.pci_dev.bus,
                                        ahci_ctrl.pci_dev.device,
                                        ahci_ctrl.pci_dev.function,
                                        PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE;
    pci_config_write_word(ahci_ctrl.pci_dev.bus,
                          ahci_ctrl.pci_dev.device,
                          ahci_ctrl.pci_dev.function,
                          PCI_COMMAND,
                          cmd);

    if (pci_enable_bus_master(&ahci_ctrl.pci_dev) != 0) {
        debug_printf("[AHCI] Failed to enable bus mastering\n");
        return -1;
    }

    uint64_t bar5_full = pci_read_bar64(&ahci_ctrl.pci_dev, 5);
    if (bar5_full == 0 || bar5_full == 0xFFFFFFFF) {
        debug_printf("[AHCI] Invalid BAR5: 0x%lx\n", (unsigned long)bar5_full);
        return -1;
    }

    // Check raw BAR for I/O space bit
    uint32_t bar5_raw = pci_config_read_dword(ahci_ctrl.pci_dev.bus, ahci_ctrl.pci_dev.device,
                                               ahci_ctrl.pci_dev.function, PCI_BAR5);
    if (bar5_raw & 0x1) {
        debug_printf("[AHCI] BAR5 is I/O space (not MMIO)\n");
        return -1;
    }

    ahci_ctrl.hba_phys = bar5_full;
    debug_printf("[AHCI] BAR5: 0x%lx\n", (unsigned long)ahci_ctrl.hba_phys);

    ahci_ctrl.hba_mem = (ahci_hba_mem_t*)vmm_map_mmio(ahci_ctrl.hba_phys,
                                                       AHCI_ABAR_SPAN,
                                                       VMM_FLAGS_KERNEL_RW);
    if (!ahci_ctrl.hba_mem) {
        debug_printf("[AHCI] Failed to map BAR5 MMIO\n");
        return -1;
    }

    debug_printf("[AHCI] BAR5: phys=0x%08lx mapped to virt=0x%p (span=0x%x)\n",
                 ahci_ctrl.hba_phys, ahci_ctrl.hba_mem, (unsigned)AHCI_ABAR_SPAN);

    /* AHCI 1.3.1 §10.1.2 step 1, before any register but GHC is touched —
     * see ahci_enable_ae. What stood here instead was a CAP read with a
     * comment claiming "CAP is RO and valid before AHCI enable", which is the
     * opposite of what §5.3.2.3 says of that read, followed by a CAP2 read and
     * a BOHC read-modify-write inside the handoff, all of them before the
     * first AE write. QEMU sets CAP.SAM, so AE reads back '1' there whatever
     * software does and the whole block returns correct values on the bench;
     * on a CAP.SAM='0' HBA whose firmware left AHCI mode off, the cached CAP
     * that every later decision is made from — num_slots, s64a_support,
     * ncq_support, the CAP.SSS staggered-spin-up test — is indeterminate. */
    if (!ahci_enable_ae()) {
        kprintf("[AHCI] the controller will not enter AHCI mode: GHC.AE does "
                "not read back as '1' after %d writes (GHC=0x%08x). Every "
                "other register would answer indeterminately (AHCI 1.3.1 "
                "§5.3.2.3), so nothing is read from it and no disk is taken "
                "from this controller\n",
                AHCI_AE_WRITE_ATTEMPTS, ahci_ctrl.hba_mem->ghc);
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        return -1;
    }

    /* §10.1.2 step 4's read, now that it means something. CAP is HwInit, so
     * this value survives the HBA reset below. */
    ahci_ctrl.cap = ahci_ctrl.hba_mem->cap;

    /* CAP2 is only defined from AHCI 1.2 onwards, so on an older HBA offset
     * 24h is reserved and the version register decides whether to read it at
     * all — the same gate Linux applies in ahci_save_initial_config ("CAP2
     * register is only defined for AHCI 1.2 and later"). VS is MJR in bits
     * 31:16 and MNR in 15:0 (§3.1.5). Reserved locations read '0' by §3, so a
     * pre-1.2 HBA would answer 0 anyway; asking the version is how the driver
     * knows that rather than assumes it. */
    uint32_t vs        = ahci_ctrl.hba_mem->vs;
    uint16_t vs_major  = (uint16_t)(vs >> 16);
    uint16_t vs_minor  = (uint16_t)(vs & 0xFFFFu);
    uint32_t cap2      = (vs_major > 1 || (vs_major == 1 && vs_minor >= 0x0200u))
                         ? ahci_ctrl.hba_mem->cap2 : 0;

    /* AHCI 1.3.1 §10.6.3: ask the firmware for the HBA before driving it — and
     * SAY what the answer was. The next write is GHC.HR, and §10.4.3 makes it
     * a reset of GHC.AE, GHC.IE, the IS register and every non-HwInit port
     * register field: pulling that out from under an SMI
     * handler that still believes it owns the controller is the hazard this
     * handoff exists to avoid, and it is the lesser of the two here. The
     * greater is a machine with no disk at all — this kernel reaches its
     * volume through no other controller, §10.6.3 sets no upper bound on the
     * grant and never says to abandon the HBA, and mainline Linux resets this
     * same controller without asking anybody. So the driver proceeds, and the
     * board is told on which terms. */
    if (!ahci_bios_handoff(cap2)) {
        kprintf("[AHCI] proceeding without the firmware's handoff: the "
                "controller is reset and driven from here\n");
    }

    /* §10.1.2: "Software may perform an HBA reset prior to initializing the
     * controller by setting GHC.AE to '1' and then setting GHC.HR to '1'."
     * AE was set above, before anything else was read. */
    debug_printf("[AHCI] Performing HBA reset...\n");
    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_HR;
    mfence();

    // §10.4.3: HR self-clears when the reset completes (<= 1 second).
    uint64_t reset_deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < reset_deadline) {
        if ((ahci_ctrl.hba_mem->ghc & AHCI_GHC_HR) == 0) {
            break;
        }
        cpu_pause();
    }

    if (ahci_ctrl.hba_mem->ghc & AHCI_GHC_HR) {
        debug_printf("[AHCI] HBA reset timeout\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        return -1;
    }

    debug_printf("[AHCI] HBA reset complete\n");

    /* Re-enable AHCI mode — §10.4.3 has the reset clear GHC.AE. Same helper,
     * because a controller that needed the bit written more than once before
     * the reset needs it just as much after. */
    if (!ahci_enable_ae()) {
        kprintf("[AHCI] GHC.AE does not read back as '1' after the HBA reset "
                "(%d writes, GHC=0x%08x) — the controller is not in AHCI mode "
                "and nothing more is read from it\n",
                AHCI_AE_WRITE_ATTEMPTS, ahci_ctrl.hba_mem->ghc);
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        return -1;
    }

    debug_printf("[AHCI] AHCI mode enabled\n");

    /* Derive capabilities (CAP is static across the HBA reset). */
    ahci_ctrl.s64a_support = (ahci_ctrl.cap & AHCI_CAP_S64A) != 0;
    ahci_ctrl.ncq_support  = (ahci_ctrl.cap & AHCI_CAP_SNCQ) != 0;
    ahci_ctrl.num_slots    = ((ahci_ctrl.cap >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;

    /*
     * ‼ AND THE CONTROLLER SAYS WHO IT IS, OUT LOUD.
     *
     * Every line above that described this HBA — where it was found, what its
     * BAR5 is, its version, its CAP — was a debug_printf, so on the board the
     * whole of AHCI was four refusals and nothing else: no way to tell six
     * implemented ports from eight, no way to know whether CAP.SSS was set and
     * therefore whether PxCMD.SUD was ever written, no way to check any
     * reading of the port refusals against the controller that produced them.
     * These two lines and the ports-implemented line below are what an
     * operator needs to read the four refusals that may follow them.
     *
     * xHCI already announces itself this way on the same boot; the disk
     * controller this machine reaches its volume through did not.
     */
    kprintf("[AHCI] controller at %02x:%02x.%x  %04x:%04x — AHCI %u.%u.%u, its "
            "registers at 0x%08lx mapped for 0x%x bytes\n",
            ahci_ctrl.pci_dev.bus, ahci_ctrl.pci_dev.device,
            ahci_ctrl.pci_dev.function,
            ahci_ctrl.pci_dev.vendor_id, ahci_ctrl.pci_dev.device_id,
            (unsigned)vs_major, (unsigned)(vs_minor >> 8),
            (unsigned)(vs_minor & 0xFFu),
            (unsigned long)ahci_ctrl.hba_phys, (unsigned)AHCI_ABAR_SPAN);

    {
        /* CAP.ISS is the same three-value encoding as PxSSTS.SPD (AHCI 1.3.1
         * §3.1.1 against §3.3.10), which is what makes a port's negotiated
         * speed comparable to its controller's ceiling further down. */
        uint8_t     iss      = (uint8_t)((ahci_ctrl.cap >> AHCI_CAP_ISS_SHIFT)
                                         & AHCI_CAP_ISS_MASK);
        const char* iss_name = ahci_link_gen_name(iss);
        char        links[64];

        if (iss_name != NULL) {
            ksnprintf(links, sizeof(links), "up to %s", iss_name);
        } else {
            ksnprintf(links, sizeof(links),
                      "at a generation this driver does not name (CAP.ISS=%uh)",
                      iss);
        }

        /* Staggered spin-up is not trivia here: it is the whole of whether
         * ahci_port_init writes PxCMD.SUD at all, so a port that came up empty
         * cannot be argued about without knowing which of the two paths it
         * took (AHCI 1.3.1 §10.1.1, and §10.4.3 for what the HBA reset does
         * instead when the bit is clear). */
        const char* spin =
            (ahci_ctrl.cap & AHCI_CAP_SSS)
            ? "staggered spin-up yes, so every port is commanded to spin its "
              "drive up before its link is waited for"
            : "staggered spin-up no, so nothing here writes PxCMD.SUD and each "
              "link is started by the HBA reset itself";

        kprintf("[AHCI] it has %u command slot(s), NCQ %s, 64-bit addressing "
                "%s, drives its links %s; %s (CAP=0x%08x CAP2=0x%08x)\n",
                ahci_ctrl.num_slots,
                ahci_ctrl.ncq_support ? "yes" : "no",
                ahci_ctrl.s64a_support ? "yes" : "no",
                links, spin, ahci_ctrl.cap, cap2);
    }

    ahci_ctrl.initialized = true;

    if (ahci_init_ports() != 0) {
        kprintf("[AHCI] none of the %u implemented port(s) yielded a disk — "
                "each one's reason is above, and this controller is let go\n",
                CountSetBits(ahci_ctrl.port_implemented));
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        ahci_ctrl.initialized = false;
        return -1;
    }

    kprintf("[AHCI] %u of %u implemented port(s) in service\n",
            ahci_ctrl.num_active_ports,
            CountSetBits(ahci_ctrl.port_implemented));

    /* Self-test was unconditional and ran on every boot — fine in QEMU,
     * questionable on real hardware where a hung port would stall the
     * whole boot. Gate behind CONFIG_AHCI_SELFTEST. */
#if defined(CONFIG_AHCI_SELFTEST) && CONFIG_AHCI_SELFTEST
    ahci_test_read();
#endif

    return 0;
}

void ahci_test_read(void) {
    if (!ahci_ctrl.initialized) {
        debug_printf("[AHCI] Test READ: Not initialized\n");
        return;
    }

    ahci_port_t* port = &ahci_ctrl.ports[0];
    if (!port->active) {
        debug_printf("[AHCI] Test READ: Port 0 not active\n");
        return;
    }

    debug_printf("[AHCI] Test READ: Reading sector 0 (MBR)...\n");

    void* dma_page = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!dma_page) {
        debug_printf("[AHCI] Test READ: Failed to allocate DMA buffer\n");
        return;
    }

    uintptr_t dma_phys = (uintptr_t)dma_page;
    void* dma_virt = vmm_phys_to_virt(dma_phys);
    memset(dma_virt, 0, 4096);

    uint8_t slot = 0;
    spin_lock(&port->lock);
    port->slot_bitmap &= ~(1 << slot);
    spin_unlock(&port->lock);

    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port->clb_virt;
    cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w = 0;
    cmdheader[slot].prdtl = 1;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    cmdtbl->prdt[0].dba = (uint32_t)dma_phys;
    cmdtbl->prdt[0].dbau = (uint32_t)(dma_phys >> 32);
    cmdtbl->prdt[0].dbc = 511;  // 512 - 1 (0-based byte count)
    cmdtbl->prdt[0].i = 1;

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    memset(cmdfis, 0, sizeof(fis_reg_h2d_t));

    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_READ_DMA_EXT;

    cmdfis->lba0 = 0;
    cmdfis->lba1 = 0;
    cmdfis->lba2 = 0;
    cmdfis->lba3 = 0;
    cmdfis->lba4 = 0;
    cmdfis->lba5 = 0;

    cmdfis->device = 1 << 6;  // LBA mode

    cmdfis->countl = 1;
    cmdfis->counth = 0;

    mfence();

    port->regs->ci = 1 << slot;

    // Wait up to 5 seconds for IDENTIFY command to complete
    uint64_t id_deadline = rdtsc() + cpu_ms_to_tsc(5000);
    while (rdtsc() < id_deadline) {
        if ((port->regs->ci & (1 << slot)) == 0) {
            break;
        }
        cpu_pause();
    }

    if (port->regs->ci & (1 << slot)) {
        debug_printf("[AHCI] Test READ: Timeout waiting for command completion\n");
        /* PxCI bit `slot` is still set: the HBA has not said it is done with
         * this command, so dma_page may still be a live DMA target. Freeing
         * it here — as this used to do unconditionally — is the same
         * corruption ahci_port_identify carried at its own timeout exit: a
         * page the controller still owns handed back to pmm_alloc, to be
         * overwritten by whatever the next caller puts there. AHCI 1.3.1
         * §3.3.14 makes a successful ahci_port_stop() (PxCMD.ST 1->0, which
         * clears PxCI itself, walked out through PxCMD.CR/FR/FRE per
         * §10.3.2) the one proof that the HBA let the buffer go; a port that
         * will not stop keeps it, and the page stays held rather than freed. */
        if (ahci_port_stop(port) == 0) {
            pmm_free(dma_page, 1);
        } else {
            kprintf("[AHCI] Test READ: port %u will not stop (CMD=0x%08x) — "
                    "its DMA page is held, not freed, because the controller "
                    "may still be writing to it\n",
                    port->port_num, port->regs->cmd);
        }
        return;
    }

    uint32_t tfd = port->regs->tfd;
    if (tfd & AHCI_PTFD_STS_ERR) {
        debug_printf("[AHCI] Test READ: Error bit set in TFD (0x%08x)\n", tfd);
        pmm_free(dma_page, 1);
        return;
    }

    uint8_t* mbr = (uint8_t*)dma_virt;
    uint8_t sig1 = mbr[510];
    uint8_t sig2 = mbr[511];

    if (sig1 == 0x55 && sig2 == 0xAA) {
        debug_printf("[AHCI] Test READ: SUCCESS (MBR signature 0x55AA verified)\n");
        debug_printf("[AHCI] Test READ: First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     mbr[0], mbr[1], mbr[2], mbr[3], mbr[4], mbr[5], mbr[6], mbr[7],
                     mbr[8], mbr[9], mbr[10], mbr[11], mbr[12], mbr[13], mbr[14], mbr[15]);
    } else {
        debug_printf("[AHCI] Test READ: FAILED (expected 0x55AA, got 0x%02x%02x)\n", sig1, sig2);
    }

    spin_lock(&port->lock);
    port->slot_bitmap |= (1 << slot);
    spin_unlock(&port->lock);
    pmm_free(dma_page, 1);
}

uint8_t ahci_get_active_port_count(void) {
    return ahci_ctrl.num_active_ports;
}

uint32_t ahci_get_active_port_mask(void) {
    if (!ahci_ctrl.initialized) return 0;
    uint32_t mask = 0;
    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (ahci_ctrl.ports[i].active) {
            mask |= (1U << i);
        }
    }
    return mask;
}

bool ahci_is_initialized(void) {
    return ahci_ctrl.initialized;
}
