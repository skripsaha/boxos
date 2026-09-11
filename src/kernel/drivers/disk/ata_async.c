
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
#include "logbook.h"
#include "idt.h"
#include "irqchip.h"
#include "touch.h"
#include "baton.h"

#define BMIDE_REG_CMD       0
#define BMIDE_REG_STATUS    2
#define BMIDE_REG_PRD       4

#define BMICR_START         0x01
#define BMICR_READ_FROM_DISK 0x08

#define BMISR_ACTIVE        0x01
#define BMISR_ERROR         0x02
#define BMISR_IRQ           0x04

#define ATA_PRD_EOT         0x8000

#define ATA_DMA_MAX_SECTORS ATA_ASYNC_MAX_SECTORS
#define ATA_CMD_QUEUE_CAP   128

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

typedef struct AtaCmd {
    uint8_t        drive_idx;
    uint8_t        channel;
    uint16_t       count;
    uint64_t       lba;
    bool           is_write;
    void          *user_buf;

    volatile uint32_t done;
    volatile error_t  sync_rc;
    uint64_t       submit_tsc;

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
    volatile uint64_t spurious_irqs;

    AtaCmd   *volatile landing;
    volatile uint64_t  landing_clobber;
    volatile uint8_t   clobber_warned;

    uint8_t            ch_idx;
    volatile uint32_t  recovering;
    Baton  recover_node;
} AtaAsyncCh;

static AtaAsyncCh g_ata_async[ATA_CHANNEL_COUNT];
static volatile uint8_t g_ata_async_ready = 0;
static volatile uint8_t g_ata_irq_armed   = 0;

static __attribute__((aligned(4096), section(".bss")))
    uint8_t g_prd_storage[ATA_CHANNEL_COUNT][4096];

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

static void ata_set_nien(uint8_t channel, bool masked) {
    AtaChannel *ch = &g_ata_channels[channel];
    if (!ch->present) return;
    outb(ch->ctrl_reg, masked ? ATA_CTRL_nIEN : 0);
    for (int i = 0; i < 4; i++) (void)inb(ch->ctrl_reg);
}

extern int ata_set_xfer_mode(uint8_t drive_idx, uint8_t mode_byte);

extern int ata_program_lba(uint8_t drive_idx, uint64_t lba, uint16_t count,
                           uint8_t cmd28, uint8_t cmd48);

static void bmide_engine_stop(AtaAsyncCh *aa) {
    outb(aa->bmide_base + BMIDE_REG_CMD,    0);
    outb(aa->bmide_base + BMIDE_REG_STATUS, BMISR_ERROR | BMISR_IRQ);
}

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

    bmide_engine_stop(aa);
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

static AtaCmd *queue_pop_head(AtaAsyncCh *aa) {
    AtaCmd *c = aa->wait_head;
    if (!c) return NULL;
    aa->wait_head = c->next;
    if (!aa->wait_head) aa->wait_tail = NULL;
    c->next = NULL;
    aa->queue_depth--;
    return c;
}


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

static void ata_run_completion(AtaAsyncCh *aa, AtaCmd *cmd) {
    if (!cmd->is_write && cmd->sync_rc == OK && cmd->user_buf) {
        memcpy(cmd->user_buf, aa->buf_virt, (uint32_t)cmd->count * ATA_SECTOR_SIZE);
    }

    spin_lock(&aa->cmd_lock);
    AtaCmd *next = queue_pop_head(aa);
    aa->in_flight = next;
    if (next) bmide_kick(aa, next);
    spin_unlock(&aa->cmd_lock);

    if (cmd->sync_rc != OK) {
        atomic_fetch_add_u64(&aa->cmds_failed, 1);
        touch_publish_error(cmd->drive_idx, cmd->sync_rc, cmd->lba,
                            cmd->count, cmd->is_write);
    }
    atomic_fetch_add_u64(&aa->cmds_completed, 1);

    __atomic_store_n(&cmd->done, 1u, __ATOMIC_RELEASE);
}

static void bmide_complete_locked(AtaAsyncCh *aa, uint8_t bmsr) {
    AtaCmd *cmd = aa->in_flight;

    uint8_t st = ata_read_status(cmd->drive_idx);

    error_t rc = OK;
    if (st == 0xFF)                          rc = ERR_DEVICE_NOT_READY;
    else if (st & ATA_SR_ERR)                rc = ERR_IO;
    else if (st & ATA_SR_DF)                 rc = ERR_IO;
    else if (bmsr & BMISR_ERROR)             rc = ERR_IO;

    bmide_engine_stop(aa);
    mfence();

    cmd->sync_rc = rc;
    __atomic_store_n(&cmd->reconciled, 1u, __ATOMIC_RELAXED);

    if (__atomic_load_n(&aa->landing, __ATOMIC_RELAXED) != NULL)
        atomic_fetch_add_u64(&aa->landing_clobber, 1);
    else
        __atomic_store_n(&aa->landing, cmd, __ATOMIC_RELEASE);
}

static void ata_channel_irq_process(uint8_t ch_idx) {
    AtaAsyncCh *aa = &g_ata_async[ch_idx];
    if (!aa->enabled) return;

    spin_lock(&aa->cmd_lock);

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

static void ata_recover_worker(void *ctx) {
    AtaAsyncCh *aa     = (AtaAsyncCh *)ctx;
    uint8_t     ch_idx = aa->ch_idx;

    spin_lock(&aa->cmd_lock);
    uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);
    AtaCmd *cmd  = aa->in_flight;
    if (cmd && !__atomic_load_n(&cmd->reconciled, __ATOMIC_RELAXED) &&
        (bmsr & BMISR_IRQ)) {
        bmide_complete_locked(aa, bmsr);
        __atomic_store_n(&aa->recovering, 0u, __ATOMIC_RELEASE);
        spin_unlock(&aa->cmd_lock);
        return;
    }
    spin_unlock(&aa->cmd_lock);

    ata_channel_soft_reset(ch_idx);

    spin_lock(&aa->cmd_lock);
    AtaCmd *wedged = aa->in_flight;
    aa->in_flight  = NULL;
    __atomic_store_n(&aa->landing, NULL, __ATOMIC_RELAXED);
    ata_set_nien(ch_idx, false);
    AtaCmd *next = queue_pop_head(aa);
    aa->in_flight = next;
    if (next) bmide_kick(aa, next);
    __atomic_store_n(&aa->recovering, 0u, __ATOMIC_RELEASE);
    spin_unlock(&aa->cmd_lock);

    if (wedged) {
        wedged->sync_rc = ERR_IO;
        atomic_fetch_add_u64(&aa->cmds_failed, 1);
        touch_publish_error(wedged->drive_idx, ERR_IO, wedged->lba,
                            wedged->count, wedged->is_write);
        __atomic_store_n(&wedged->done, 1u, __ATOMIC_RELEASE);
    }
}

void bmide_watchdog_scan(void) {
    if (!__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return;
    if (g_amp.total_cores < 2) return;

    uint64_t now          = rdtsc();
    uint64_t liveness_tsc  = cpu_ms_to_tsc(CONFIG_ATA_LIVENESS_MS);

    for (uint8_t ch = 0; ch < ATA_CHANNEL_COUNT; ch++) {
        AtaAsyncCh *aa = &g_ata_async[ch];
        if (!aa->enabled) continue;
        if (__atomic_load_n(&aa->recovering, __ATOMIC_ACQUIRE)) continue;
        if (!__atomic_load_n(&aa->in_flight, __ATOMIC_RELAXED)) continue;

        if (!spin_trylock(&aa->cmd_lock)) continue;

        AtaCmd *cmd = aa->in_flight;
        if (!cmd || __atomic_load_n(&cmd->reconciled, __ATOMIC_RELAXED)) {
            spin_unlock(&aa->cmd_lock);
            continue;
        }

        uint8_t bmsr = inb(aa->bmide_base + BMIDE_REG_STATUS);

        if (bmsr & BMISR_IRQ) {
            bmide_complete_locked(aa, bmsr);
            spin_unlock(&aa->cmd_lock);
            continue;
        }

        uint8_t alt = inb(g_ata_channels[ch].ctrl_reg);
        bool wedge = false;
        if (alt == 0xFF)                                wedge = true;
        else if (!(bmsr & BMISR_ACTIVE))                wedge = true;
        else if (now - cmd->submit_tsc > liveness_tsc)  wedge = true;

        if (wedge && __sync_bool_compare_and_swap(&aa->recovering, 0u, 1u)) {
            spin_unlock(&aa->cmd_lock);
            BatonPass(&aa->recover_node);
        } else {
            spin_unlock(&aa->cmd_lock);
        }
    }
}


static error_t submit_internal(AtaCmd *cmd) {
    uint8_t      ch_idx = cmd->channel;
    AtaAsyncCh  *aa     = &g_ata_async[ch_idx];

    spin_lock(&aa->cmd_lock);
    if (aa->queue_depth >= ATA_CMD_QUEUE_CAP) {
        spin_unlock(&aa->cmd_lock);
        return ERR_BUSY;
    }
    if (aa->in_flight || __atomic_load_n(&aa->recovering, __ATOMIC_RELAXED)) {
        queue_push_tail(aa, cmd);
        atomic_fetch_add_u64(&aa->cmds_submitted, 1);
        spin_unlock(&aa->cmd_lock);
        return OK;
    }
    aa->in_flight = cmd;
    atomic_fetch_add_u64(&aa->cmds_submitted, 1);
    bmide_kick(aa, cmd);
    spin_unlock(&aa->cmd_lock);
    return OK;
}

static bool async_path_viable(void) {
    if (!__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return false;
    if (!__atomic_load_n(&g_ata_irq_armed,   __ATOMIC_ACQUIRE)) return false;
    if (g_amp.total_cores < 2) return false;
    return true;
}

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
        return bmide_transfer_polled(&cmd);
    }

    error_t srq = submit_internal(&cmd);
    if (srq != OK) {
        if (srq == ERR_BUSY)         return ATA_ERR_TIMEOUT;
        if (srq == ERR_INVALID_ARGUMENT) return ATA_ERR_INVALID_ARGS;
        return ATA_ERR_DRIVE_FAULT;
    }

    AtaAsyncCh *aa = &g_ata_async[cmd.channel];
    asm volatile("sti" ::: "memory");
    while (!__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {
        AtaCmd *c = __atomic_exchange_n(&aa->landing, NULL, __ATOMIC_ACQ_REL);
        if (c) ata_run_completion(aa, c);
        cpu_pause();
    }

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

    aa->ch_idx           = ch_idx;
    aa->recovering       = 0;
    aa->recover_node.run  = ata_recover_worker;
    aa->recover_node.ctx  = aa;
    aa->recover_node.next = NULL;

    bmide_engine_stop(aa);

    aa->enabled = true;
    debug_printf("[ATA_ASYNC] ch%u BMIDE up — bm_base=0x%04x prd_phys=0x%08lx "
                 "buf_phys=0x%08lx\n",
                 ch_idx, aa->bmide_base,
                 (unsigned long)aa->prd_phys, (unsigned long)aa->buf_phys);
}

void ata_async_negotiate_dma_mode(uint8_t drive_idx, const uint16_t *id) {
    ATADevice *dev = &g_ata_devices[drive_idx];
    dev->dma_type = ATA_DMA_TYPE_NONE;
    dev->dma_mode = 0;

    if (!(id[49] & (1u << 8))) return;

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

void ata_async_init(void) {
    if (__atomic_load_n(&g_ata_async_ready, __ATOMIC_ACQUIRE)) return;

    memset(g_ata_async, 0, sizeof(g_ata_async));

    {
        TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
        TouchLogbookResolve("storage:ata:error", &full, &bare);
        __atomic_store_n(&g_ata_err_full, full, __ATOMIC_RELEASE);
        __atomic_store_n(&g_ata_err_bare, bare, __ATOMIC_RELEASE);
    }

    if (ahci_is_initialized()) {
        debug_printf("[ATA_ASYNC] AHCI active — BMIDE async engine skipped\n");
        __atomic_store_n(&g_ata_async_ready, 1u, __ATOMIC_RELEASE);
        return;
    }

    for (uint8_t c = 0; c < ATA_CHANNEL_COUNT; c++) {
        bmide_init_channel(c);
    }

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

    for (uint8_t c = 0; c < ATA_CHANNEL_COUNT; c++) {
        if (g_ata_async[c].enabled) ata_set_nien(c, false);
    }

    __atomic_store_n(&g_ata_irq_armed,   1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ata_async_ready, 1u, __ATOMIC_RELEASE);

    debug_printf("[ATA_ASYNC] ready — ch0=%u ch1=%u gsi0=%u gsi1=%u\n",
                 g_ata_async[0].enabled, g_ata_async[1].enabled,
                 gsi0, gsi1);
}

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
    cmd.lba          = 0;
    cmd.count        = 1;
    cmd.is_write     = false;
    cmd.user_buf     = buf;
    cmd.sync_rc      = OK;

    irqchip_disable_irq(gsi);

    error_t sub = submit_internal(&cmd);
    bool ok = false;
    if (sub == OK) {
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(3000);
        while (rdtsc() < deadline) {
            bmide_watchdog_scan();
            AtaCmd *c = __atomic_exchange_n(&aa->landing, NULL, __ATOMIC_ACQ_REL);
            if (c) ata_run_completion(aa, c);
            if (__atomic_load_n(&cmd.done, __ATOMIC_ACQUIRE)) {
                ok = (cmd.sync_rc == OK);
                break;
            }
            cpu_pause();
        }
    }

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

    spin_lock(&aa->cmd_lock);
    bool installed = (aa->in_flight == NULL && !aa->recovering);
    if (installed) aa->in_flight = &fake;
    spin_unlock(&aa->cmd_lock);
    if (!installed) {
        kfree(vbuf);
        kprintf("[BMIDE-WD] TIER-2 self-test skipped (channel busy)\n");
        return OK;
    }

    bmide_watchdog_scan();

    bool worker_ok = false;
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(6000);
    while (rdtsc() < deadline) {
        BatonPump(g_amp.bsp_index);
        if (__atomic_load_n(&fake.done, __ATOMIC_ACQUIRE)) {
            worker_ok = (fake.sync_rc == ERR_IO);
            break;
        }
        cpu_pause();
    }

    if (!worker_ok) {
        spin_lock(&aa->cmd_lock);
        if (aa->in_flight == &fake) { bmide_engine_stop(aa); aa->in_flight = NULL; }
        __atomic_store_n(&aa->landing, NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&aa->recovering, 0u, __ATOMIC_RELEASE);
        spin_unlock(&aa->cmd_lock);
    }

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
#endif