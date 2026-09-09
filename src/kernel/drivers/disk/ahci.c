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

#define AHCI_TIMEOUT_MS CONFIG_AHCI_CMD_TIMEOUT_MS

static ahci_controller_t ahci_ctrl;

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

    // AHCI spec: 500ms for CR to clear after ST cleared
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

    // AHCI spec: 500ms for FR to clear after FRE cleared
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

    // Wait up to 500ms for CR to clear before starting
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

/* K-Core bottom-half: full port recovery (COMRESET) after a fatal error.
 * Posted to a K-Core via the port's embedded never-drop recovery node
 * (BatonPass) because COMRESET busy-waits for hundreds of ms,
 * far too long for interrupt context. The IRQ path already failed the
 * port's outstanding async slots with ERR_IO before scheduling this. */
static void ahci_deferred_recover(void* ctx) {
    ahci_port_t* port = (ahci_port_t*)ctx;
    ahci_port_recover(port);
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
        if (!state->active) continue;

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
     * is universal on AHCI controllers. Route it to the BSP's LAPIC; the
     * vector is dispatched directly by irq_handler (AHCI_MSI_VECTOR). */
    int msi = pci_msi_enable(ahci_ctrl.pci_dev.bus,
                             ahci_ctrl.pci_dev.device,
                             ahci_ctrl.pci_dev.function,
                             AHCI_MSI_VECTOR,
                             g_amp.bsp_lapic_id);
    if (msi == 0) {
        ahci_ctrl.irq_vector  = AHCI_MSI_VECTOR;
        ahci_ctrl.irq_enabled = true;
        debug_printf("[AHCI] MSI enabled (vector 0x%02x -> LAPIC %u)\n",
                     AHCI_MSI_VECTOR, g_amp.bsp_lapic_id);
        return;
    }

    /* Fallback: legacy INTx via the PCI interrupt line (an IOAPIC GSI). */
    if (ahci_ctrl.irq_vector == 0xFF || ahci_ctrl.irq_vector >= IRQ_MAX_COUNT) {
        debug_printf("[AHCI] No MSI capability and no usable INTx line; polling mode\n");
        ahci_ctrl.irq_enabled = false;
        return;
    }

    irq_register_handler(ahci_ctrl.irq_vector, ahci_irq_handler);
    irqchip_enable_irq(ahci_ctrl.irq_vector);
    ahci_ctrl.irq_enabled = true;
    debug_printf("[AHCI] INTx IRQ %u registered (MSI unavailable)\n", ahci_ctrl.irq_vector);
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
    pmm_free(buf_page, 1);
    return result;
}

static int ahci_port_init(uint8_t port_num) {
    if (port_num >= AHCI_MAX_PORTS) {
        return -1;
    }

    ahci_port_t* port = &ahci_ctrl.ports[port_num];
    memset(port, 0, sizeof(ahci_port_t));

    port->port_num = port_num;
    port->regs = ahci_get_port_regs(port_num);
    spinlock_init(&port->lock);

    /* Never-drop recovery node: COMRESET is posted through this embedded
     * node (BatonPass), so a wedged-port recovery can never be
     * dropped for want of a defer slot. */
    port->recover_node.run = ahci_deferred_recover;
    port->recover_node.ctx = port;

    port->status = AHCI_PORT_FAILED;

    volatile ahci_port_regs_t* regs = port->regs;

    if (ahci_port_stop(port) != 0) {
        kprintf("[AHCI] port %u: would not stop, so it cannot be set up — "
                "no disk on it\n", port_num);
        return -1;
    }

    // CLB (1KB) + received-FIS (256B) share one DMA32 page; both are DMA
    // targets that must sit below 4GB (PHYS_TAG_DMA32 == [0,1GB)).
    void* clb_page = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!clb_page) {
        kprintf("[AHCI] port %u: no DMA32 page for its command list — "
                "no disk on it\n", port_num);
        return -1;
    }

    uintptr_t clb_phys = (uintptr_t)clb_page;
    void* clb_virt = vmm_phys_to_virt(clb_phys);
    memset(clb_virt, 0, 4096);

    port->clb_virt = clb_virt;
    port->clb_phys = clb_phys;
    port->fis_virt = (uint8_t*)clb_virt + 1024;
    port->fis_phys = clb_phys + 1024;

    for (uint8_t slot = 0; slot < AHCI_MAX_SLOTS; slot++) {
        void* ctba_page = pmm_alloc(1, PHYS_TAG_DMA32);
        if (!ctba_page) {
            debug_printf("[AHCI] Port %u: Failed to allocate CTBA for slot %u\n", port_num, slot);
            for (uint8_t i = 0; i < slot; i++) {
                pmm_free((void*)port->ctba_phys[i], 1);
            }
            pmm_free(clb_page, 1);
            return -1;
        }

        uintptr_t ctba_phys = (uintptr_t)ctba_page;
        void* ctba_virt = vmm_phys_to_virt(ctba_phys);
        memset(ctba_virt, 0, 4096);
        port->ctba_virt[slot] = ctba_virt;
        port->ctba_phys[slot] = ctba_phys;

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

    // Clear stale interrupt/error state before bring-up.
    regs->is   = 0xFFFFFFFF;
    regs->serr = 0xFFFFFFFF;

    // Enable FIS receive so the device's initial D2H register FIS lands.
    regs->cmd |= AHCI_PCMD_FRE;

    /* Staggered spin-up (AHCI 1.3.1 §10.1.1): when CAP.SSS is set the drive
     * stays spun down until commanded. Set SUD + power-on + ICC=active.
     * Harmless on non-SSS HBAs where SUD reads as 1 / is reserved. */
    if (ahci_ctrl.cap & AHCI_CAP_SSS) {
        uint32_t cmd = regs->cmd;
        cmd |= AHCI_PCMD_SUD | AHCI_PCMD_POD;
        cmd  = (cmd & ~((uint32_t)AHCI_PCMD_ICC_MASK << AHCI_PCMD_ICC_SHIFT))
               | ((uint32_t)AHCI_PCMD_ICC_ACTIVE << AHCI_PCMD_ICC_SHIFT);
        regs->cmd = cmd;
        mfence();
    }

    // Wait for PHY communication established (PxSSTS.DET == 3), bounded 1s.
    uint8_t det = 0;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < deadline) {
        det = (regs->ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK;
        if (det == AHCI_SSTS_DET_PRESENT) {
            break;
        }
        cpu_pause();
    }
    if (det != AHCI_SSTS_DET_PRESENT) {
        debug_printf("[AHCI] Port %u: no device after spin-up (DET=%u)\n", port_num, det);
        goto fail_free;
    }

    // Clear link errors raised while establishing communication.
    regs->serr = 0xFFFFFFFF;

    // Wait for the device to leave BSY/DRQ (ready for commands), bounded 1s.
    deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < deadline) {
        if ((regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ)) == 0) {
            break;
        }
        cpu_pause();
    }
    if (regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ)) {
        debug_printf("[AHCI] Port %u: device not ready (TFD=0x%08x)\n", port_num, regs->tfd);
        goto fail_free;
    }

    // Start the command-list engine; commands may now be issued.
    regs->cmd |= AHCI_PCMD_ST;
    mfence();

    port->signature = regs->sig;

    // Only ATA disks are driven by this stack. ATAPI/PM/SEMB are skipped.
    if (port->signature != AHCI_SIG_ATA) {
        debug_printf("[AHCI] Port %u: non-ATA device (sig=0x%08x), skipping\n",
                     port_num, port->signature);
        ahci_port_stop(port);
        goto fail_free;
    }

    // Learn geometry/capabilities via IDENTIFY DEVICE.
    uint16_t id[256];
    if (ahci_port_identify(port, id) != OK) {
        debug_printf("[AHCI] Port %u: IDENTIFY DEVICE failed\n", port_num);
        ahci_port_stop(port);
        goto fail_free;
    }

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
        ahci_port_stop(port);
        goto fail_free;
    }

    /* Command slots: cap to the HBA's advertised count; for non-NCQ ports
     * allow only one outstanding command (non-queued commands may not
     * overlap on a port). slot_bitmap: 1 = free. */
    uint32_t usable = (ahci_ctrl.num_slots >= 32)
                      ? 0xFFFFFFFFu
                      : ((1u << ahci_ctrl.num_slots) - 1u);
    port->slot_bitmap = port->ncq ? usable : 0x1u;

    port->status = AHCI_PORT_ACTIVE;
    port->active = 1;

    debug_printf("[AHCI] Port %u: ATA disk, %llu sectors, sec=%uB, LBA%s, NCQ=%s (sig=0x%08x)\n",
                 port_num, (unsigned long long)port->total_sectors,
                 port->logical_sector_size, port->lba48 ? "48" : "28",
                 port->ncq ? "yes" : "no", port->signature);

    return 0;

fail_free:
    for (uint8_t i = 0; i < AHCI_MAX_SLOTS; i++) {
        if (port->ctba_phys[i]) {
            pmm_free((void*)port->ctba_phys[i], 1);
        }
    }
    pmm_free(clb_page, 1);
    return -1;
}

static int ahci_init_ports(void) {
    if (!ahci_ctrl.initialized) {
        return -1;
    }

    uint32_t pi = ahci_ctrl.hba_mem->pi;
    ahci_ctrl.port_implemented = pi;
    debug_printf("[AHCI] Ports Implemented: 0x%08x\n", pi);

    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1U << i))) continue;

        /* Fast-skip empty ports on non-SSS HBAs where DET is already
         * meaningful. On staggered-spin-up controllers the device reports
         * DET=0/1 until ahci_port_init() spins it up, so we must attempt
         * full bring-up there rather than skip. */
        if (!(ahci_ctrl.cap & AHCI_CAP_SSS)) {
            volatile ahci_port_regs_t* regs = ahci_get_port_regs(i);
            uint8_t det = (regs->ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK;
            if (det != AHCI_SSTS_DET_PRESENT) {
                debug_printf("[AHCI] Port %u: No device present (DET=%u)\n", i, det);
                continue;
            }
        }

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

    if (ahci_port_stop(port) != 0) {
        debug_printf("[AHCI] Port %u: Failed to stop engine for COMRESET\n", port->port_num);
        __atomic_fetch_add(&port->stats.comreset_fail_count, 1, __ATOMIC_RELAXED);
        return ERR_IO;
    }

    regs->serr = 0xFFFFFFFF;

    uint32_t sctl = regs->sctl;
    sctl = (sctl & ~0xF) | 0x1;
    regs->sctl = sctl;

    uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_COMRESET_WAIT);
    while (rdtsc() < timeout_tsc) {
        cpu_pause();
    }

    sctl = regs->sctl;
    sctl = (sctl & ~0xF);
    regs->sctl = sctl;

    timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_COMRESET_WAIT);
    while (rdtsc() < timeout_tsc) {
        uint32_t ssts = regs->ssts;
        uint8_t det = (ssts >> AHCI_SSTS_DET_SHIFT) & AHCI_SSTS_DET_MASK;
        if (det == AHCI_SSTS_DET_PRESENT) {
            debug_printf("[AHCI] Port %u: COMRESET successful (SSTS=0x%08x)\n", port->port_num, ssts);
            regs->serr = 0xFFFFFFFF;
            regs->is = 0xFFFFFFFF;
            if (ahci_port_start(port) == 0) {
                return OK;
            }
            break;
        }
        cpu_pause();
    }

    debug_printf("[AHCI] Port %u: COMRESET failed (timeout)\n", port->port_num);
    __atomic_fetch_add(&port->stats.comreset_fail_count, 1, __ATOMIC_RELAXED);
    return ERR_TIMEOUT;
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
            port->status = AHCI_PORT_ACTIVE;
            return OK;
        }

        debug_printf("[AHCI] Port %u: COMRESET attempt %d/%d failed\n",
                     port->port_num, attempt + 1, AHCI_MAX_COMRESET_ATTEMPTS);
    }

    debug_printf("[AHCI] Port %u: Recovery failed after %d attempts\n",
                 port->port_num, AHCI_MAX_COMRESET_ATTEMPTS);
    port->status = AHCI_PORT_ERROR;
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

/* AHCI 1.3.1 §10.6.3 — BIOS/OS handoff. On platforms that share the HBA
 * with SMM firmware (CAP2.BOH set — common on real laptops/desktops), the
 * OS must request ownership before touching the controller, otherwise BIOS
 * SMI code and the OS fight over the registers (hangs / corruption). This
 * is a true no-op on QEMU, which never advertises BOH. Requires hba_mem
 * already mapped. */
static void ahci_bios_handoff(void) {
    if (!(ahci_ctrl.hba_mem->cap2 & AHCI_CAP2_BOH)) {
        return;  // handoff unsupported — OS already owns the HBA
    }

    debug_printf("[AHCI] BIOS/OS handoff: requesting OS ownership...\n");

    // Request OS ownership (set OOS); other bits preserved.
    ahci_ctrl.hba_mem->bohc |= AHCI_BOHC_OOS;
    mfence();

    /* Poll the BIOS Owned Semaphore until firmware releases it, bounded
     * to 1s so a misbehaving BIOS cannot hang the boot. */
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(1000);
    while (rdtsc() < deadline) {
        if ((ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BOS) == 0) {
            break;
        }
        cpu_pause();
    }

    if (ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BOS) {
        debug_printf("[AHCI] WARNING: BIOS did not release HBA within 1s (continuing)\n");
    }

    /* If BIOS Busy is still set, firmware is finishing cleanup (e.g.
     * spinning drives down); spec recommends granting up to 2s more. */
    if (ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BB) {
        debug_printf("[AHCI] BIOS busy after handoff; waiting up to 2s...\n");
        deadline = rdtsc() + cpu_ms_to_tsc(2000);
        while (rdtsc() < deadline) {
            if ((ahci_ctrl.hba_mem->bohc & AHCI_BOHC_BB) == 0) {
                break;
            }
            cpu_pause();
        }
    }

    debug_printf("[AHCI] BIOS/OS handoff complete (BOHC=0x%08x)\n",
                 ahci_ctrl.hba_mem->bohc);
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

    // IRQ line is at PCI config offset 0x3C (BIOS-assigned IRQ / GSI)
    uint8_t irq_line = pci_config_read_byte(ahci_ctrl.pci_dev.bus,
                                             ahci_ctrl.pci_dev.device,
                                             ahci_ctrl.pci_dev.function,
                                             0x3C);
    ahci_ctrl.irq_vector = irq_line;
    ahci_ctrl.irq_enabled = false;
    ahci_ctrl.total_interrupts = 0;

    if (irq_line == 0xFF || irq_line >= IRQ_MAX_COUNT) {
        debug_printf("[AHCI] WARNING: Invalid IRQ line %u, IRQ disabled\n", irq_line);
        ahci_ctrl.irq_vector = 0xFF;  // Mark as invalid
    } else {
        debug_printf("[AHCI] IRQ line: %u\n", ahci_ctrl.irq_vector);
    }

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

    /* CAP is RO and valid before AHCI enable; cache it. CAP2 carries the
     * BOH bit and must be read before the handoff below. */
    ahci_ctrl.cap = ahci_ctrl.hba_mem->cap;

    /* AHCI 1.3.1 §10.6.3: take the HBA away from BIOS before driving it. */
    ahci_bios_handoff();

    /* §5.3.2.3: GHC.AE must be set before accessing other AHCI registers.
     * Set it, issue the HBA reset (which clears AE), then set it again —
     * the spec-mandated ordering for a clean reset. */
    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_AE;
    mfence();

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

    // Re-enable AHCI mode — the reset cleared GHC.AE.
    ahci_ctrl.hba_mem->ghc |= AHCI_GHC_AE;
    mfence();

    if ((ahci_ctrl.hba_mem->ghc & AHCI_GHC_AE) == 0) {
        debug_printf("[AHCI] Failed to enable AHCI mode\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        return -1;
    }

    debug_printf("[AHCI] AHCI mode enabled\n");

    /* Derive capabilities (CAP is static across the HBA reset). */
    ahci_ctrl.s64a_support = (ahci_ctrl.cap & AHCI_CAP_S64A) != 0;
    ahci_ctrl.ncq_support  = (ahci_ctrl.cap & AHCI_CAP_SNCQ) != 0;
    ahci_ctrl.num_slots    = ((ahci_ctrl.cap >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;

    debug_printf("[AHCI] CAP: NCQ=%s, Slots=%u, S64A=%s, CAP2=0x%08x\n",
                 ahci_ctrl.ncq_support ? "yes" : "no",
                 ahci_ctrl.num_slots,
                 ahci_ctrl.s64a_support ? "yes" : "no",
                 ahci_ctrl.hba_mem->cap2);

    ahci_ctrl.initialized = true;

    if (ahci_init_ports() != 0) {
        debug_printf("[AHCI] No active ports found\n");
        vmm_unmap_mmio(ahci_ctrl.hba_mem, AHCI_ABAR_SPAN);
        ahci_ctrl.initialized = false;
        return -1;
    }

    debug_printf("[AHCI] Initialization complete (%u active port(s))\n", ahci_ctrl.num_active_ports);

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
        pmm_free(dma_page, 1);
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
