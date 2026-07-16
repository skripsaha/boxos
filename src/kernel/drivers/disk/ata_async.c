/* ===========================================================================
 *  ata_async.c — IRQ-driven Bus-Master IDE DMA engine.
 *
 *  Replaces the polled completion loop with a submit/IRQ pipeline:
 *
 *    submit (caller, K-Core)
 *      ├── kmalloc AtaCmd, snapshot args
 *      ├── lock channel, link onto in_flight (or queue tail), unlock
 *      └── if just installed → kick_dma() programs PRD + LBA + SSBM
 *
 *    INTRQ from drive → IDT dispatcher → ata_irq_handler
 *      ├── for each channel: if BMISR.IRQ set →
 *      │     read STATUS (clears drive-side INTRQ per ATA-7 §6.2.5)
 *      │     classify status (BMISR.ERROR / STATUS.ERR → ERR_IO)
 *      │     reset BMIDE engine, W1C BMISR.ERROR|IRQ
 *      │     defer the heavy work via irq_defer
 *      └── EOI handled by IDT dispatcher
 *
 *    irq_defer pump (K-Core) → ata_complete_deferred
 *      ├── if read & OK: memcpy DMA staging → user_buf
 *      ├── lock channel, pop next wait_head → in_flight; unlock; kick_dma
 *      ├── invoke cb (Touch publish here if needed)
 *      └── kfree AtaCmd (skip if sync-wait, caller owns)
 *
 *  Single-channel cap of one in-flight cmd matches PATA hardware: master
 *  and slave share the bus and only one ATA command may be outstanding
 *  at a time. Submits past that point queue up; the bottom-half pops the
 *  next one when the current retires.
 *
 *  Pre-multicore boot path: the IRQ infrastructure isn't viable until
 *  irq_defer_init runs and a K-Core exists to pump deferred slots, so
 *  ata_dma_sync transparently falls back to a polled BMIDE engine for
 *  that narrow window (tagfs mount, fsck). The polled path keeps nIEN=1
 *  so it never collides with the IRQ-driven path that is enabled later.
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
#include "irq_defer.h"
#include "idt.h"
#include "irqchip.h"
#include "touch.h"
#include "storage_completion.h"   /* never-drop K-Core recovery node */

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
    bool           is_sync_wait;
    void          *user_buf;     /* kernel-mapped src (write) or dst (read) */

    AtaAsyncCb     cb;
    void          *cb_ctx;

    volatile uint32_t done;      /* 0 = in-flight, 1 = completed */
    volatile error_t  sync_rc;
    uint64_t       submit_tsc;

    /* Set to 1 (under cmd_lock) by whoever classifies this cmd's completion —
     * the IRQ handler OR the watchdog's TIER-1 reconcile. It is the arbiter
     * that keeps the periodic watchdog scan from mis-judging a just-completed
     * cmd (in_flight still points at it until its bottom-half runs) as a fresh
     * wedge: the scan only ever acts on an UN-reconciled in_flight cmd. */
    volatile uint8_t  reconciled;

    struct AtaCmd *next;
} AtaCmd;

/* Staged error info for the deferred (K-Core) Touch publish — see
 * ata_run_completion / ata_err_worker. Kept off the completing spinner
 * because that spinner may hold tagfs write_lock, and TouchPublish takes
 * subscriber locks (a REACT subscriber can take write_lock → inversion). */
typedef struct AtaErrEv {
    uint8_t  drive;
    bool     is_write;
    uint16_t count;
    error_t  rc;
    uint64_t lba;
} AtaErrEv;

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
    volatile uint64_t spurious_irqs;

    /* Ф26 landing — per-channel single-slot NEVER-DROP completion mailbox
     * for the SYNC path. The IRQ stamps the completed cmd here (RELEASE);
     * the waiting spinner claims it (ACQ_REL XCHG) and runs the bottom-half
     * itself, so a sync BMIDE completion can never be dropped the way
     * irq_defer could. ≤1 in-flight per channel ⇒ one slot suffices; the
     * async fire-and-forget path (no spinner) still rides irq_defer. */
    AtaCmd   *volatile landing;
    volatile uint64_t  landing_clobber;   /* invariant tripwire — must stay 0 */
    volatile uint8_t   clobber_warned;    /* one-shot: loud print on first clobber */
    AtaErrEv           err_ev;            /* staged for deferred error publish */

    /* Ф26 BMIDE watchdog. On a genuine wedge the PIT-tick scan CASes
     * `recovering` 0->1 (coalescing repeat detections into one) and posts
     * `recover_node` to a K-Core via StorageCompletionPush — never-drop and
     * allocation-free, exactly as AHCI defers COMRESET — because
     * ata_channel_soft_reset busy-waits BSY for up to 2 s, far too long for the
     * tick's IRQ context. While recovering, the IRQ handler / scan / submit all
     * stand off the channel. run = ata_recover_worker, ctx = this channel; set
     * once in bmide_init_channel. */
    uint8_t            ch_idx;
    volatile uint32_t  recovering;
    StorageCompletion  recover_node;
} AtaAsyncCh;

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

static inline void ata_delay_400ns(uint8_t drive_idx) {
    uint16_t ctrl = drive_channel_ptr(drive_idx)->ctrl_reg;
    for (int i = 0; i < 4; i++) (void)inb(ctrl);
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
    for (int i = 0; i < 4; i++) (void)inb(ch->ctrl_reg); /* 400 ns settle */
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
 *     (which leaves it idle / DRDY) before popping next from queue.
 *
 * No ata_wait_ready spin here — that would busy-poll inside cmd_lock with
 * IRQs off for up to CONFIG_ATA_TIMEOUT_MS; safety relies on the
 * serialisation protocol instead. */
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

/* Polled fallback. Used only when irq_defer isn't yet up (early init /
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

/* ---------------------------------------------------------------------
 *  Bottom half — runs on K-Core via irq_defer pump
 * ------------------------------------------------------------------ */

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
    TouchPublish("storage:ata:error", &ev, sizeof(ev));
}

/* Deferred error publish: runs on a K-Core (irq_defer), so it never holds
 * tagfs write_lock and TouchPublish's subscriber locks are order-safe. */
static void ata_err_worker(void *ctx) {
    AtaAsyncCh *aa = (AtaAsyncCh *)ctx;
    AtaErrEv e = aa->err_ev;   /* snapshot */
    touch_publish_error(e.drive, e.rc, e.lba, e.count, e.is_write);
}

/* Shared completion bottom-half. Invoked by the SYNC-path spinner (via a
 * landing claim) AND by the ASYNC-path irq_defer trampoline. Straight-line,
 * never blocks: memcpy-out, kick the next queued cmd, then signal the
 * spinner (sync) or fire the callback + free (async). Because the sync
 * caller may run this while holding tagfs write_lock, the error Touch
 * publish is DEFERRED to a K-Core (ata_err_worker) — the actual I/O error
 * is still returned to the caller via cmd->sync_rc regardless. */
static void ata_run_completion(AtaAsyncCh *aa, AtaCmd *cmd) {
    /* Read path: copy DMA staging into the caller's buffer BEFORE the next
     * kick reuses the shared staging page. */
    if (!cmd->is_write && cmd->sync_rc == OK && cmd->user_buf) {
        memcpy(cmd->user_buf, aa->buf_virt, (uint32_t)cmd->count * ATA_SECTOR_SIZE);
    }

    /* Pull the next queued cmd and kick the engine on it. cmd_lock serialises
     * the BMIDE register writes against a fast next-completion IRQ. */
    spin_lock(&aa->cmd_lock);
    AtaCmd *next = queue_pop_head(aa);
    aa->in_flight = next;
    if (next) bmide_kick(aa, next);
    spin_unlock(&aa->cmd_lock);

    if (cmd->sync_rc != OK) {
        atomic_fetch_add_u64(&aa->cmds_failed, 1);
        aa->err_ev = (AtaErrEv){ .drive = cmd->drive_idx, .is_write = cmd->is_write,
                                 .count = cmd->count, .rc = cmd->sync_rc, .lba = cmd->lba };
        irq_defer(ata_err_worker, aa);   /* best-effort telemetry, off the spinner */
    }
    atomic_fetch_add_u64(&aa->cmds_completed, 1);

    if (cmd->cb) {
        cmd->cb(cmd->drive_idx, cmd->sync_rc, cmd->cb_ctx);
    }

    if (cmd->is_sync_wait) {
        /* Caller owns the stack cmd. Publish done; the spinner returns. */
        __atomic_store_n(&cmd->done, 1u, __ATOMIC_RELEASE);
    } else {
        kfree(cmd);
    }
}

/* irq_defer trampoline for the async fire-and-forget path (no spinner).
 * Kept so the async submit API stays a working seam for a future
 * IDE-only user-async backend; the live (sync) path uses landing. */
static void ata_complete_deferred(void *ctx) {
    AtaCmd *cmd = (AtaCmd *)ctx;
    if (!cmd) return;
    ata_run_completion(&g_ata_async[cmd->channel], cmd);
}

/* ---------------------------------------------------------------------
 *  Shared completion body — classify + retire the in-flight cmd.
 *
 *  Caller holds cmd_lock and has already established, under that lock, that
 *  aa->in_flight is non-NULL, un-reconciled, and its INTRQ is asserted
 *  (`bmsr` is that same snapshot, IRQ bit set). Invoked identically by the
 *  raw IRQ handler and by the watchdog's TIER-1 lost-INTRQ reconcile, so a
 *  completion is retired by exactly one of them (whoever W1Cs the engine
 *  first clears BMISR.IRQ; the other then reads it clear and no-ops).
 *
 *  For a SYNC cmd it stamps the never-drop `landing` slot and returns NULL.
 *  For an ASYNC cmd it returns the cmd so the caller can irq_defer it AFTER
 *  releasing cmd_lock (never nest cmd_lock -> irq_defer's ring lock).
 * ------------------------------------------------------------------ */
static AtaCmd *bmide_complete_locked(AtaAsyncCh *aa, uint8_t bmsr) {
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

    /* SYNC cmds (all live traffic): stamp the per-channel landing slot — a
     * live spinner claims it (XCHG) and runs the bottom-half itself, so this
     * can NEVER be dropped. The ≤1-in-flight invariant guarantees landing is
     * NULL here (the prior completion was claimed before its next cmd could be
     * kicked); a non-NULL means the invariant broke upstream — count it, never
     * overwrite (that would strand a live spinner). RELEASE publishes sync_rc
     * to the claimer's ACQUIRE. in_flight stays set: the read-staging page is
     * copied out in ata_run_completion before the next kick. ASYNC fire-and-
     * forget cmds (no spinner) keep riding irq_defer, drained on a K-Core. */
    if (cmd->is_sync_wait) {
        if (__atomic_load_n(&aa->landing, __ATOMIC_RELAXED) != NULL)
            atomic_fetch_add_u64(&aa->landing_clobber, 1);
        else
            __atomic_store_n(&aa->landing, cmd, __ATOMIC_RELEASE);
        return NULL;
    }
    return cmd;   /* async — caller irq_defers after unlocking */
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

    /* A K-Core is SRST-recovering this channel: BMISR reads are meaningless
     * mid-reset and the drive's INTRQ is masked (nIEN=1), so any edge that
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
         * watchdog already reconciled this cmd and its bottom-half has yet
         * to move in_flight along) — clear drive latch + BMISR + stop
         * engine, then drop the line. */
        if (g_ata_channels[ch_idx].present) {
            (void)inb(g_ata_channels[ch_idx].cmd_base + ATA_REG_STATUS);
        }
        bmide_engine_stop(aa);
        atomic_fetch_add_u64(&aa->spurious_irqs, 1);
        spin_unlock(&aa->cmd_lock);
        return;
    }

    AtaCmd *defer = bmide_complete_locked(aa, bmsr);
    spin_unlock(&aa->cmd_lock);
    if (defer) irq_defer(ata_complete_deferred, defer);
}

static void ata_irq_handler(void) {
    if (!__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return;
    for (uint8_t ch = 0; ch < ATA_CHANNEL_COUNT; ch++) {
        ata_channel_irq_process(ch);
    }
}

/* ---------------------------------------------------------------------
 *  Watchdog — K-Core SRST recovery of a genuinely wedged channel.
 *
 *  Posted by bmide_watchdog_scan via the channel's never-drop recover_node;
 *  runs on a K-Core pump (StorageCompletionPump) where the up-to-2 s BSY wait
 *  in ata_channel_soft_reset is affordable. `recovering` is already 1 (the
 *  scan CAS'd it) and gates the IRQ handler / scan / submit off this channel
 *  for the duration.
 *
 *  Stack-cmd lifetime is the delicate part. The wedged sync cmd lives on its
 *  waiter's stack; the waiter spins until it observes cmd->done. If any other
 *  actor (a late IRQ, this worker) could still touch the cmd after the waiter
 *  returns, that is a use-after-free. The 2026-05-28 audit note flagged exactly
 *  this and proposed heap+refcount for every I/O; we avoid that hot-path cost
 *  with strict ORDERING instead: SRST silences the drive (nIEN held =1) so no
 *  late INTRQ can reference the cmd, we NULL in_flight and re-arm the channel,
 *  and only THEN — last of all — publish the wedged cmd's failure. The instant
 *  its waiter observes done, no live reference to the stack frame survives.
 * ------------------------------------------------------------------ */
static void ata_recover_worker(void *ctx) {
    AtaAsyncCh *aa     = (AtaAsyncCh *)ctx;
    uint8_t     ch_idx = aa->ch_idx;

    /* Belt-and-suspenders: the drive may have completed between the scan
     * flagging the wedge and this worker being pumped. If its INTRQ is now
     * latched, reconcile normally and skip the reset — no spurious ERR_IO. */
    spin_lock(&aa->cmd_lock);
    uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);
    AtaCmd *cmd  = aa->in_flight;
    if (cmd && !__atomic_load_n(&cmd->reconciled, __ATOMIC_RELAXED) &&
        (bmsr & BMISR_IRQ)) {
        AtaCmd *defer = bmide_complete_locked(aa, bmsr);
        __atomic_store_n(&aa->recovering, 0u, __ATOMIC_RELEASE);
        spin_unlock(&aa->cmd_lock);
        if (defer) irq_defer(ata_complete_deferred, defer);
        return;
    }
    spin_unlock(&aa->cmd_lock);

    /* Genuine wedge. SRST with nIEN held =1 throughout silences the drive's
     * INTRQ, so the wedged command can raise no further interrupt. */
    ata_channel_soft_reset(ch_idx);

    /* Retire the wedged cmd out of flight and re-launch the innocent queue
     * (the "re-launch waiting ships" recovery: commands queued behind the
     * wedged one were never issued to the drive, so a reset makes them
     * runnable again — failing them would spuriously punish untouched I/O).
     * All under one lock hold so a concurrent submit either lands in the queue
     * we re-kick from or is serialised behind recovering being cleared. */
    spin_lock(&aa->cmd_lock);
    AtaCmd *wedged = aa->in_flight;
    aa->in_flight  = NULL;
    __atomic_store_n(&aa->landing, NULL, __ATOMIC_RELAXED); /* no completion landed */
    ata_set_nien(ch_idx, false);            /* SRST left nIEN=1; re-arm the IRQ path */
    AtaCmd *next = queue_pop_head(aa);
    aa->in_flight = next;
    if (next) bmide_kick(aa, next);
    __atomic_store_n(&aa->recovering, 0u, __ATOMIC_RELEASE);
    spin_unlock(&aa->cmd_lock);

    /* Publish the wedged cmd's failure LAST — in_flight no longer points at it
     * and the drive is silenced+re-armed, so its (stack) storage is dead to
     * every other actor the instant its waiter observes done. */
    if (wedged) {
        wedged->sync_rc = ERR_IO;
        atomic_fetch_add_u64(&aa->cmds_failed, 1);
        aa->err_ev = (AtaErrEv){ .drive = wedged->drive_idx, .is_write = wedged->is_write,
                                 .count = wedged->count, .rc = ERR_IO, .lba = wedged->lba };
        irq_defer(ata_err_worker, aa);      /* best-effort telemetry, off this path */
        if (wedged->cb) {
            wedged->cb(wedged->drive_idx, ERR_IO, wedged->cb_ctx);
        }
        if (wedged->is_sync_wait) {
            __atomic_store_n(&wedged->done, 1u, __ATOMIC_RELEASE);
        } else {
            kfree(wedged);
        }
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
            AtaCmd *defer = bmide_complete_locked(aa, bmsr);
            spin_unlock(&aa->cmd_lock);
            if (defer) irq_defer(ata_complete_deferred, defer);
            continue;
        }

        /* No interrupt latched. Alternate Status (ctrl_reg) does NOT clear the
         * drive INTRQ latch, unlike the primary STATUS — safe to snoop. */
        uint8_t alt = inb(g_ata_channels[ch].ctrl_reg);
        bool wedge = false;
        if (alt == 0xFF)                                wedge = true;  /* 2a device gone */
        else if (!(bmsr & BMISR_ACTIVE))                wedge = true;  /* 2a engine quit, no INTRQ */
        else if (now - cmd->submit_tsc > liveness_tsc)  wedge = true;  /* 2b frozen engine */

        if (wedge && __sync_bool_compare_and_swap(&aa->recovering, 0u, 1u)) {
            spin_unlock(&aa->cmd_lock);
            /* Never-drop, allocation-free hand-off to a K-Core SRST — the
             * up-to-2 s reset cannot run in this IRQ (PIT) context. */
            StorageCompletionPush(&aa->recover_node);
        } else {
            spin_unlock(&aa->cmd_lock);
        }
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
    /* Queue (never kick) while a K-Core is SRST-recovering the channel: the
     * recover worker re-launches the queue head itself once the reset settles,
     * so kicking onto a mid-reset drive is structurally excluded. */
    if (aa->in_flight || __atomic_load_n(&aa->recovering, __ATOMIC_RELAXED)) {
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
    /* irq_defer pump only exists on multi-core (the BSP runs userspace on
     * single-core and never drains the ring). */
    if (g_amp.total_cores < 2) return false;
    extern volatile uint8_t g_irq_defer_ready;
    if (!__atomic_load_n(&g_irq_defer_ready, __ATOMIC_ACQUIRE)) return false;
    return true;
}

static error_t do_submit_async(uint8_t drive_idx, uint64_t lba, uint16_t count,
                               bool is_write, void *user_buf,
                               AtaAsyncCb cb, void *cb_ctx) {
    if (!cb) return ERR_INVALID_ARGUMENT;
    if (drive_idx >= ATA_DRIVE_COUNT) return ERR_INVALID_ARGUMENT;
    if (!user_buf || count == 0 || count > ATA_DMA_MAX_SECTORS)
        return ERR_INVALID_ARGUMENT;
    if (!ata_async_usable(drive_idx)) return ERR_DEVICE_NOT_READY;
    if (!async_path_viable())         return ERR_DEVICE_NOT_READY;

    ATADevice *d = &g_ata_devices[drive_idx];
    if (!d->exists) return ERR_DEVICE_NOT_READY;
    if (lba + count > d->total_sectors) return ERR_INVALID_ARGUMENT;

    AtaCmd *cmd = (AtaCmd *)kmalloc(sizeof(*cmd));
    if (!cmd) return ERR_IO;
    memset(cmd, 0, sizeof(*cmd));
    cmd->drive_idx    = drive_idx;
    cmd->channel      = drive_channel(drive_idx);
    cmd->lba          = lba;
    cmd->count        = count;
    cmd->is_write     = is_write;
    cmd->user_buf     = user_buf;
    cmd->cb           = cb;
    cmd->cb_ctx       = cb_ctx;
    cmd->is_sync_wait = false;
    cmd->sync_rc      = OK;

    error_t rc = submit_internal(cmd);
    if (rc != OK) {
        kfree(cmd);
    }
    return rc;
}

error_t ata_submit_read_async(uint8_t drive_idx, uint64_t lba,
                              uint16_t count, void *user_buf,
                              AtaAsyncCb cb, void *cb_ctx) {
    return do_submit_async(drive_idx, lba, count, false, user_buf, cb, cb_ctx);
}

error_t ata_submit_write_async(uint8_t drive_idx, uint64_t lba,
                               uint16_t count, const void *user_buf,
                               AtaAsyncCb cb, void *cb_ctx) {
    return do_submit_async(drive_idx, lba, count, true,
                           (void *)(uintptr_t)user_buf, cb, cb_ctx);
}

/* ---------------------------------------------------------------------
 *  Sync wrapper — submit + IRQ-wait, polled fallback when irq_defer down
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
    cmd.cb           = NULL;
    cmd.cb_ctx       = NULL;
    cmd.is_sync_wait = true;
    cmd.sync_rc      = OK;

    if (!async_path_viable()) {
        /* irq_defer / multi-core not ready — drive the channel polled,
         * with nIEN temporarily set so the IRQ path stays silent. */
        return bmide_transfer_polled(&cmd);
    }

    /* IRQ-driven path. submit, then wait on done via sti;hlt. */
    error_t srq = submit_internal(&cmd);
    if (srq != OK) {
        if (srq == ERR_BUSY)         return ATA_ERR_TIMEOUT;
        if (srq == ERR_INVALID_ARGUMENT) return ATA_ERR_INVALID_ARGS;
        return ATA_ERR_DRIVE_FAULT;
    }

    /* Wait loop. sti once and spin-pause — HLT would only wake on an IRQ
     * targeting THIS core, but the BMIDE IRQ is routed point-to-point to the
     * BSP via the IO-APIC; a K-Core caller would HLT forever. sti keeps IF=1
     * so the IRQ can reach the BSP and stamp `landing`.
     *
     * Never-drop delivery: the completion IRQ stamps this channel's `landing`
     * slot (the sync path no longer rides irq_defer), and we CLAIM it here via
     * an ACQ_REL XCHG and run the bottom-half ourselves. Because the lock-
     * holder drains its own completion, a BSP stuck spinning on a lock this
     * caller holds can never strand it — the textbook BMIDE × write_lock
     * deadlock is structurally impossible, with one atomic word instead of an
     * MPMC ring. The claimed `c` may be ANOTHER waiter's cmd on this channel
     * (we drain whatever landed, not necessarily our own) — correct and
     * necessary, exactly what the old irq_defer_pump(bsp) did.
     *
     * No deadline / no timeout: the stack `cmd` is valid until we observe
     * done; a wedged drive spins forever (orthogonal — the SRST watchdog is a
     * separate follow-up, and `landing` is the seam it will reuse). Healthy
     * drives complete in <1 ms. */
    AtaAsyncCh *aa = &g_ata_async[cmd.channel];
    asm volatile("sti" ::: "memory");
    while (!__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {
        AtaCmd *c = __atomic_exchange_n(&aa->landing, NULL, __ATOMIC_ACQ_REL);
        if (c) ata_run_completion(aa, c);
        cpu_pause();
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

    /* Watchdog SRST-recovery seam: node points back at this channel and runs
     * ata_recover_worker on a K-Core when bmide_watchdog_scan posts it. */
    aa->ch_idx           = ch_idx;
    aa->recovering       = 0;
    aa->recover_node.run  = ata_recover_worker;
    aa->recover_node.ctx  = aa;
    aa->recover_node.next = NULL;

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
void ata_async_negotiate_dma_mode(uint8_t drive_idx, const uint16_t *id) {
    ATADevice *dev = &g_ata_devices[drive_idx];
    dev->dma_type = ATA_DMA_TYPE_NONE;
    dev->dma_mode = 0;

    if (!(id[49] & (1u << 8))) return;  /* IDENTIFY word 49 bit 8 = DMA */

    if (id[53] & (1u << 2)) {
        uint16_t udma_sup = id[88] & 0x7Fu;
        for (int mode = 6; mode >= 0; mode--) {
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

    if (gsi0 != 0xFF && gsi0 < IRQ_MAX_COUNT) {
        irq_register_handler(gsi0, ata_irq_handler);
        irqchip_enable_irq(gsi0);
    }
    if (gsi1 != 0xFF && gsi1 < IRQ_MAX_COUNT && gsi1 != gsi0) {
        irq_register_handler(gsi1, ata_irq_handler);
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
    cmd.is_sync_wait = true;
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
            AtaCmd *c = __atomic_exchange_n(&aa->landing, NULL, __ATOMIC_ACQ_REL);
            if (c) ata_run_completion(aa, c);
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
 *  On-demand diagnostic — TIER-2 wedge -> K-Core SRST recovery, end-to-end.
 *
 *  QEMU's PIIX IDE completes reliably and cannot be made to hang, so the
 *  genuine-wedge path (bmide_watchdog_scan TIER-2 -> ata_recover_worker) has no
 *  natural trigger. This drives it deterministically: install an in-flight cmd
 *  for which NO DMA was ever issued — the drive will never signal, an exact
 *  "engine idle, no INTRQ" wedge — then let the real scan detect it (TIER-2a)
 *  and post SRST recovery to a K-Core, and assert the worker (a) failed the
 *  wedged cmd ERR_IO and (b) left the channel usable (a real read succeeds).
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
    fake.is_sync_wait = true;
    fake.sync_rc      = OK;
    fake.submit_tsc   = rdtsc();

    /* Install as in-flight WITHOUT issuing DMA — a wedge the drive never ends.
     * Only on a quiescent channel (no live traffic to disturb). */
    spin_lock(&aa->cmd_lock);
    bool installed = (aa->in_flight == NULL && !aa->recovering);
    if (installed) aa->in_flight = &fake;
    spin_unlock(&aa->cmd_lock);
    if (!installed) {
        kfree(vbuf);
        kprintf("[BMIDE-WD] TIER-2 self-test skipped (channel busy)\n");
        return OK;
    }

    /* Real detection path: the scan sees engine-idle-without-INTRQ (TIER-2a),
     * CASes recovering, and posts SRST recovery to the drain core's queue. */
    bmide_watchdog_scan();

    /* Bounded wait for ata_recover_worker to SRST + fail `fake` ERR_IO. The
     * recovery node is posted to the BSP drain queue, but at this boot stage the
     * BSP has not yet entered kcore_run_loop, so nothing drains it — we pump it
     * here ourselves (single-consumer safe: this runs on the BSP, which owns the
     * queue). This exercises the real push+pump+worker chain, not a direct call. */
    bool worker_ok = false;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(6000);
    while (rdtsc() < deadline) {
        StorageCompletionPump(g_amp.bsp_index);
        if (__atomic_load_n(&fake.done, __ATOMIC_ACQUIRE)) {
            worker_ok = (fake.sync_rc == ERR_IO);
            break;
        }
        cpu_pause();
    }

    /* Safety net: if the worker never completed (a regression), the fake would
     * be left in-flight with recovering stuck — its stack frame is about to die.
     * Undo it so boot continues cleanly and nothing dereferences a dead frame. */
    if (!worker_ok) {
        spin_lock(&aa->cmd_lock);
        if (aa->in_flight == &fake) { bmide_engine_stop(aa); aa->in_flight = NULL; }
        __atomic_store_n(&aa->landing, NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&aa->recovering, 0u, __ATOMIC_RELEASE);
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
