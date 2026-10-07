/* ===========================================================================
 *  ata_async.c — IRQ-driven Bus-Master IDE DMA engine.
 *
 *  Replaces the polled completion loop with a submit/IRQ pipeline:
 *
 *    submit (the caller)
 *      ├── the AtaCmd on the caller's own stack, the args in it
 *      ├── lock channel, link onto in_flight (or queue tail), unlock
 *      └── if just installed → bmide_kick() programs PRD + LBA + SSBM
 *
 *    INTRQ from drive → IDT dispatcher → ata_irq_handler
 *      ├── for each channel: if BMISR.IRQ set →
 *      │     read STATUS (clears drive-side INTRQ per ATA-7 §6.2.5)
 *      │     classify status (BMISR.ERROR / STATUS.ERR → ERR_IO)
 *      │     reset BMIDE engine, W1C BMISR.ERROR|IRQ
 *      │     stamp the channel's `landing` slot with the completed cmd
 *      └── EOI handled by IDT dispatcher
 *
 *    the waiting caller (ata_dma_sync) looks at BMISR.IRQ itself too
 *      └── BmideAnswerHere: the same retire, under the same cmd_lock, so the
 *          answer never waits for the core the interrupt is routed to
 *
 *    the waiting caller (ata_dma_sync) claims `landing` → ata_run_completion
 *      ├── if read & OK: memcpy DMA staging → user_buf
 *      ├── lock channel, pop next wait_head → in_flight; unlock; bmide_kick
 *      ├── on error: say it on "storage:ata:error" through the Touch IRQ ring
 *      └── publish done — the caller owns the stack cmd
 *
 *  Single-channel cap of one in-flight cmd matches PATA hardware: master
 *  and slave share the bus and only one ATA command may be outstanding
 *  at a time. Submits past that point queue up; the bottom-half pops the
 *  next one when the current retires. Every command has a waiting caller:
 *  there is no fire-and-forget path here, because nothing in the kernel
 *  asked for one, and a path nobody rides is a path nobody proves. The
 *  channel's other commands — PIO transfers, FLUSH CACHE, a polled DMA — take
 *  the channel whole (AtaChannelTake) between two of the engine's, never
 *  beside one.
 *
 *  Pre-multicore boot path: the IRQ-driven path is taken only multi-core, as
 *  it — with its watchdog and SRST recovery — was built and proven; before
 *  that, and on one core, ata_dma_sync transparently drives a polled BMIDE
 *  engine (tagfs mount, fsck). The polled path keeps nIEN=1 so it never
 *  collides with the IRQ-driven path that is enabled later.
 *
 *  References:
 *    Intel BMIDE Spec Rev 1.0 §3 (register layout, completion semantics)
 *    ATA-7 / T13-1532D §6.2.5 (INTRQ clear on STATUS read), §7.41 (SET FEATURES)
 *    Intel SDM Vol 3 §11 (memory ordering for bus-master DMA on x86)
 * =========================================================================*/

#include "ata.h"
#include "ata_async.h"
#include "klib.h"
#include "io.h"
#include "cpu_calibrate.h"
#include "kernel_config.h"
#include "atomics.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "boxos_memory.h"
#include "ahci.h"
#include "amp.h"
#include "logbook.h"     /* TouchLogbookResolve — the error report's name, once */
#include "idt.h"
#include "irqchip.h"
#include "touch.h"
#include "scheduler.h"     /* SchedulerHoldHere — a recovery is run to its end */
#include "system_halt.h"   /* HaltStopIfOwedHere — and stopped only after it */
#include "boardroom.h"     /* BoardroomNoteSeatReady — the channel answers again */

/* ---------------------------------------------------------------------
 *  BMIDE register layout (Intel BMIDE Rev 1.0 §3).
 *  Mirrors the constants formerly in ata.c — now owned here since the
 *  async engine drives every BMIDE access on real-HW boots.
 * ------------------------------------------------------------------ */
#define BMIDE_REG_CMD       0   /* BMICR */
#define BMIDE_REG_STATUS    2   /* BMISR */
#define BMIDE_REG_PRD       4   /* BMIDT — 32-bit physical PRD pointer */

#define BMICR_START         0x01    /* SSBM */
#define BMICR_READ_FROM_DISK 0x08   /* RWCON: 1 = engine WRITES memory */

#define BMISR_ACTIVE        0x01
#define BMISR_ERROR         0x02    /* W1C */
#define BMISR_IRQ           0x04    /* W1C */

#define ATA_PRD_EOT         0x8000

#define ATA_DMA_MAX_SECTORS ATA_ASYNC_MAX_SECTORS
#define ATA_CMD_QUEUE_CAP   128     /* per-channel back-pressure */

/* ATA register offsets within a channel command block. Duplicated from
 * ata.c (private there) — kept local for self-contained register access. */
#define ATA_REG_DATA            0
#define ATA_REG_ERROR           1
#define ATA_REG_FEATURES        1
#define ATA_REG_SECCOUNT        2
#define ATA_REG_LBA_LO          3
#define ATA_REG_LBA_MID         4
#define ATA_REG_LBA_HI          5
#define ATA_REG_DRIVE_HEAD      6
#define ATA_REG_STATUS          7
#define ATA_REG_COMMAND         7
#define ATA_CTRL_SRST           0x04
#define ATA_CTRL_nIEN           0x02

/* ---------------------------------------------------------------------
 *  Per-channel async state
 * ------------------------------------------------------------------ */
typedef struct AtaCmd {
    uint8_t        drive_idx;
    uint8_t        channel;
    uint16_t       count;
    uint64_t       lba;
    bool           is_write;
    void          *user_buf;     /* kernel-mapped src (write) or dst (read) */

    volatile uint32_t done;      /* 0 = in-flight, 1 = completed */
    volatile error_t  sync_rc;
    uint64_t       submit_tsc;

    /* Set to 1 (under cmd_lock) by whoever classifies this cmd's completion —
     * the IRQ handler, a waiter's own look (BmideAnswerHere) OR the watchdog's
     * TIER-1 reconcile. It is the arbiter that keeps the periodic watchdog
     * scan from mis-judging a just-completed cmd (in_flight still points at it
     * until its bottom-half runs) as a fresh wedge: the scan only ever acts on
     * an UN-reconciled in_flight cmd. */
    volatile uint8_t  reconciled;

    struct AtaCmd *next;
} AtaCmd;

typedef struct AtaAsyncCh {
    bool        enabled;
    uint16_t    bmide_base;
    uint8_t    *prd_virt;
    uintptr_t   prd_phys;
    void       *buf_virt;
    uintptr_t   buf_phys;

    spinlock_t  cmd_lock;
    AtaCmd     *in_flight;
    AtaCmd     *wait_head;
    AtaCmd     *wait_tail;
    uint32_t    queue_depth;

    volatile uint64_t cmds_submitted;
    volatile uint64_t cmds_completed;
    volatile uint64_t cmds_failed;
    /* Interrupts that found nothing to retire — including the edge of every
     * command its waiter retired by hand first (BmideAnswerHere), which still
     * reaches the BSP afterwards. Telemetry, never a verdict. */
    volatile uint64_t spurious_irqs;

    /* Ф26 landing — per-channel single-slot NEVER-DROP completion mailbox.
     * The IRQ stamps the completed cmd here (RELEASE); the waiting caller
     * claims it (ACQ_REL XCHG) and runs the bottom-half itself, so a
     * completion can never be dropped for want of a slot. ≤1 in-flight per
     * channel ⇒ one slot suffices. */
    AtaCmd   *volatile landing;
    volatile uint64_t  landing_clobber;   /* invariant tripwire — must stay 0 */
    volatile uint8_t   clobber_warned;    /* one-shot: loud print on first clobber */

    /* Ф26 BMIDE watchdog. A genuine wedge — seen by the PIT tick's scan, or by
     * a waiter whose command outlived CONFIG_ATA_LIVENESS_MS — moves
     * `recovering` from NONE to OWED under cmd_lock, one detection standing
     * for every repeat of it. The reset busy-waits BSY for up to
     * ATA_SRST_BSY_HELD_MS, far too long for the tick's IRQ context, so the
     * tick only marks it owed: whichever waiter on the channel next finds it
     * owed takes it OWED->RUNNING and runs ata_recover_worker itself
     * (BmideRecoverIfOwed). Every command in flight or queued has a waiter
     * spinning on it, so the reset never waits on a core that is not there —
     * a drain core that never pumps, a BSP with its interrupts off. While it
     * is not NONE the IRQ handler, the scan and submit all stand off. */
    uint8_t            ch_idx;
    volatile uint32_t  recovering;

    /* The channel taken whole by a caller of AtaChannelTake, under cmd_lock:
     * nothing of the engine's is kicked while it is set — a submit queues, a
     * completion and a recovery leave the queue where it is — and the Give
     * kicks the head. */
    bool               claimed;
} AtaAsyncCh;

#define BMIDE_RECOVERY_NONE     0u
#define BMIDE_RECOVERY_OWED     1u   /* a wedge was seen; the next waiter to look runs the reset */
#define BMIDE_RECOVERY_RUNNING  2u

static AtaAsyncCh g_ata_async[ATA_CHANNEL_COUNT];
static volatile uint8_t g_ata_async_ready = 0;     /* 1 once init completed */
static volatile uint8_t g_ata_irq_armed   = 0;     /* 1 once IRQ wired */

/* PRD table backing — one page per channel, page-aligned so the
 * "no cross 64KB" + "4-byte aligned" rules from Intel BMIDE §4.2 are
 * satisfied unconditionally. Only the first 8 bytes hold a PRD entry. */
static __attribute__((aligned(4096), section(".bss")))
    uint8_t g_prd_storage[ATA_CHANNEL_COUNT][4096];

/* ---------------------------------------------------------------------
 *  Local helpers — channel/drive arithmetic + register I/O
 * ------------------------------------------------------------------ */
static inline uint8_t drive_channel(uint8_t drive_idx) { return drive_idx >> 1; }
static inline uint8_t drive_is_slave(uint8_t drive_idx) { return drive_idx & 1; }
static inline AtaChannel *drive_channel_ptr(uint8_t drive_idx) {
    return &g_ata_channels[drive_channel(drive_idx)];
}

/* ATA8-ACS §6.2.1's 400 ns after a drive-select — the idiom and the reason
 * four non-posted reads satisfy it live once, in ata.h. */
static inline void ata_delay_400ns(uint8_t drive_idx) {
    ata_settle_400ns(drive_channel_ptr(drive_idx)->ctrl_reg);
}

static inline uint8_t ata_read_status(uint8_t drive_idx) {
    return inb(drive_channel_ptr(drive_idx)->cmd_base + ATA_REG_STATUS);
}

static inline void ata_select_drive(uint8_t drive_idx) {
    AtaChannel *ch = drive_channel_ptr(drive_idx);
    uint8_t head_byte = drive_is_slave(drive_idx) ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER;
    outb(ch->cmd_base + ATA_REG_DRIVE_HEAD, head_byte);
    ata_delay_400ns(drive_idx);
}

static int ata_wait_ready(uint8_t drive_idx) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_TIMEOUT_MS);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRDY)) return 0;
    }
    return ATA_ERR_TIMEOUT;
}

/* Use the channel's Device Control register to flip the IRQ mask. For
 * the IRQ-driven path nIEN is cleared once at init and stays clear so
 * the drive raises INTRQ on every command completion. The polled
 * fallback re-asserts nIEN locally to keep its rdtsc-poll loop the only
 * observer of "transfer complete". */
static void ata_set_nien(uint8_t channel, bool masked) {
    AtaChannel *ch = &g_ata_channels[channel];
    if (!ch->present) return;
    outb(ch->ctrl_reg, masked ? ATA_CTRL_nIEN : 0);
    ata_settle_400ns(ch->ctrl_reg);
}

/* SET FEATURES "set transfer mode" — shared with ata.c PIO negotiation
 * (forward-declared in ata.h). Polled, init-only path. */
extern int ata_set_xfer_mode(uint8_t drive_idx, uint8_t mode_byte);

/* Forward decl — defined in ata.c, used by both DMA-issue paths. */
extern int ata_program_lba(uint8_t drive_idx, uint64_t lba, uint16_t count,
                           uint8_t cmd28, uint8_t cmd48);

/* ---------------------------------------------------------------------
 *  BMIDE engine programming
 * ------------------------------------------------------------------ */
static void bmide_engine_stop(AtaAsyncCh *aa) {
    outb(aa->bmide_base + BMIDE_REG_CMD,    0);
    outb(aa->bmide_base + BMIDE_REG_STATUS, BMISR_ERROR | BMISR_IRQ);
}

/* Build PRD + program engine + select drive + issue DMA command. Caller
 * MUST hold aa->cmd_lock — IRQs are masked locally (irqsave spinlock)
 * so the BMISR / BMICR writes here can't interleave with an IRQ on the
 * same CPU. Drive is assumed DRDY:
 *   - first cmd after ata_async_init: drive ended IDENTIFY in DRDY state
 *   - subsequent cmds: prior IRQ-deferred completion just ack'd the drive
 *     (which leaves it idle / DRDY) before popping next from queue;
 *   - the first after a caller gave the channel back (AtaChannelGive): that
 *     caller waited for its own command to end before it gave it.
 *
 * No ata_wait_ready spin here — that would busy-poll inside cmd_lock with
 * IRQs off for up to CONFIG_ATA_TIMEOUT_MS. What makes it safe is that
 * nothing else ever writes the channel's command block while the engine
 * owns it: every command that is not the engine's takes the channel whole
 * first (AtaChannelTake), which waits for the engine to be idle and keeps
 * it idle until the Give. */
static void bmide_kick(AtaAsyncCh *aa, AtaCmd *cmd) {
    uint32_t total_bytes = (uint32_t)cmd->count * ATA_SECTOR_SIZE;

    if (cmd->is_write) {
        memcpy(aa->buf_virt, cmd->user_buf, total_bytes);
        mfence();
    }

    *(uint32_t *)(aa->prd_virt + 0) = (uint32_t)aa->buf_phys;
    *(uint16_t *)(aa->prd_virt + 4) = (uint16_t)total_bytes;
    *(uint16_t *)(aa->prd_virt + 6) = ATA_PRD_EOT;
    mfence();

    bmide_engine_stop(aa);                          /* clean slate */
    outl(aa->bmide_base + BMIDE_REG_PRD, (uint32_t)aa->prd_phys);

    uint8_t dir = cmd->is_write ? 0 : BMICR_READ_FROM_DISK;
    outb(aa->bmide_base + BMIDE_REG_CMD, dir);

    ata_select_drive(cmd->drive_idx);
    ata_program_lba(cmd->drive_idx, cmd->lba, cmd->count,
                    cmd->is_write ? ATA_CMD_WRITE_DMA     : ATA_CMD_READ_DMA,
                    cmd->is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT);

    outb(aa->bmide_base + BMIDE_REG_CMD, dir | BMICR_START);
    cmd->submit_tsc = rdtsc();
}

/* Polled fallback. Used before the IRQ-driven path is viable (early init /
 * single-core boots). Local nIEN=1 keeps the IRQ silent so we don't
 * confuse the K-Core-less startup. After completion we always restore
 * nIEN=0 because that is the steady-state ata_async_init established
 * once and the IRQ-driven path needs it to take over later.
 *
 * NOT a "save/restore" pattern: reading ctrl_reg on legacy IDE returns
 * the Alternate Status register (BSY/DRDY/IDX), NOT the previously-
 * written Device Control byte — so a true "prior nIEN" read is not
 * possible. The steady-state contract carried by ata_async_init makes
 * an unconditional restore correct. */
static int bmide_transfer_polled(AtaCmd *cmd) {
    uint8_t      ch_idx = cmd->channel;
    AtaAsyncCh  *aa     = &g_ata_async[ch_idx];

    ata_set_nien(ch_idx, true);

    uint32_t total_bytes = (uint32_t)cmd->count * ATA_SECTOR_SIZE;
    if (cmd->is_write) {
        memcpy(aa->buf_virt, cmd->user_buf, total_bytes);
        mfence();
    }

    *(uint32_t *)(aa->prd_virt + 0) = (uint32_t)aa->buf_phys;
    *(uint16_t *)(aa->prd_virt + 4) = (uint16_t)total_bytes;
    *(uint16_t *)(aa->prd_virt + 6) = ATA_PRD_EOT;
    mfence();

    bmide_engine_stop(aa);
    outl(aa->bmide_base + BMIDE_REG_PRD, (uint32_t)aa->prd_phys);
    uint8_t dir = cmd->is_write ? 0 : BMICR_READ_FROM_DISK;
    outb(aa->bmide_base + BMIDE_REG_CMD, dir);

    ata_select_drive(cmd->drive_idx);
    if (ata_wait_ready(cmd->drive_idx) != 0) {
        bmide_engine_stop(aa);
        ata_set_nien(ch_idx, false);
        return ATA_ERR_TIMEOUT;
    }

    ata_program_lba(cmd->drive_idx, cmd->lba, cmd->count,
                    cmd->is_write ? ATA_CMD_WRITE_DMA     : ATA_CMD_READ_DMA,
                    cmd->is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT);

    outb(aa->bmide_base + BMIDE_REG_CMD, dir | BMICR_START);

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_TIMEOUT_MS);
    int rc = ATA_ERR_TIMEOUT;
    while (rdtsc() < deadline) {
        uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);
        if (bmsr & BMISR_ERROR) { rc = ATA_ERR_DRIVE_FAULT; break; }
        uint8_t st = ata_read_status(cmd->drive_idx);
        if (st == 0xFF) { rc = ATA_ERR_NO_DEVICE; break; }
        if (st & ATA_SR_ERR) { rc = ATA_ERR_DRIVE_FAULT; break; }
        if (!(bmsr & BMISR_ACTIVE) && !(st & ATA_SR_BSY) && !(st & ATA_SR_DRQ)) {
            rc = 0;
            break;
        }
        cpu_pause();
    }

    bmide_engine_stop(aa);
    mfence();

    if (rc == 0 && !cmd->is_write) {
        memcpy(cmd->user_buf, aa->buf_virt, total_bytes);
    }

    ata_set_nien(ch_idx, false);
    return rc;
}

/* ---------------------------------------------------------------------
 *  Per-channel queue management
 * ------------------------------------------------------------------ */

/* Caller holds aa->cmd_lock. Append cmd to wait tail. */
static void queue_push_tail(AtaAsyncCh *aa, AtaCmd *cmd) {
    cmd->next = NULL;
    if (!aa->wait_head) {
        aa->wait_head = cmd;
        aa->wait_tail = cmd;
    } else {
        aa->wait_tail->next = cmd;
        aa->wait_tail = cmd;
    }
    aa->queue_depth++;
}

/* Caller holds aa->cmd_lock. Pop head; NULL if empty. */
static AtaCmd *queue_pop_head(AtaAsyncCh *aa) {
    AtaCmd *c = aa->wait_head;
    if (!c) return NULL;
    aa->wait_head = c->next;
    if (!aa->wait_head) aa->wait_tail = NULL;
    c->next = NULL;
    aa->queue_depth--;
    return c;
}

/* The engine's next command, off the queue and onto the drive — unless a
 * caller has taken the channel whole, which then has it first and kicks the
 * head itself when it gives the channel back. Caller holds aa->cmd_lock. */
static void bmide_launch_next_locked(AtaAsyncCh *aa) {
    AtaCmd *next = aa->claimed ? NULL : queue_pop_head(aa);
    aa->in_flight = next;
    if (next) bmide_kick(aa, next);
}

/* ---------------------------------------------------------------------
 *  Bottom half — runs on whoever claims the landing: a waiting caller, or
 *  a caller taking the channel whole
 * ------------------------------------------------------------------ */

/* The error report's name, resolved once at init (Touch is up by then) and
 * cached: the report is published where the completion runs, and that may
 * be a caller holding a TagFS write lock — a context that can claim a slot
 * in the Touch IRQ ring and knock, but must not resolve a name or take a
 * bucket lock. */
static volatile TouchTag g_ata_err_full = TOUCH_TAG_INVALID;
static volatile TouchTag g_ata_err_bare = TOUCH_TAG_INVALID;

static void touch_publish_error(uint8_t drive_idx, error_t status,
                                uint64_t lba, uint16_t count, bool is_write) {
    struct __attribute__((packed)) {
        uint8_t  drive_idx;
        uint8_t  is_write;
        uint16_t count;
        uint32_t status;
        uint64_t lba;
        uint64_t tsc;
    } ev = {
        .drive_idx = drive_idx,
        .is_write  = is_write ? 1u : 0u,
        .count     = count,
        .status    = (uint32_t)status,
        .lba       = lba,
        .tsc       = rdtsc(),
    };
    TouchPublishIrqPair(__atomic_load_n(&g_ata_err_full, __ATOMIC_ACQUIRE),
                        __atomic_load_n(&g_ata_err_bare, __ATOMIC_ACQUIRE),
                        &ev, sizeof(ev), 0, TOUCH_FLAG_KERNEL);
}

/* Shared completion bottom-half, run by whoever claimed the landing
 * (BmideTakeLanding), with interrupts off from the claim to the publish.
 * Straight-line, never blocks: memcpy-out, kick the next queued cmd, say an
 * error, then publish done so the waiter returns. The error is said through
 * the Touch IRQ ring precisely because this may run under a TagFS write lock:
 * the ring's claim and knock take no lock, and the K-Core that reads the ring
 * publishes where subscriber locks are order-safe. */
static void ata_run_completion(AtaAsyncCh *aa, AtaCmd *cmd) {
    /* Read path: copy DMA staging into the caller's buffer BEFORE the next
     * kick reuses the shared staging page. */
    if (!cmd->is_write && cmd->sync_rc == OK && cmd->user_buf) {
        memcpy(cmd->user_buf, aa->buf_virt, (uint32_t)cmd->count * ATA_SECTOR_SIZE);
    }

    /* The next queued cmd onto the engine. cmd_lock serialises the BMIDE
     * register writes against a fast next-completion IRQ. */
    spin_lock(&aa->cmd_lock);
    bmide_launch_next_locked(aa);
    spin_unlock(&aa->cmd_lock);

    if (cmd->sync_rc != OK) {
        atomic_fetch_add_u64(&aa->cmds_failed, 1);
        touch_publish_error(cmd->drive_idx, cmd->sync_rc, cmd->lba,
                            cmd->count, cmd->is_write);
    }
    atomic_fetch_add_u64(&aa->cmds_completed, 1);

    /* The caller owns the stack cmd. Publish done; the waiter returns. */
    __atomic_store_n(&cmd->done, 1u, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------
 *  Shared completion body — classify + retire the in-flight cmd.
 *
 *  Caller holds cmd_lock and has already established, under that lock, that
 *  aa->in_flight is non-NULL, un-reconciled, and its INTRQ is asserted
 *  (`bmsr` is that same snapshot, IRQ bit set). Invoked identically by the
 *  raw IRQ handler, by the waiter's own look (BmideAnswerHere) and by the
 *  watchdog's TIER-1 lost-INTRQ reconcile, so a completion is retired by
 *  exactly one of them (whoever W1Cs the engine first clears BMISR.IRQ; the
 *  others then read it clear and no-op).
 *
 *  It stamps the never-drop `landing` slot; the waiting caller claims it.
 * ------------------------------------------------------------------ */
static void bmide_complete_locked(AtaAsyncCh *aa, uint8_t bmsr) {
    AtaCmd *cmd = aa->in_flight;

    /* ATA-7 §6.2.5: reading STATUS clears the drive-side IRQ latch. */
    uint8_t st = ata_read_status(cmd->drive_idx);

    error_t rc = OK;
    if (st == 0xFF)                          rc = ERR_DEVICE_NOT_READY;
    else if (st & ATA_SR_ERR)                rc = ERR_IO;
    else if (st & ATA_SR_DF)                 rc = ERR_IO;
    else if (bmsr & BMISR_ERROR)             rc = ERR_IO;

    /* W1C the BMIDE latches and stop the engine (also clears BMICR.Start,
     * the manual-clear some controllers need for BMISR.Active) — required
     * before the next kick can re-program BMICR/BMIDT per Intel BMIDE §3.1. */
    bmide_engine_stop(aa);
    mfence();

    cmd->sync_rc = rc;
    __atomic_store_n(&cmd->reconciled, 1u, __ATOMIC_RELAXED);

    /* Stamp the per-channel landing slot — the waiting caller claims it
     * (XCHG) and runs the bottom-half itself, so this can NEVER be dropped.
     * The ≤1-in-flight invariant guarantees landing is NULL here (the prior
     * completion was claimed before its next cmd could be kicked); a non-NULL
     * means the invariant broke upstream — count it, never overwrite (that
     * would strand a live waiter). RELEASE publishes sync_rc to the claimer's
     * ACQUIRE. in_flight stays set: the read-staging page is copied out in
     * ata_run_completion before the next kick. */
    if (__atomic_load_n(&aa->landing, __ATOMIC_RELAXED) != NULL)
        atomic_fetch_add_u64(&aa->landing_clobber, 1);
    else
        __atomic_store_n(&aa->landing, cmd, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------
 *  IRQ handler — checks each channel for BMIDE-asserted completion
 * ------------------------------------------------------------------ */
static void ata_channel_irq_process(uint8_t ch_idx) {
    AtaAsyncCh *aa = &g_ata_async[ch_idx];
    if (!aa->enabled) return;

    /* Take cmd_lock up front — register reads/writes are serialised
     * against submit/complete on other cores. Same-core preemption is
     * impossible because the lock is irqsave (this IRQ couldn't have
     * fired while the lock was held locally). */
    spin_lock(&aa->cmd_lock);

    /* A recovery of this channel is owed or running: the waiter that runs it
     * looks at the latch first itself, BMISR reads are meaningless mid-reset,
     * and the drive's INTRQ is masked (nIEN=1) through it, so any edge that
     * reaches here is stale. Stand off entirely. */
    if (__atomic_load_n(&aa->recovering, __ATOMIC_RELAXED)) {
        spin_unlock(&aa->cmd_lock);
        return;
    }

    uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);
    if (!(bmsr & BMISR_IRQ)) {
        atomic_fetch_add_u64(&aa->spurious_irqs, 1);
        spin_unlock(&aa->cmd_lock);
        return;
    }

    AtaCmd *cmd = aa->in_flight;
    if (!cmd || __atomic_load_n(&cmd->reconciled, __ATOMIC_RELAXED)) {
        /* IRQ but nothing outstanding to retire (idle channel, or the
         * watchdog or the cmd's waiter already reconciled it and its
         * bottom-half has yet to move in_flight along) — clear drive latch
         * + BMISR + stop engine, then drop the line. */
        if (g_ata_channels[ch_idx].present) {
            (void)inb(g_ata_channels[ch_idx].cmd_base + ATA_REG_STATUS);
        }
        bmide_engine_stop(aa);
        atomic_fetch_add_u64(&aa->spurious_irqs, 1);
        spin_unlock(&aa->cmd_lock);
        return;
    }

    bmide_complete_locked(aa, bmsr);
    spin_unlock(&aa->cmd_lock);
}

static void ata_irq_handler(void) {
    if (!__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return;
    for (uint8_t ch = 0; ch < ATA_CHANNEL_COUNT; ch++) {
        ata_channel_irq_process(ch);
    }
}

/* ---------------------------------------------------------------------
 *  Recovery — the SRST of a genuinely wedged channel.
 *
 *  Run by the first waiter on the channel to find it owed (BmideRecoverIfOwed)
 *  — the wedged command's own waiter, one queued behind it, or a caller taking
 *  the channel whole — with `recovering` already RUNNING, which keeps the IRQ
 *  handler, the scan and submit off the channel for the duration. An
 *  ATA_SRST_BSY_HELD_MS wait on BSY is affordable there, and the spec's full
 *  31 s would NOT be: the waiter may hold a TagFS lock, and every other command
 *  on the channel waits behind the reset.
 *
 *  Stack-cmd lifetime is the delicate part. The wedged sync cmd lives on its
 *  waiter's stack; the waiter spins until it observes cmd->done. If any other
 *  actor (a late IRQ, this worker) could still touch the cmd after the waiter
 *  returns, that is a use-after-free. The 2026-05-28 audit note flagged exactly
 *  this and proposed heap+refcount for every I/O; we avoid that hot-path cost
 *  with strict ORDERING instead: SRST silences the drive (nIEN held =1) so no
 *  late INTRQ can reference the cmd, we NULL in_flight and re-arm the channel,
 *  and only THEN — last of all — publish the wedged cmd's failure. The instant
 *  its waiter observes done, no live reference to the stack frame survives —
 *  and when this worker runs on that very waiter, it observes it on its next
 *  turn.
 * ------------------------------------------------------------------ */
static void ata_recover_worker(AtaAsyncCh *aa) {
    uint8_t ch_idx = aa->ch_idx;

    /* Belt-and-suspenders: the drive may have completed between the wedge
     * being seen and this worker running. If its INTRQ is now latched,
     * reconcile normally and skip the reset — no spurious ERR_IO. */
    spin_lock(&aa->cmd_lock);
    uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);
    AtaCmd *cmd  = aa->in_flight;
    if (cmd && !__atomic_load_n(&cmd->reconciled, __ATOMIC_RELAXED) &&
        (bmsr & BMISR_IRQ)) {
        bmide_complete_locked(aa, bmsr);
        __atomic_store_n(&aa->recovering, BMIDE_RECOVERY_NONE, __ATOMIC_RELEASE);
        spin_unlock(&aa->cmd_lock);
        return;
    }
    spin_unlock(&aa->cmd_lock);

    /* Genuine wedge. SRST with nIEN held =1 throughout silences the drive's
     * INTRQ, so the wedged command can raise no further interrupt. */
    ata_channel_soft_reset(ch_idx, ATA_SRST_BSY_HELD_MS);

    /* Retire the wedged cmd out of flight and re-launch the innocent queue
     * (the "re-launch waiting ships" recovery: commands queued behind the
     * wedged one were never issued to the drive, so a reset makes them
     * runnable again — failing them would spuriously punish untouched I/O).
     * All under one lock hold so a concurrent submit either lands in the queue
     * we re-kick from or is serialised behind recovering being cleared. A
     * caller that took the channel meanwhile has it next instead. */
    spin_lock(&aa->cmd_lock);
    AtaCmd *wedged = aa->in_flight;
    __atomic_store_n(&aa->landing, NULL, __ATOMIC_RELAXED); /* no completion landed */
    ata_set_nien(ch_idx, false);            /* SRST left nIEN=1; re-arm the IRQ path */
    bmide_launch_next_locked(aa);
    __atomic_store_n(&aa->recovering, BMIDE_RECOVERY_NONE, __ATOMIC_RELEASE);
    spin_unlock(&aa->cmd_lock);

    /* The channel answers again, for whichever of its two drives has a seat
     * — the bell a refusal on it waits for (boardroom.h). */
    for (uint8_t d = 0; d < 2; d++) BoardroomNoteSeatReady(BOARD_ATA, (uint8_t)((ch_idx << 1) | d));

    /* Publish the wedged cmd's failure LAST — in_flight no longer points at it
     * and the drive is silenced+re-armed, so its (stack) storage is dead to
     * every other actor the instant its waiter observes done. */
    if (wedged) {
        wedged->sync_rc = ERR_IO;
        atomic_fetch_add_u64(&aa->cmds_failed, 1);
        touch_publish_error(wedged->drive_idx, ERR_IO, wedged->lba,
                            wedged->count, wedged->is_write);
        __atomic_store_n(&wedged->done, 1u, __ATOMIC_RELEASE);
    }
}

/* ---------------------------------------------------------------------
 *  Watchdog scan — BSP PIT tick, one glance per channel (see ata_async.h).
 *
 *  Three tiers, all triggered by hardware fact, never by a clock — except the
 *  single software-unobservable case (TIER 2b), where a liveness-of-last-resort
 *  bound (CONFIG_ATA_LIVENESS_MS) is the only possible signal. Uses spin_trylock
 *  so the tick is never blocked behind a cross-core submit/complete; a skipped
 *  channel is simply re-examined next tick.
 * ------------------------------------------------------------------ */
void bmide_watchdog_scan(void) {
    if (!__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return;
    /* The IRQ+landing path (and thus any wedge to recover) only exists
     * multi-core; single-core drives the polled fallback with its own bound. */
    if (g_amp.total_cores < 2) return;

    uint64_t now          = rdtsc();
    uint64_t liveness_tsc  = cpu_ms_to_tsc(CONFIG_ATA_LIVENESS_MS);

    for (uint8_t ch = 0; ch < ATA_CHANNEL_COUNT; ch++) {
        AtaAsyncCh *aa = &g_ata_async[ch];
        if (!aa->enabled) continue;
        if (__atomic_load_n(&aa->recovering, __ATOMIC_ACQUIRE)) continue;
        /* Cheap unlocked idle early-out — the common case. Re-checked under
         * the lock; a just-submitted cmd read as NULL here has a fresh
         * submit_tsc and is not overdue anyway. */
        if (!__atomic_load_n(&aa->in_flight, __ATOMIC_RELAXED)) continue;

        if (!spin_trylock(&aa->cmd_lock)) continue;   /* never stall the tick */

        AtaCmd *cmd = aa->in_flight;
        if (!cmd || __atomic_load_n(&cmd->reconciled, __ATOMIC_RELAXED)) {
            spin_unlock(&aa->cmd_lock);
            continue;
        }

        uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);

        /* TIER 1 — lost INTRQ: the drive latched its interrupt (BMISR.IRQ) but
         * the CPU IRQ never ran (edge dropped / misrouted on legacy IDE).
         * Retire it now via the same body the IRQ handler uses. Pure event. */
        if (bmsr & BMISR_IRQ) {
            bmide_complete_locked(aa, bmsr);
            spin_unlock(&aa->cmd_lock);
            continue;
        }

        /* No interrupt latched. Alternate Status (ctrl_reg) does NOT clear the
         * drive INTRQ latch, unlike the primary STATUS — safe to snoop. */
        uint8_t alt = inb(g_ata_channels[ch].ctrl_reg);
        bool wedge = false;
        if (alt == 0xFF)                                wedge = true;  /* 2a device gone */
        else if (!(bmsr & BMISR_ACTIVE))                wedge = true;  /* 2a engine quit, no INTRQ */
        else if ((int64_t)(now - cmd->submit_tsc) > (int64_t)liveness_tsc)
            wedge = true;                                              /* 2b frozen engine */

        /* Owed, not run: the up-to-2 s reset cannot run in this IRQ (PIT)
         * context, and the command's waiter is spinning on the channel right
         * now — it runs the reset on its next turn (BmideRecoverIfOwed). */
        if (wedge)
            __atomic_store_n(&aa->recovering, BMIDE_RECOVERY_OWED, __ATOMIC_RELEASE);
        spin_unlock(&aa->cmd_lock);
    }
}

/* ---------------------------------------------------------------------
 *  Submit path
 * ------------------------------------------------------------------ */

static error_t submit_internal(AtaCmd *cmd) {
    uint8_t      ch_idx = cmd->channel;
    AtaAsyncCh  *aa     = &g_ata_async[ch_idx];

    spin_lock(&aa->cmd_lock);
    if (aa->queue_depth >= ATA_CMD_QUEUE_CAP) {
        spin_unlock(&aa->cmd_lock);
        return ERR_BUSY;
    }
    /* Queue (never kick) while a recovery of the channel is owed or running,
     * or a caller has taken the channel whole: the recovery re-launches the
     * queue head itself once the reset settles, and the caller's Give does,
     * so kicking onto a mid-reset drive, or beside a command of the PIO path,
     * is structurally excluded. */
    if (aa->in_flight || aa->claimed ||
        __atomic_load_n(&aa->recovering, __ATOMIC_RELAXED)) {
        queue_push_tail(aa, cmd);
        atomic_fetch_add_u64(&aa->cmds_submitted, 1);
        spin_unlock(&aa->cmd_lock);
        return OK;
    }
    aa->in_flight = cmd;
    atomic_fetch_add_u64(&aa->cmds_submitted, 1);
    /* Hold cmd_lock across bmide_kick to serialise the BMIDE/drive
     * register writes against the IRQ-side completion path on other
     * cores. The kick is bounded (no wait_ready spin); ~10µs with
     * local IRQs off is well inside acceptable budget. */
    bmide_kick(aa, cmd);
    spin_unlock(&aa->cmd_lock);
    return OK;
}

static bool async_path_viable(void) {
    if (!__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return false;
    if (!__atomic_load_n(&g_ata_irq_armed,   __ATOMIC_ACQUIRE)) return false;
    /* Multi-core only, as the IRQ-driven wait, its watchdog and the SRST
     * recovery were built and proven; on one core the BSP drives the channel
     * polled. */
    if (g_amp.total_cores < 2) return false;
    return true;
}

/* ---------------------------------------------------------------------
 *  The waiter's own look at its channel
 *
 *  The drive answers a bus-master DMA with its INTRQ, and the controller
 *  latches that in BMISR.IRQ whether or not any core takes the interrupt it
 *  raises — while the IO-APIC routes the interrupt to the BSP alone. A wait
 *  that only the BSP's interrupt could finish had to turn interrupts on
 *  (`sti`) under whatever lock its caller held, and still waited for good
 *  whenever the BSP spun with its own off: on that very lock, or in a halt's
 *  roll call. So the waiter reads the latch itself and retires what it finds
 *  by the same body, under the same cmd_lock, as the IRQ handler and the
 *  tick's TIER 1: whoever W1Cs the engine first retires the command, and the
 *  others read the latch clear. The interrupt that still reaches the BSP
 *  afterwards finds nothing and is counted in spurious_irqs.
 *
 *  The same look holds the command to the watchdog's liveness bound
 *  (CONFIG_ATA_LIVENESS_MS, TIER 2b): a command out that long with nothing
 *  latched makes a recovery owed from here, so a lost completion ends even
 *  while the BSP's tick does not run — its interrupts shut, on a lock or in a
 *  roll call. The tick's quicker signs of a wedge stay the tick's: the engine
 *  seen idle with nothing latched is also how a drive looks between its last
 *  data word and its INTRQ, and a waiter looking in a tight loop would meet
 *  that on every write.
 *
 *  A trylock, because whoever holds cmd_lock is retiring, moving the queue
 *  along, recovering or kicking the engine, and the waiter comes round again
 *  anyway. It leaves the caller's interrupts as they were: spin_trylock turns
 *  them off only while it holds the lock, so the IRQ handler never meets it
 *  on this core.
 * ------------------------------------------------------------------ */
static void BmideAnswerHere(AtaAsyncCh *aa) {
    if (!spin_trylock(&aa->cmd_lock)) return;
    if (!__atomic_load_n(&aa->recovering, __ATOMIC_RELAXED)) {
        AtaCmd *cmd = aa->in_flight;
        if (cmd && !__atomic_load_n(&cmd->reconciled, __ATOMIC_RELAXED)) {
            uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);
            /* Signed: submit_tsc was read on the core that kicked the
             * command, and a look an instant later on another core may read
             * a counter a few ticks behind it. */
            if (bmsr & BMISR_IRQ)
                bmide_complete_locked(aa, bmsr);
            else if ((int64_t)(rdtsc() - cmd->submit_tsc) >
                     (int64_t)cpu_ms_to_tsc(CONFIG_ATA_LIVENESS_MS))
                __atomic_store_n(&aa->recovering, BMIDE_RECOVERY_OWED, __ATOMIC_RELEASE);
        }
    }
    spin_unlock(&aa->cmd_lock);
}

static inline uint64_t BmideIrqOff(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static inline void BmideIrqBack(uint64_t flags) {
    if (flags & (1u << 9))
        asm volatile("sti" ::: "memory");
}

/* The completion that landed on the channel, claimed and run, whoever's it
 * is — with interrupts off from the claim to the publish of its `done`. A
 * core stopped by a halt, or switched away from, in between would leave
 * another waiter spinning on a command it can no longer see: in_flight has
 * moved on, and nothing else ever publishes that `done`. The window is one
 * staging-page copy and one kick. */
static bool BmideTakeLanding(AtaAsyncCh *aa) {
    uint64_t flags = BmideIrqOff();
    AtaCmd  *c     = __atomic_exchange_n(&aa->landing, NULL, __ATOMIC_ACQ_REL);
    if (c)
        ata_run_completion(aa, c);
    BmideIrqBack(flags);
    return c != NULL;
}

/* A recovery the channel owes, run here by the first to find it owed. The
 * core is held for its length (SchedulerHoldHere): a reset switched away
 * from, or stopped by an orderly halt, would leave `recovering` RUNNING and
 * every command on the channel waiting behind it for good — the halt's own
 * sync among them. It stops, if the halt asked it to, once the reset is
 * done. */
static void BmideRecoverIfOwed(AtaAsyncCh *aa) {
    uint32_t owed = BMIDE_RECOVERY_OWED;
    if (__atomic_load_n(&aa->recovering, __ATOMIC_ACQUIRE) != owed ||
        !__atomic_compare_exchange_n(&aa->recovering, &owed, BMIDE_RECOVERY_RUNNING,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    SchedulerHoldHere();
    ata_recover_worker(aa);
    SchedulerLetGoHere();
    HaltStopIfOwedHere("the reset of a legacy-IDE channel it found owed");
}

/* One turn of every wait on a channel: a command's waiter, and a caller
 * taking the channel whole. A caller with interrupts off takes no shootdown
 * IPI either, so the turn answers shootdowns by hand, as ata_poll_pause
 * does. */
static void BmideWaitTurn(AtaAsyncCh *aa) {
    if (BmideTakeLanding(aa))
        return;
    BmideAnswerHere(aa);
    BmideRecoverIfOwed(aa);
    vmm_tlb_shootdown_poll();
    cpu_pause();
}

/* ---------------------------------------------------------------------
 *  The channel taken whole, for a command that is not the engine's
 * ------------------------------------------------------------------ */
uint64_t AtaChannelTake(uint8_t channel) {
    AtaAsyncCh *aa    = &g_ata_async[channel];
    uint64_t    flags = BmideIrqOff();

    for (;;) {
        spin_lock(&aa->cmd_lock);
        bool mine = !aa->claimed;
        aa->claimed = true;
        spin_unlock(&aa->cmd_lock);
        if (mine)
            break;
        BmideWaitTurn(aa);
    }
    /* From here the engine starts nothing new; what it has on the channel is
     * finished — retired by this turn or by its own waiter — or reset. */
    for (;;) {
        spin_lock(&aa->cmd_lock);
        bool idle = !aa->in_flight &&
                    __atomic_load_n(&aa->recovering, __ATOMIC_RELAXED) == BMIDE_RECOVERY_NONE;
        spin_unlock(&aa->cmd_lock);
        if (idle)
            return flags;
        BmideWaitTurn(aa);
    }
}

void AtaChannelGive(uint8_t channel, uint64_t flags) {
    AtaAsyncCh *aa = &g_ata_async[channel];

    spin_lock(&aa->cmd_lock);
    aa->claimed = false;
    if (aa->enabled && !aa->in_flight)
        bmide_launch_next_locked(aa);
    spin_unlock(&aa->cmd_lock);
    BmideIrqBack(flags);
}

/* The recovery ata_recover_worker runs on a wedge, asked from outside: the
 * channel taken whole as a flush takes it, so no command of the engine's or
 * of a PIO caller's finds it mid-reset; SRST; the drive's INTRQ unmasked again
 * when the engine's IRQ path is what waits for it (SRST left nIEN=1). The
 * drive is back when, selected again, it is neither absent, nor BSY, nor
 * without DRDY. ATA_SRST_BSY_HELD_MS: the channel is held, interrupts off. */
bool AtaDriveReset(uint8_t drive_idx) {
    if (drive_idx >= ATA_DRIVE_COUNT || !g_ata_devices[drive_idx].exists ||
        !drive_channel_ptr(drive_idx)->present)
        return false;

    uint8_t     ch_idx = drive_channel(drive_idx);
    AtaAsyncCh *aa     = &g_ata_async[ch_idx];
    uint64_t    held   = AtaChannelTake(ch_idx);

    ata_channel_soft_reset(ch_idx, ATA_SRST_BSY_HELD_MS);
    ata_select_drive(drive_idx);
    uint8_t status = ata_read_status(drive_idx);
    bool    back   = status != 0 && status != 0xFF && !(status & ATA_SR_BSY) &&
                     (status & ATA_SR_DRDY);
    if (aa->enabled && __atomic_load_n(&g_ata_irq_armed, __ATOMIC_ACQUIRE))
        ata_set_nien(ch_idx, false);

    AtaChannelGive(ch_idx, held);
    return back;
}

/* ---------------------------------------------------------------------
 *  Sync wrapper — submit and wait for the drive's answer, polled fallback
 *  before the IRQ path is viable
 * ------------------------------------------------------------------ */
int ata_dma_sync(uint8_t drive_idx, uint64_t lba, uint16_t count,
                 bool is_write, void *buf)
{
    if (drive_idx >= ATA_DRIVE_COUNT) return ATA_ERR_INVALID_ARGS;
    if (!buf || count == 0 || count > ATA_DMA_MAX_SECTORS)
        return ATA_ERR_INVALID_ARGS;
    if (!ata_async_usable(drive_idx)) return ATA_ERR_NOT_SUPPORTED;

    AtaCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.drive_idx    = drive_idx;
    cmd.channel      = drive_channel(drive_idx);
    cmd.lba          = lba;
    cmd.count        = count;
    cmd.is_write     = is_write;
    cmd.user_buf     = buf;
    cmd.sync_rc      = OK;

    if (!async_path_viable()) {
        /* Not multi-core, or the IRQ side not armed — drive the channel
         * polled, with nIEN temporarily set so the IRQ path stays silent, and
         * the channel taken whole: on one core the idle context's transfer
         * and a system call's may otherwise meet on it. */
        uint64_t held = AtaChannelTake(cmd.channel);
        int rc = bmide_transfer_polled(&cmd);
        AtaChannelGive(cmd.channel, held);
        return rc;
    }

    /* IRQ-driven path: submit, then wait for done. */
    error_t srq = submit_internal(&cmd);
    if (srq != OK) {
        /* The channel's queue is full. Nothing timed out and nothing about
         * this drive is broken, so it is said as what it is — the caller
         * takes the PIO path for this one transfer and leaves the DMA path's
         * standing alone (ata_note_dma_fallback in ata.c). */
        if (srq == ERR_BUSY)         return ATA_ERR_BUSY;
        if (srq == ERR_INVALID_ARGUMENT) return ATA_ERR_INVALID_ARGS;
        return ATA_ERR_DRIVE_FAULT;
    }

    /* The wait, with interrupts exactly as the caller left them (off, when
     * it holds a spin_lock), and needing none on any core. Each turn claims
     * whatever completion has landed on the channel and runs its bottom half;
     * the claimed one may be ANOTHER waiter's cmd on this channel, and that
     * is correct and necessary: whoever is awake runs the completion that
     * landed. Otherwise the turn looks at the channel's interrupt latch
     * itself (BmideAnswerHere), so a BSP spinning with its interrupts off —
     * on a lock this caller holds, or in a halt's roll call — cannot strand
     * the command whose interrupt only it would take; and it runs a recovery
     * the channel owes itself (BmideRecoverIfOwed), so no reset waits on a
     * drain core that never pumps (BmideWaitTurn).
     *
     * No deadline of the wait's own: the stack `cmd` is live until done is
     * observed. A drive that never answers is failed ERR_IO by the reset the
     * watchdog's bound makes owed — the tick's, or this waiter's own look
     * once CONFIG_ATA_LIVENESS_MS has passed. Healthy drives complete in
     * <1 ms. */
    AtaAsyncCh *aa = &g_ata_async[cmd.channel];
    while (!__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {
        BmideWaitTurn(aa);
    }

    /* Tripwire: landing_clobber must be 0 (≤1-in-flight guarantees landing is
     * NULL at every stamp). Print once, loudly, if it ever fired — a clobber
     * means a completion was lost upstream (and its spinner hung). */
    if (__atomic_load_n(&aa->landing_clobber, __ATOMIC_RELAXED) != 0 &&
        __atomic_exchange_n(&aa->clobber_warned, 1u, __ATOMIC_ACQ_REL) == 0) {
        kprintf("[ATA] BUG: landing_clobber=%llu on channel %u — "
                "the <=1-in-flight invariant broke\n",
                (unsigned long long)__atomic_load_n(&aa->landing_clobber, __ATOMIC_RELAXED),
                (unsigned)cmd.channel);
    }

    if (cmd.sync_rc == OK)                   return 0;
    if (cmd.sync_rc == ERR_DEVICE_NOT_READY) return ATA_ERR_NO_DEVICE;
    return ATA_ERR_DRIVE_FAULT;
}

/* ---------------------------------------------------------------------
 *  BMIDE per-channel bring-up
 * ------------------------------------------------------------------ */
static void bmide_init_channel(uint8_t ch_idx) {
    AtaChannel *ch = &g_ata_channels[ch_idx];
    AtaAsyncCh *aa = &g_ata_async[ch_idx];

    aa->enabled = false;
    if (!ch->present || ch->bmide_base == 0) return;

    aa->bmide_base = ch->bmide_base;
    aa->prd_virt   = g_prd_storage[ch_idx];
    aa->prd_phys   = (uintptr_t)aa->prd_virt - CONFIG_KERNEL_VMA_OFFSET;
    if (!IS_32BIT_SAFE(aa->prd_phys)) {
        debug_printf("[ATA_ASYNC] ch%u: PRD table above 4 GB, DMA disabled\n", ch_idx);
        return;
    }

    void *page_phys = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!page_phys) {
        debug_printf("[ATA_ASYNC] ch%u: no DMA32 page, DMA disabled\n", ch_idx);
        return;
    }
    aa->buf_phys = (uintptr_t)page_phys;
    aa->buf_virt = vmm_phys_to_virt(aa->buf_phys);
    if (!aa->buf_virt) {
        pmm_free(page_phys, 1);
        debug_printf("[ATA_ASYNC] ch%u: cannot map staging page, DMA disabled\n", ch_idx);
        return;
    }

    spinlock_init(&aa->cmd_lock);
    aa->in_flight    = NULL;
    aa->wait_head    = NULL;
    aa->wait_tail    = NULL;
    aa->queue_depth  = 0;

    aa->ch_idx     = ch_idx;
    aa->recovering = BMIDE_RECOVERY_NONE;
    aa->claimed    = false;

    /* Stale-handover scrub: stop engine, W1C error/IRQ. */
    bmide_engine_stop(aa);

    aa->enabled = true;
    debug_printf("[ATA_ASYNC] ch%u BMIDE up — bm_base=0x%04x prd_phys=0x%08lx "
                 "buf_phys=0x%08lx\n",
                 ch_idx, aa->bmide_base,
                 (unsigned long)aa->prd_phys, (unsigned long)aa->buf_phys);
}

/* ---------------------------------------------------------------------
 *  DMA-mode negotiation (UDMA preferred, MDMA fallback)
 * ------------------------------------------------------------------ */
/* IDENTIFY word 93 — the cable evidence, and the only kind a host without
 * CBLID- sensing circuitry can have (ATA/ATAPI-6 r3a §8.15.49, Annex B).
 * Bit 15 = 0 and bit 14 = 1 mark the word valid; bit 13 is what the device saw
 * on PDIAG-:CBLID- during its own IDENTIFY: set means the line was above VIH,
 * which is the 80-conductor assembly leaving it disconnected and pulled up.
 * Bits (15:13) = 011b is the "80" row of §5.2.11 Table 8, and it is the same
 * (word93 & 0xE000) == 0x6000 Linux tests in ata_drive_40wire(). */
#define ATA_ID_CABLE_WORD          93
#define ATA_ID_CABLE_MASK          0xE000u
#define ATA_ID_CABLE_80_CONDUCTOR  0x6000u
#define ATA_ID_MAJOR_VERSION_WORD  80
#define ATA_ID_MAJOR_VERSION_ATA5  0x0020

/* The highest Ultra DMA mode a system with no proof of an 80-conductor cable
 * may put a device into. ATA/ATAPI-6 (T13/1410D r3a) §8.46.11, the description
 * of the very subcommand ata_set_xfer_mode issues: "For systems using a cable
 * assembly, the host shall detect that an 80-conductor cable assembly is
 * connecting the host with the device(s) before enabling any Ultra DMA mode
 * greater than 2 in the device(s) (see Annex B)." §6.7 says it again of the
 * system as a whole, and §5.2.11 Table 8 Notes 1 and 2 say what to do when the
 * evidence is missing or contradictory: "host should not use Ultra DMA modes
 * higher than mode 2 without using other means to confirm presence of
 * 80-conductor cable." */
#define ATA_UDMA_MAX_40_CONDUCTOR  2

/* Does this drive prove the cable can carry Ultra DMA above mode 2?
 *
 * Two ways it can, and no third. Serial ATA behind a legacy/compatibility-mode
 * controller — the configuration that puts a modern board on this driver at
 * all — has no ribbon to be wrong about, and it says so by reporting word 93
 * as zero while word 80 claims ATA-5 or later (the same pair Linux's
 * ata_id_is_sata() reads; the signed test on word 80 throws out the reserved
 * 0000h and FFFFh). Otherwise the device's own CBLID- sample decides, per
 * Annex B, and anything short of the 011b row is NOT proof: an invalid word,
 * a device that does not implement the test, or a device that saw the line
 * pulled low through a 40-conductor cable all come out the same — unproven,
 * and therefore capped.
 *
 * Host-side detection (§6.7's CBLID- sense at the host connector) is the
 * method the standard puts first, but it lives in a per-chipset register this
 * driver has no table for; leaving the cap on when it cannot be read is the
 * conservative half of the same rule, not a shortcut around it. */
static bool ata_cable_carries_fast_udma(const uint16_t *id)
{
    if (id[ATA_ID_CABLE_WORD] == 0 &&
        (int16_t)id[ATA_ID_MAJOR_VERSION_WORD] >= ATA_ID_MAJOR_VERSION_ATA5)
        return true;   /* Serial ATA — no cable assembly to qualify */

    return (id[ATA_ID_CABLE_WORD] & ATA_ID_CABLE_MASK) == ATA_ID_CABLE_80_CONDUCTOR;
}

void ata_async_negotiate_dma_mode(uint8_t drive_idx, const uint16_t *id) {
    ATADevice *dev = &g_ata_devices[drive_idx];
    dev->dma_type = ATA_DMA_TYPE_NONE;
    dev->dma_mode = 0;

    if (!(id[49] & (1u << 8))) return;  /* IDENTIFY word 49 bit 8 = DMA */

    if (id[53] & (1u << 2)) {
        uint16_t udma_sup = id[88] & 0x7Fu;
        int top = 6;
        if (!ata_cable_carries_fast_udma(id)) {
            top = ATA_UDMA_MAX_40_CONDUCTOR;
            /* Only worth a line when the drive would otherwise have gone
             * faster — and it is a kprintf, because a disk running at a third
             * of its rated speed for a cable reason is exactly the thing an
             * operator on real hardware has to be able to see. */
            if (udma_sup >> (ATA_UDMA_MAX_40_CONDUCTOR + 1))
                kprintf("[ATA] drv%u: no 80-conductor cable proof (IDENTIFY "
                        "word 93 = 0x%04x) — Ultra DMA held at mode %d\n",
                        drive_idx, id[ATA_ID_CABLE_WORD],
                        ATA_UDMA_MAX_40_CONDUCTOR);
        }
        for (int mode = top; mode >= 0; mode--) {
            if (!(udma_sup & (1u << mode))) continue;
            if (ata_set_xfer_mode(drive_idx, (uint8_t)(0x40 | mode)) == 0) {
                dev->dma_type = ATA_DMA_TYPE_UDMA;
                dev->dma_mode = (uint8_t)mode;
                return;
            }
        }
    }

    uint16_t mdma_sup = id[63] & 0x07u;
    for (int mode = 2; mode >= 0; mode--) {
        if (!(mdma_sup & (1u << mode))) continue;
        if (ata_set_xfer_mode(drive_idx, (uint8_t)(0x20 | mode)) == 0) {
            dev->dma_type = ATA_DMA_TYPE_MDMA;
            dev->dma_mode = (uint8_t)mode;
            return;
        }
    }
}

bool ata_async_usable(uint8_t drive_idx) {
    if (drive_idx >= ATA_DRIVE_COUNT) return false;
    if (!g_ata_async[drive_channel(drive_idx)].enabled) return false;
    if (g_ata_devices[drive_idx].dma_type == ATA_DMA_TYPE_NONE) return false;
    return true;
}

/* ---------------------------------------------------------------------
 *  Public init — called once from ata_init, after channels discovered
 * ------------------------------------------------------------------ */
void ata_async_init(void) {
    if (__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return;

    memset(g_ata_async, 0, sizeof(g_ata_async));

    /* The error report's name, once: ata_init runs after guide_init, so the
     * registry is up, and the completion paths that say an error must not
     * resolve anything themselves (touch_publish_error). */
    {
        TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
        TouchLogbookResolve("storage:ata:error", &full, &bare);
        __atomic_store_n(&g_ata_err_full, full, __ATOMIC_RELEASE);
        __atomic_store_n(&g_ata_err_bare, bare, __ATOMIC_RELEASE);
    }

    /* AHCI owns block I/O — legacy BMIDE engine stays dormant. */
    if (ahci_is_initialized()) {
        debug_printf("[ATA_ASYNC] AHCI active — BMIDE async engine skipped\n");
        __atomic_store_n(&g_ata_async_ready, 1u, __ATOMIC_RELEASE);
        return;
    }

    for (uint8_t c = 0; c < ATA_CHANNEL_COUNT; c++) {
        bmide_init_channel(c);
    }

    /* IRQ wiring — only register if at least one channel has BMIDE up,
     * else there is no consumer for the line and unmask would surface
     * spurious IDE INTRQs. */
    bool any = false;
    for (uint8_t c = 0; c < ATA_CHANNEL_COUNT; c++) any = any || g_ata_async[c].enabled;
    if (!any) {
        debug_printf("[ATA_ASYNC] No BMIDE-capable channel — IRQ wiring skipped\n");
        __atomic_store_n(&g_ata_async_ready, 1u, __ATOMIC_RELEASE);
        return;
    }

    uint8_t gsi0 = g_ata_channels[0].present ? g_ata_channels[0].irq_gsi : 0xFF;
    uint8_t gsi1 = g_ata_channels[1].present ? g_ata_channels[1].irq_gsi : 0xFF;

    if (gsi0 != 0xFF) {
        irq_register_handler(gsi0, ata_irq_handler, RIGHT_OF_WAY_WORK_IS_REDONE);
        irqchip_enable_irq(gsi0);
    }
    if (gsi1 != 0xFF && gsi1 != gsi0) {
        irq_register_handler(gsi1, ata_irq_handler, RIGHT_OF_WAY_WORK_IS_REDONE);
        irqchip_enable_irq(gsi1);
    }

    /* Clear nIEN on every channel that just got BMIDE — drive INTRQ
     * now propagates to BMISR.IRQ and on to the IDT through the
     * IO-APIC pin we just unmasked. */
    for (uint8_t c = 0; c < ATA_CHANNEL_COUNT; c++) {
        if (g_ata_async[c].enabled) ata_set_nien(c, false);
    }

    __atomic_store_n(&g_ata_irq_armed,   1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ata_async_ready, 1u, __ATOMIC_RELEASE);

    debug_printf("[ATA_ASYNC] ready — ch0=%u ch1=%u gsi0=%u gsi1=%u\n",
                 g_ata_async[0].enabled, g_ata_async[1].enabled,
                 gsi0, gsi1);
}

/* ---------------------------------------------------------------------
 *  Stats accessors
 * ------------------------------------------------------------------ */
uint64_t ata_async_cmds_submitted(uint8_t channel) {
    return channel < ATA_CHANNEL_COUNT ? atomic_load_u64(&g_ata_async[channel].cmds_submitted) : 0;
}
uint64_t ata_async_cmds_completed(uint8_t channel) {
    return channel < ATA_CHANNEL_COUNT ? atomic_load_u64(&g_ata_async[channel].cmds_completed) : 0;
}
uint64_t ata_async_cmds_failed(uint8_t channel) {
    return channel < ATA_CHANNEL_COUNT ? atomic_load_u64(&g_ata_async[channel].cmds_failed) : 0;
}
uint64_t ata_async_spurious_irqs(uint8_t channel) {
    return channel < ATA_CHANNEL_COUNT ? atomic_load_u64(&g_ata_async[channel].spurious_irqs) : 0;
}
uint32_t ata_async_queue_depth(uint8_t channel) {
    if (channel >= ATA_CHANNEL_COUNT) return 0;
    AtaAsyncCh *aa = &g_ata_async[channel];
    spin_lock(&aa->cmd_lock);
    uint32_t d = aa->queue_depth + (aa->in_flight ? 1u : 0u);
    spin_unlock(&aa->cmd_lock);
    return d;
}

/* ---------------------------------------------------------------------
 *  Boot self-test — TIER-1 (lost-INTRQ) reconcile, on the real BMIDE engine.
 *
 *  The watchdog's most valuable recovery is TIER 1: a completion the drive DID
 *  signal (BMISR.IRQ latched) but whose CPU interrupt was dropped/misrouted —
 *  the classic legacy-IDE INTRQ loss. We reproduce it exactly and safely: mask
 *  the channel's IOAPIC pin (so a genuine 1-sector read completes with the
 *  interrupt latched in BMISR but never delivered to a core), then drive the
 *  scan ourselves and confirm it retires the command with the data read back.
 *
 *  Safe by construction: read-only (LBA 0), the stack cmd is retired under
 *  cmd_lock BEFORE the pin is re-enabled (no late edge can touch this frame),
 *  and the wait is bounded — a broken watchdog FAILS loudly here, never hangs.
 *  The SRST tiers (2a/2b) require a genuinely wedged drive and are reachable
 *  only on real PATA hardware. Multi-core + BMIDE only; skips otherwise.
 * ------------------------------------------------------------------ */
error_t bmide_watchdog_selftest(void) {
    if (g_amp.total_cores < 2) {
        kprintf("[BMIDE-WD] TIER-1 self-test skipped (single-core)\n");
        return OK;
    }
    uint8_t drive = 0xFF;
    for (uint8_t d = 0; d < ATA_DRIVE_COUNT; d++) {
        if (ata_async_usable(d)) { drive = d; break; }
    }
    if (drive == 0xFF || !async_path_viable()) {
        kprintf("[BMIDE-WD] TIER-1 self-test skipped (no BMIDE async drive)\n");
        return OK;
    }

    uint8_t     ch  = drive_channel(drive);
    AtaAsyncCh *aa  = &g_ata_async[ch];
    uint8_t     gsi = g_ata_channels[ch].irq_gsi;

    void *buf = kmalloc(ATA_SECTOR_SIZE);
    if (!buf) { kprintf("[BMIDE-WD] TIER-1 self-test skipped (no mem)\n"); return OK; }

    AtaCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.drive_idx    = drive;
    cmd.channel      = ch;
    cmd.lba          = 0;                 /* boot sector — read-only, non-destructive */
    cmd.count        = 1;
    cmd.is_write     = false;
    cmd.user_buf     = buf;
    cmd.sync_rc      = OK;

    /* Simulate a dropped IDE INTRQ: mask the channel's IOAPIC pin. The drive
     * still asserts INTRQ on completion (latched in BMISR.IRQ), but no CPU
     * interrupt runs — so only the watchdog can retire the command. */
    irqchip_disable_irq(gsi);

    error_t sub = submit_internal(&cmd);
    bool ok = false;
    if (sub == OK) {
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(3000);
        while (rdtsc() < deadline) {
            bmide_watchdog_scan();                 /* TIER-1 → stamps `landing` */
            BmideTakeLanding(aa);
            if (__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {
                ok = (cmd.sync_rc == OK);
                break;
            }
            cpu_pause();
        }
    }

    /* Retire any residue BEFORE re-enabling the pin, so a late edge cannot
     * dereference this stack frame after we return. */
    if (!ok) {
        spin_lock(&aa->cmd_lock);
        if (aa->in_flight == &cmd) { bmide_engine_stop(aa); aa->in_flight = NULL; }
        __atomic_store_n(&aa->landing, NULL, __ATOMIC_RELAXED);
        spin_unlock(&aa->cmd_lock);
    }
    irqchip_enable_irq(gsi);
    kfree(buf);

    if (ok) {
        kprintf("[BMIDE-WD] TIER-1 lost-INTRQ reconcile PASS\n");
        return OK;
    }
    kprintf("[BMIDE-WD] TIER-1 lost-INTRQ reconcile FAIL (sub=%d rc=%d)\n",
            (int)sub, (int)cmd.sync_rc);
    return ERR_IO;
}

/* ---------------------------------------------------------------------
 *  On-demand diagnostic — TIER-2 wedge -> SRST recovery, end-to-end.
 *
 *  QEMU's PIIX IDE completes reliably and cannot be made to hang, so the
 *  genuine-wedge path (bmide_watchdog_scan TIER-2 -> ata_recover_worker) has no
 *  natural trigger. This drives it deterministically: install an in-flight cmd
 *  for which NO DMA was ever issued — the drive will never signal, an exact
 *  "engine idle, no INTRQ" wedge — then let the real scan detect it (TIER-2a)
 *  and mark the recovery owed, turn the wait a waiter turns, and assert the
 *  recovery it runs (a) failed the wedged cmd ERR_IO and (b) left the channel
 *  usable (a real read succeeds).
 *
 *  This SRSTs the boot drive, so it is compiled in only under WEDGETEST=on and
 *  never runs on a production boot. Safe: read-only, channel-quiescent guard,
 *  bounded wait, and the fake cmd's stack lifetime is covered by the worker's
 *  publish-done-last ordering (no DMA was issued, so nothing else references it).
 * ------------------------------------------------------------------ */
#if CONFIG_BMIDE_WEDGE_SELFTEST
error_t bmide_wedge_selftest(void) {
    if (g_amp.total_cores < 2) {
        kprintf("[BMIDE-WD] TIER-2 self-test skipped (single-core)\n");
        return OK;
    }
    uint8_t drive = 0xFF;
    for (uint8_t d = 0; d < ATA_DRIVE_COUNT; d++) {
        if (ata_async_usable(d)) { drive = d; break; }
    }
    if (drive == 0xFF || !async_path_viable()) {
        kprintf("[BMIDE-WD] TIER-2 self-test skipped (no BMIDE async drive)\n");
        return OK;
    }

    uint8_t     ch  = drive_channel(drive);
    AtaAsyncCh *aa  = &g_ata_async[ch];
    void *vbuf = kmalloc(ATA_SECTOR_SIZE);
    if (!vbuf) { kprintf("[BMIDE-WD] TIER-2 self-test skipped (no mem)\n"); return OK; }

    AtaCmd fake;
    memset(&fake, 0, sizeof(fake));
    fake.drive_idx    = drive;
    fake.channel      = ch;
    fake.lba          = 0;
    fake.count        = 1;
    fake.is_write     = false;
    fake.user_buf     = vbuf;
    fake.sync_rc      = OK;
    fake.submit_tsc   = rdtsc();

    /* Install as in-flight WITHOUT issuing DMA — a wedge the drive never ends.
     * Only on a quiescent channel (no live traffic to disturb). */
    spin_lock(&aa->cmd_lock);
    bool installed = (aa->in_flight == NULL && !aa->recovering && !aa->claimed);
    if (installed) aa->in_flight = &fake;
    spin_unlock(&aa->cmd_lock);
    if (!installed) {
        kfree(vbuf);
        kprintf("[BMIDE-WD] TIER-2 self-test skipped (channel busy)\n");
        return OK;
    }

    /* Real detection path: the scan sees engine-idle-without-INTRQ (TIER-2a)
     * and marks the channel's recovery owed. */
    bmide_watchdog_scan();

    /* Bounded wait for the recovery to SRST + fail `fake` ERR_IO, turned the
     * way every waiter on the channel turns (BmideWaitTurn) — so what runs the
     * reset is the waiter's own turn, as it is for a real wedge, not a direct
     * call. */
    bool worker_ok = false;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(6000);
    while (rdtsc() < deadline) {
        BmideWaitTurn(aa);
        if (__atomic_load_n(&fake.done, __ATOMIC_ACQUIRE)) {
            worker_ok = (fake.sync_rc == ERR_IO);
            break;
        }
    }

    /* Safety net: if the worker never completed (a regression), the fake would
     * be left in-flight with recovering stuck — its stack frame is about to die.
     * Undo it so boot continues cleanly and nothing dereferences a dead frame. */
    if (!worker_ok) {
        spin_lock(&aa->cmd_lock);
        if (aa->in_flight == &fake) { bmide_engine_stop(aa); aa->in_flight = NULL; }
        __atomic_store_n(&aa->landing, NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&aa->recovering, BMIDE_RECOVERY_NONE, __ATOMIC_RELEASE);
        spin_unlock(&aa->cmd_lock);
    }

    /* Prove the channel recovered: a real read must now succeed (exercises the
     * same submit -> kick -> complete path the worker's queue re-launch uses). */
    bool recovered = worker_ok && (ata_dma_sync(drive, 0, 1, false, vbuf) == 0);

    kfree(vbuf);

    if (worker_ok && recovered) {
        kprintf("[BMIDE-WD] TIER-2 wedge->SRST recover PASS "
                "(cmd failed ERR_IO + channel re-usable)\n");
        return OK;
    }
    kprintf("[BMIDE-WD] TIER-2 wedge->SRST recover FAIL "
            "(worker_ok=%d done=%d rc=%d recovered=%d)\n",
            (int)worker_ok, (int)fake.done, (int)fake.sync_rc, (int)recovered);
    return ERR_IO;
}
#endif /* CONFIG_BMIDE_WEDGE_SELFTEST */
