#include "ahci.h"
#include "klib.h"
#include "amp.h"
#include "atomics.h"
#include "baton.h"
#include "pmm.h"
#include "vmm.h"
#include "hourglass.h"
#include "boardroom.h"   /* seat:ready — the seat answers again */

/*
 * When a port's engine stops on a fault, which of the commands it was running
 * failed, and which only stood beside the one that did.
 *
 * A task-file error on an NCQ port aborts every command the device had queued,
 * and the HBA stops. What this driver used to do was fail every async command
 * in flight with ERR_IO — so one unreadable sector turned the reads and writes
 * of every other file on the disk, in flight at that moment, into errors, and
 * a write that failed that way failed the commit of whoever issued it.
 *
 * Now the fault only HOLDS them (AhciPortFault), and the recovery sorts them:
 * the culprit is answered with the error, the innocent go back to the front of
 * the roadstead and are issued again once the engine runs. Who the culprit is
 * is a fact the port or the device states, never a guess:
 *
 *   a task-file error on a port without NCQ — one command at a time, so the
 *     one in flight is it (PxCI named it);
 *   a task-file error on an NCQ port — the device's NCQ Command Error log (log
 *     address 10h) names the failed tag, read after the engine is restarted
 *     the way AHCI 1.3.1 §6.2.2.2 walks it, before any reset could clear it.
 *     A log that says the failure was a non-queued command, or a tag no async
 *     command held, clears every held command;
 *   commands the watchdog found past their deadline — those are the ones the
 *     device stopped answering.
 *
 * Where no fact names one — a host-bus or interface error, a log that will
 * not read, a command stood over that went unanswered — none of the held can
 * be proven innocent, and every one of them is failed, as before, and said.
 *
 * The recovery is run by whoever finds it owed first: the port's baton on the
 * drain core, or a caller standing over the port that found it halted
 * (ahci_roadstead.c) — so it never waits for a core that is itself waiting for
 * it. Never two at once on one port: a fault that comes while one runs is owed
 * to that runner, which goes round again.
 */

#define AHCI_FATAL_BITS (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)

/* port->recovery: nothing owed; owed and waiting for a runner; running; and
 * running with another fault come in meanwhile, which the runner owes too. */
enum {
    AHCI_RECOVERY_NONE = 0,
    AHCI_RECOVERY_OWED,
    AHCI_RECOVERY_RUNNING,
    AHCI_RECOVERY_RUNNING_OWED
};

/* Under the port's lock, with `halted` raised in the same critical section,
 * so a runner deciding under that lock whether the engine may go again sees
 * the ask or its halt together. */
static void AhciRecoveryAsk(ahci_port_t* port)
{
    uint32_t seen = __atomic_load_n(&port->recovery, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t want = seen == AHCI_RECOVERY_NONE    ? AHCI_RECOVERY_OWED :
                        seen == AHCI_RECOVERY_RUNNING ? AHCI_RECOVERY_RUNNING_OWED : seen;
        if (want == seen ||
            __atomic_compare_exchange_n(&port->recovery, &seen, want, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

bool AhciRecoveryOwed(const ahci_port_t* port)
{
    return __atomic_load_n(&port->recovery, __ATOMIC_ACQUIRE) == AHCI_RECOVERY_OWED;
}

uint32_t AhciPortFault(ahci_port_t* port, uint32_t fatal_bits, uint32_t seen_trouble,
                       uint64_t deadline_tsc)
{
    volatile ahci_port_regs_t* regs = port->regs;
    uint32_t overdue = 0;

    spin_lock(&port->lock);
    if (!port->active ||
        __atomic_load_n(&port->trouble, __ATOMIC_ACQUIRE) != seen_trouble) {
        spin_unlock(&port->lock);
        return 0;
    }
    uint32_t issued = __atomic_load_n(&port->issued_mask, __ATOMIC_ACQUIRE);
    if (deadline_tsc) {
        uint64_t now = rdtsc();
        for (uint32_t m = issued; m; m &= m - 1) {
            uint8_t s = (uint8_t)__builtin_ctz(m);
            if (now - port->submit_tsc[s] > deadline_tsc) {
                overdue |= (1U << s);
            }
        }
        if (!overdue && !fatal_bits) {
            spin_unlock(&port->lock);
            return 0;
        }
    }
    /* Raised before anything else, and before any recovery can stop the
     * engine: a command stood over reads a slot bit that cleared under a
     * changed count as one the engine may have dropped. */
    __atomic_add_fetch(&port->trouble, 1u, __ATOMIC_SEQ_CST);
    port->halted = true;
    uint32_t won = ahci_claim(port, issued);
    overdue &= won;
    port->held          |= won;
    port->fault_bits    |= fatal_bits;
    port->fault_overdue |= overdue;
    AhciRecoveryAsk(port);
    spin_unlock(&port->lock);

    __atomic_store_n(&port->stats.last_error_tsc, rdtsc(), __ATOMIC_RELAXED);
    if (fatal_bits) {
        __sync_fetch_and_add(&port->ncq_errors, 1);
        __atomic_fetch_add(&port->stats.tfes_count, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&port->stats.last_tfd,  regs->tfd,  __ATOMIC_RELAXED);
        __atomic_store_n(&port->stats.last_serr, regs->serr, __ATOMIC_RELAXED);
    }

    /* Posted only on more than one core. On one, the baton would be run by
     * the timer tick when it interrupted ring 3 — in interrupt context, where
     * a COMRESET's hundreds of milliseconds do not belong — and async I/O does
     * not exist there, so the only callers the fault can matter to are callers
     * standing over the port, which run the recovery themselves. */
    if (g_amp.total_cores > 1 &&
        __sync_bool_compare_and_swap(&port->recovering, 0, 1)) {
        BatonPass(&port->recover_node);   /* never-drop */
    }
    return overdue;
}

void AhciPortTroubled(uint8_t port_num, uint32_t seen_trouble)
{
    ahci_port_t* port = ahci_get_port_state(port_num);
    if (port) {
        (void)AhciPortFault(port, 0, seen_trouble, 0);
    }
}

/* AHCI 1.3.1 §6.2.2.2, the recovery for a task-file error: PxCMD.ST down and
 * PxCMD.CR seen clear, PxSERR and PxIS cleared, and — unless the device is
 * still BSY or DRQ, which only a reset clears — PxCMD.ST up again. No COMRESET:
 * a reset would clear the NCQ Command Error log that names the culprit. */
static bool AhciEngineRestart(ahci_port_t* port)
{
    volatile ahci_port_regs_t* regs = port->regs;

    if (ahci_port_stop(port) != 0) {
        return false;
    }
    regs->serr = 0xFFFFFFFF;
    regs->is   = 0xFFFFFFFF;
    if (regs->tfd & (AHCI_PTFD_STS_BSY | AHCI_PTFD_STS_DRQ)) {
        return false;
    }
    return ahci_port_start(port) == 0;
}

/*
 * Ask the device which queued command failed: READ LOG EXT of the NCQ Command
 * Error log, one sector, issued non-queued on `slot` — a held slot, whose
 * command table the held berth will be built again into — into a DMA page of
 * its own. Byte 0 carries NQ (bit 7: the error was a non-queued command's) and
 * the failed tag (bits 4:0); byte 511 makes the sum of all 512 bytes zero.
 *
 * 1: *tag_out names a tag an async berth held. 0: the log names no such tag —
 * a non-queued command, or one a caller stood over. -1: the log could not be
 * read, and nobody can be proven innocent.
 */
static int AhciNcqCulprit(ahci_port_t* port, uint8_t slot, uint32_t held,
                          uint8_t* tag_out)
{
    volatile ahci_port_regs_t* regs = port->regs;

    void* page = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!page) {
        return -1;
    }
    uintptr_t phys = (uintptr_t)page;
    uint8_t*  log  = (uint8_t*)vmm_phys_to_virt(phys);
    memset(log, 0, 512);

    ahci_cmd_header_t* hdr = &((ahci_cmd_header_t*)port->clb_virt)[slot];
    hdr->cfl   = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    hdr->w     = 0;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    ahci_cmd_table_t* tbl = (ahci_cmd_table_t*)port->ctba_virt[slot];
    memset(tbl, 0, sizeof(ahci_cmd_table_t));
    tbl->prdt[0].dba  = (uint32_t)phys;
    tbl->prdt[0].dbau = (uint32_t)(phys >> 32);
    tbl->prdt[0].dbc  = 512 - 1;

    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)&tbl->cfis[0];
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = ATA_CMD_READ_LOG_EXT;
    fis->device   = (1 << 6);              /* LBA */
    fis->lba0     = ATA_LOG_NCQ_ERROR;     /* the log address */
    fis->countl   = 1;                     /* one 512-byte page */

    uint32_t trouble = __atomic_load_n(&port->trouble, __ATOMIC_ACQUIRE);
    mfence();
    spin_lock(&port->lock);
    regs->ci = (1U << slot);
    spin_unlock(&port->lock);

    /* The device's own patience for one command, as a command stood over gets;
     * everything before it that ends the wait is a fact the port reports. */
    HourGlass glass;
    bool timed = HourGlassTurn(&glass, (uint64_t)CONFIG_AHCI_IO_TIMEOUT_MS * 1000ULL);
    bool done  = false;
    for (;;) {
        if ((regs->ci & (1U << slot)) == 0) {
            done = true;
            break;
        }
        if ((regs->is & AHCI_FATAL_BITS) != 0 ||
            __atomic_load_n(&port->trouble, __ATOMIC_ACQUIRE) != trouble ||
            (regs->ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PRESENT ||
            !timed || HourGlassRunOut(&glass)) {
            break;
        }
        cpu_pause();
    }

    int answer = -1;
    if (done && __atomic_load_n(&port->trouble, __ATOMIC_ACQUIRE) == trouble &&
        (regs->tfd & AHCI_PTFD_STS_ERR) == 0) {
        uint8_t sum = 0;
        for (uint32_t i = 0; i < 512; i++) {
            sum = (uint8_t)(sum + log[i]);
        }
        if (sum == 0) {
            if (log[0] & 0x80u) {
                answer = 0;
            } else {
                *tag_out = log[0] & 0x1Fu;
                answer = (held & (1U << *tag_out)) ? 1 : 0;
            }
        }
    }

    /* A command that did not finish still owns the page until the engine
     * lets go of it (AHCI 1.3.1 §3.3.14): stopped, it is freed; not stopped,
     * it is held, as ahci_port_identify holds its own. */
    if (done || ahci_port_stop(port) == 0) {
        pmm_free(page, 1);
    } else {
        kprintf("[AHCI] port %u: the error-log read did not finish and the port "
                "will not stop (CMD=0x%08x) — its DMA page is held, not freed\n",
                port->port_num, regs->cmd);
    }
    return answer;
}

static const char* AhciFaultName(uint32_t bits, uint32_t overdue)
{
    if (bits & (AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
        return "a host-bus or interface error";
    }
    if (bits & AHCI_PIS_TFES) {
        return "a task-file error";
    }
    if (overdue) {
        return "commands past their deadline";
    }
    return "a command that went unanswered";
}

/* One recovery: what the faults since the last pass left, sorted and
 * answered, and the engine running again — or the port gone. */
static void AhciRecoverPass(ahci_port_t* port)
{
    uint8_t         idx = port->port_num;
    RoadsteadBerth* berth[AHCI_MAX_SLOTS];

    /* What the fault left, taken out of the port whole: from here the held
     * berths are this pass's to answer, and a retirement meanwhile does not
     * answer them twice. */
    spin_lock(&port->lock);
    if (!port->active) {
        spin_unlock(&port->lock);
        return;
    }
    uint32_t held    = port->held;
    uint32_t bits    = port->fault_bits;
    uint32_t overdue = port->fault_overdue;
    port->held          = 0;
    port->fault_bits    = 0;
    port->fault_overdue = 0;
    for (uint32_t m = held; m; m &= m - 1) {
        uint8_t s = (uint8_t)__builtin_ctz(m);
        berth[s] = port->aboard[s];
        port->aboard[s] = NULL;
    }
    spin_unlock(&port->lock);

    bool     tfes    = (bits & AHCI_PIS_TFES) && !(bits & (AHCI_PIS_HBFS | AHCI_PIS_IFS));
    bool     reset    = true;
    uint32_t culprits = held;
    const char* named = "none of them could be proven innocent";

    if (tfes && AhciEngineRestart(port)) {
        reset = false;
        if (!held) {
            culprits = 0;
            named    = "no async command was in flight";
        } else if (!port->ncq) {
            named = "the port runs one command at a time, and it was that one";
        } else {
            uint8_t tag = 0;
            int said = AhciNcqCulprit(port, (uint8_t)__builtin_ctz(held), held, &tag);
            if (said < 0) {
                reset = true;
                named = "the device's error log would not read, so none of them "
                        "could be proven innocent";
            } else if (said == 0) {
                culprits = 0;
                named    = "the device's error log names a command that was not "
                           "one of them";
            } else {
                culprits = 1U << tag;
                named    = "the device's error log names it";
            }
        }
    } else if (overdue) {
        culprits = overdue;
        named    = "the watchdog found it past its deadline";
    }

    if (reset && ahci_port_recover(port) != OK) {
        /* The port has left the machine. Its roadstead was answered by the
         * retirement; the held berths were in flight on it, and are failed. */
        for (uint32_t m = held; m; m &= m - 1) {
            uint8_t s = (uint8_t)__builtin_ctz(m);
            berth[s]->answered(idx, s, ERR_IO, berth[s]->ctx);
        }
        return;
    }

    uint32_t innocent = held & ~culprits;
    bool     gone;
    bool     back;
    bool     waiting;

    spin_lock(&port->lock);
    gone = !port->active;
    if (!gone) {
        port->slot_bitmap |= held;
        for (int s = AHCI_MAX_SLOTS - 1; s >= 0; s--) {
            if (innocent & (1U << s)) {
                RoadsteadMoorFirst(&port->roadstead, berth[s]);
            }
        }
        /* A fault that came while this ran is owed another pass, which
         * restarts the engine it stopped; until then the port stays shut. */
        if (__atomic_load_n(&port->recovery, __ATOMIC_ACQUIRE) == AHCI_RECOVERY_RUNNING) {
            port->halted = false;
        }
    }
    back    = !gone && !port->halted;
    waiting = !gone && !RoadsteadIsEmpty(&port->roadstead);
    spin_unlock(&port->lock);

    if (gone) {
        for (uint32_t m = held; m; m &= m - 1) {
            uint8_t s = (uint8_t)__builtin_ctz(m);
            berth[s]->answered(idx, s, ERR_IO, berth[s]->ctx);
        }
        return;
    }
    for (uint32_t m = culprits; m; m &= m - 1) {
        uint8_t s = (uint8_t)__builtin_ctz(m);
        berth[s]->answered(idx, s, ERR_IO, berth[s]->ctx);
    }

    char tfd[24] = "";
    if (bits) {
        ksnprintf(tfd, sizeof(tfd), " (TFD=0x%08x)",
                  __atomic_load_n(&port->stats.last_tfd, __ATOMIC_RELAXED));
    }
    kprintf("[AHCI] port %u: back after %s%s, by %s — %u command(s) failed, %u "
            "sent again (%s)\n",
            idx, AhciFaultName(bits, overdue), tfd,
            reset ? "a link reset" : "an engine restart",
            (unsigned)CountSetBits(culprits), (unsigned)CountSetBits(innocent),
            named);

    if (back) {
        if (waiting) {
            KnockOn(&port->roadstead_knock);
        }
        BoardroomNoteSeatReady(BOARD_AHCI, idx);
    }
}

void AhciRecoverIfOwed(ahci_port_t* port)
{
    uint32_t seen = AHCI_RECOVERY_OWED;
    if (!__atomic_compare_exchange_n(&port->recovery, &seen, AHCI_RECOVERY_RUNNING,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }
    for (;;) {
        AhciRecoverPass(port);
        seen = AHCI_RECOVERY_RUNNING;
        if (__atomic_compare_exchange_n(&port->recovery, &seen, AHCI_RECOVERY_NONE,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
        /* A fault that came in while the pass ran is this runner's. Anything
         * else means the port was built again under it, and is not. */
        if (seen != AHCI_RECOVERY_RUNNING_OWED ||
            !__atomic_compare_exchange_n(&port->recovery, &seen, AHCI_RECOVERY_RUNNING,
                                         false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}
