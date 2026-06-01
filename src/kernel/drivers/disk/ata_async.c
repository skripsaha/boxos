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
#include "pmtag.h"
#include "vmm.h"
#include "boxos_memory.h"
#include "ahci.h"
#include "amp.h"
#include "irq_defer.h"
#include "idt.h"
#include "irqchip.h"
#include "touch.h"

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
    volatile uint64_t spurious_irqs;
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

static void ata_complete_deferred(void *ctx) {
    AtaCmd *cmd = (AtaCmd *)ctx;
    if (!cmd) return;

    AtaAsyncCh *aa = &g_ata_async[cmd->channel];

    /* Read path: copy DMA staging into the caller's buffer. The staging
     * page is only safe to read between IRQ and "next kick" — the kick
     * is sequenced below under cmd_lock, so this memcpy must precede it. */
    if (!cmd->is_write && cmd->sync_rc == OK && cmd->user_buf) {
        memcpy(cmd->user_buf, aa->buf_virt, (uint32_t)cmd->count * ATA_SECTOR_SIZE);
    }

    /* Pull the next queued cmd and kick the engine on it. Holding
     * cmd_lock across the BMIDE register writes prevents an in-flight
     * IRQ-side completion (next cmd may complete very fast on an SSD)
     * from racing on the same registers. */
    spin_lock(&aa->cmd_lock);
    AtaCmd *next = queue_pop_head(aa);
    aa->in_flight = next;
    if (next) bmide_kick(aa, next);
    spin_unlock(&aa->cmd_lock);

    /* Now run the completed cmd's bottom-half. */
    if (cmd->sync_rc != OK) {
        atomic_fetch_add_u64(&aa->cmds_failed, 1);
        touch_publish_error(cmd->drive_idx, cmd->sync_rc,
                            cmd->lba, cmd->count, cmd->is_write);
    }
    atomic_fetch_add_u64(&aa->cmds_completed, 1);

    if (cmd->cb) {
        cmd->cb(cmd->drive_idx, cmd->sync_rc, cmd->cb_ctx);
    }

    if (cmd->is_sync_wait) {
        /* Caller owns the cmd struct (stack-allocated). Just publish
         * done; sti;hlt loop will wake on the next IRQ tick. */
        __atomic_store_n(&cmd->done, 1u, __ATOMIC_RELEASE);
    } else {
        kfree(cmd);
    }
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

    uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);
    if (!(bmsr & BMISR_IRQ)) {
        atomic_fetch_add_u64(&aa->spurious_irqs, 1);
        spin_unlock(&aa->cmd_lock);
        return;
    }

    AtaCmd *cmd = aa->in_flight;
    if (!cmd) {
        /* IRQ but nothing in flight — clear drive latch + BMISR + stop
         * engine, then drop the line. */
        if (g_ata_channels[ch_idx].present) {
            (void)inb(g_ata_channels[ch_idx].cmd_base + ATA_REG_STATUS);
        }
        bmide_engine_stop(aa);
        atomic_fetch_add_u64(&aa->spurious_irqs, 1);
        spin_unlock(&aa->cmd_lock);
        return;
    }

    /* ATA-7 §6.2.5: reading STATUS clears the drive-side IRQ latch. */
    uint8_t st = ata_read_status(cmd->drive_idx);

    error_t rc = OK;
    if (st == 0xFF)                          rc = ERR_DEVICE_NOT_READY;
    else if (st & ATA_SR_ERR)                rc = ERR_IO;
    else if (st & ATA_SR_DF)                 rc = ERR_IO;
    else if (bmsr & BMISR_ERROR)             rc = ERR_IO;

    /* W1C the BMIDE latches and stop the engine — required before the
     * next kick can re-program BMICR/BMIDT per Intel BMIDE §3.1. */
    bmide_engine_stop(aa);
    mfence();

    cmd->sync_rc = rc;
    spin_unlock(&aa->cmd_lock);

    /* Defer the heavy bottom-half (memcpy + cb + Touch publish + kfree)
     * to K-Core context. The IDT dispatcher EOIs on return. */
    irq_defer(ata_complete_deferred, cmd);
}

static void ata_irq_handler(void) {
    if (!__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return;
    for (uint8_t ch = 0; ch < ATA_CHANNEL_COUNT; ch++) {
        ata_channel_irq_process(ch);
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
    if (aa->in_flight) {
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

    /* Wait loop. sti once and spin-pause — HLT would only wake on an
     * IRQ targeting THIS core, but the BMIDE IRQ is routed point-to-
     * point to the BSP via the IO-APIC; a K-Core caller would HLT
     * forever waiting for an IRQ that never arrives on its LAPIC.
     *
     * Pump BOTH the local ring AND the BSP's ring. The BMIDE completion
     * is irq_defer'd onto whichever core received the IRQ — for legacy
     * GSI 14 routing that is the BSP. Now that irq_defer is MPMC, the
     * caller drains it directly without waiting for the BSP to fall out
     * of whatever it's doing (a critical fix for the case where the BSP
     * itself is spinning on a lock held by this caller — the textbook
     * BMIDE × OFE write_lock deadlock).
     *
     * No deadline / no timeout return: the stack-allocated `cmd` becomes
     * invalid the moment this function returns, but the IRQ-deferred
     * completion holds a pointer to it and may not have run yet. The
     * canonical recovery for a wedged BMIDE channel is a watchdog-
     * driven SRST reset (future work); until that lands, sync I/O
     * blocks until the drive answers. Healthy drives complete in <1 ms
     * so this is not observable in production. */
    uint8_t self_core = amp_get_core_index();
    uint8_t bsp_core  = g_amp.bsp_index;
    asm volatile("sti" ::: "memory");
    while (!__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {
        irq_defer_pump(self_core);
        if (bsp_core != self_core) irq_defer_pump(bsp_core);
        cpu_pause();
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

    void *page_phys = PhysAllocTagged(1, PHYS_TAG_DMA32);
    if (!page_phys) {
        debug_printf("[ATA_ASYNC] ch%u: no DMA32 page, DMA disabled\n", ch_idx);
        return;
    }
    aa->buf_phys = (uintptr_t)page_phys;
    aa->buf_virt = vmm_phys_to_virt(aa->buf_phys);
    if (!aa->buf_virt) {
        PhysAllocTaggedFree(page_phys, 1);
        debug_printf("[ATA_ASYNC] ch%u: cannot map staging page, DMA disabled\n", ch_idx);
        return;
    }

    spinlock_init(&aa->cmd_lock);
    aa->in_flight    = NULL;
    aa->wait_head    = NULL;
    aa->wait_tail    = NULL;
    aa->queue_depth  = 0;

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
