#include "ahci_sync.h"
#include "ahci.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu_calibrate.h"
#include "klib.h"
#include "atomics.h"

#define AHCI_CMD_PATIENCE_MS  CONFIG_AHCI_IO_TIMEOUT_MS


typedef enum {
    AHCI_SLOT_DONE,
    AHCI_SLOT_FAULTED,
    AHCI_SLOT_GONE,
    AHCI_SLOT_SILENT
} AhciSlotEnd;

static AhciSlotEnd ahci_wait_for_slot(ahci_port_t* port_state,
                                      volatile ahci_port_regs_t* regs,
                                      uint8_t slot, bool queued)
{
    uint64_t give_up_at = rdtsc() + cpu_ms_to_tsc(AHCI_CMD_PATIENCE_MS);

    for (;;) {
        uint32_t ci   = regs->ci;
        uint32_t sact = queued ? regs->sact : 0;
        if (!(ci & (1U << slot)) && !(sact & (1U << slot))) {
            return AHCI_SLOT_DONE;
        }

        uint32_t is = regs->is;
        if (is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
            ahci_log_error(port_state, is);
            regs->is = is;
            return AHCI_SLOT_FAULTED;
        }

        if ((regs->ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PRESENT) {
            kprintf("[AHCI] port %u: the link is no longer established "
                    "(SSTS=0x%08x) — the command on slot %u has nobody to "
                    "answer it\n", port_state->port_num, regs->ssts, slot);
            return AHCI_SLOT_GONE;
        }

        if ((int64_t)(rdtsc() - give_up_at) >= 0) {
            return AHCI_SLOT_SILENT;
        }
        cpu_pause();
    }
}

int ahci_read_sectors_sync(uint8_t port, uint64_t lba,
                           uint16_t sector_count, void* buffer) {
    if (!buffer || sector_count == 0) return -1;

    ahci_port_t* port_state = ahci_get_port_state(port);
    if (!port_state) {
        debug_printf("[AHCI Sync] Invalid port %u\n", port);
        return -1;
    }

    uint32_t pages_needed = (sector_count * 512 + 4095) / 4096;
    if (pages_needed == 0) pages_needed = 1;
    void* dma_page = pmm_alloc(pages_needed, PHYS_TAG_DMA32);
    if (!dma_page) {
        debug_printf("[AHCI Sync] Failed to allocate DMA buffer for read\n");
        return -1;
    }
    uintptr_t dma_phys = (uintptr_t)dma_page;
    void* dma_virt = vmm_phys_to_virt(dma_phys);
    memset(dma_virt, 0, pages_needed * 4096);

    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        int slot = ahci_alloc_slot(port);
        if (slot < 0) {
            debug_printf("[AHCI Sync] No free slots (retry %d/%d)\n", retry + 1, AHCI_MAX_RETRIES);
            continue;
        }

        __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);

        error_t err = ahci_build_io(port, slot, lba, sector_count, (void*)dma_phys, false);
        if (err != OK) {
            ahci_free_slot(port, slot);
            pmm_free(dma_page, pages_needed);
            return -1;
        }

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        spin_lock(&port_state->lock);
        if (port_state->ncq) {
            regs->sact = (1U << slot);
        }
        regs->ci = (1U << slot);
        spin_unlock(&port_state->lock);

        AhciSlotEnd how = ahci_wait_for_slot(port_state, regs, (uint8_t)slot,
                                             port_state->ncq);
        ahci_free_slot(port, slot);

        if (how == AHCI_SLOT_DONE) {
            memcpy(buffer, dma_virt, sector_count * 512);
            pmm_free(dma_page, pages_needed);
            return 0;
        }

        if (how == AHCI_SLOT_GONE) {
            break;
        }

        if (how == AHCI_SLOT_SILENT) {
            kprintf("[AHCI] port %u: the disk did not answer a read in %u ms "
                    "(attempt %d of %d)\n", port, AHCI_CMD_PATIENCE_MS,
                    retry + 1, AHCI_MAX_RETRIES);
            __atomic_fetch_add(&port_state->stats.timeout_count, 1, __ATOMIC_RELAXED);
        } else {
            debug_printf("[AHCI Sync] Error on read, retrying (%d/%d)...\n",
                         retry + 1, AHCI_MAX_RETRIES);
        }
        if (retry < AHCI_MAX_RETRIES - 1) {
            ahci_port_recover(port_state);
        }
    }

    pmm_free(dma_page, pages_needed);
    kprintf("[AHCI] port %u: a read failed after %d attempts\n",
            port, AHCI_MAX_RETRIES);
    return -1;
}

int ahci_write_sectors_sync(uint8_t port, uint64_t lba,
                            uint16_t sector_count, const void* buffer) {
    if (!buffer || sector_count == 0) return -1;

    ahci_port_t* port_state = ahci_get_port_state(port);
    if (!port_state) {
        debug_printf("[AHCI Sync] Invalid port %u\n", port);
        return -1;
    }

    uint32_t pages_needed = (sector_count * 512 + 4095) / 4096;
    if (pages_needed == 0) pages_needed = 1;
    void* dma_page = pmm_alloc(pages_needed, PHYS_TAG_DMA32);
    if (!dma_page) {
        debug_printf("[AHCI Sync] Failed to allocate DMA buffer for write\n");
        return -1;
    }
    uintptr_t dma_phys = (uintptr_t)dma_page;
    void* dma_virt = vmm_phys_to_virt(dma_phys);
    memcpy(dma_virt, buffer, sector_count * 512);
    mfence();

    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        int slot = ahci_alloc_slot(port);
        if (slot < 0) {
            debug_printf("[AHCI Sync] No free slots (retry %d/%d)\n", retry + 1, AHCI_MAX_RETRIES);
            continue;
        }

        __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);

        error_t err = ahci_build_io(port, slot, lba, sector_count, (void*)dma_phys, true);
        if (err != OK) {
            ahci_free_slot(port, slot);
            pmm_free(dma_page, pages_needed);
            return -1;
        }

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        spin_lock(&port_state->lock);
        if (port_state->ncq) {
            regs->sact = (1U << slot);
        }
        regs->ci = (1U << slot);
        spin_unlock(&port_state->lock);

        AhciSlotEnd how = ahci_wait_for_slot(port_state, regs, (uint8_t)slot,
                                             port_state->ncq);
        ahci_free_slot(port, slot);

        if (how == AHCI_SLOT_DONE) {
            pmm_free(dma_page, pages_needed);
            return 0;
        }
        if (how == AHCI_SLOT_GONE) {
            break;
        }
        if (how == AHCI_SLOT_SILENT) {
            kprintf("[AHCI] port %u: the disk did not answer a write in %u ms "
                    "(attempt %d of %d)\n", port, AHCI_CMD_PATIENCE_MS,
                    retry + 1, AHCI_MAX_RETRIES);
            __atomic_fetch_add(&port_state->stats.timeout_count, 1, __ATOMIC_RELAXED);
        } else {
            debug_printf("[AHCI Sync] Error on write, retrying (%d/%d)...\n",
                         retry + 1, AHCI_MAX_RETRIES);
        }
        if (retry < AHCI_MAX_RETRIES - 1) {
            ahci_port_recover(port_state);
        }
    }

    pmm_free(dma_page, pages_needed);
    kprintf("[AHCI] port %u: a write failed after %d attempts\n",
            port, AHCI_MAX_RETRIES);
    return -1;
}

int ahci_flush_cache_sync(uint8_t port) {
    ahci_port_t* port_state = ahci_get_port_state(port);
    if (!port_state) {
        return -1;
    }

    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        int slot = ahci_alloc_slot(port);
        if (slot < 0) {
            continue;
        }

        ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port_state->clb_virt;
        cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
        cmdheader[slot].w = 0;
        cmdheader[slot].prdtl = 0;
        cmdheader[slot].prdbc = 0;

        ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port_state->ctba_virt[slot];
        memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

        fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
        memset(cmdfis, 0, sizeof(fis_reg_h2d_t));
        cmdfis->fis_type = FIS_TYPE_REG_H2D;
        cmdfis->c = 1;
        cmdfis->command = ATA_CMD_FLUSH_CACHE_EXT;
        cmdfis->device = 0;

        mfence();

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        spin_lock(&port_state->lock);
        regs->ci = (1U << slot);
        spin_unlock(&port_state->lock);

        AhciSlotEnd how = ahci_wait_for_slot(port_state, regs, (uint8_t)slot,
                                             false);
        ahci_free_slot(port, slot);

        if (how == AHCI_SLOT_DONE) {
            return 0;
        }
        if (how == AHCI_SLOT_GONE) {
            break;
        }
        if (how == AHCI_SLOT_SILENT) {
            kprintf("[AHCI] port %u: the disk did not answer a cache flush in "
                    "%u ms (attempt %d of %d)\n", port, AHCI_CMD_PATIENCE_MS,
                    retry + 1, AHCI_MAX_RETRIES);
        }
        if (retry < AHCI_MAX_RETRIES - 1) {
            ahci_port_recover(port_state);
        }
    }

    kprintf("[AHCI] port %u: a cache flush failed after %d attempts\n",
            port, AHCI_MAX_RETRIES);
    return -1;
}

uint32_t ahci_max_run_sectors(uint8_t port)
{
    (void)port;
    return 256u;
}