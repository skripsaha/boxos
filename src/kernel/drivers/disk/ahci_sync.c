#include "ahci_sync.h"
#include "ahci.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu_calibrate.h"
#include "klib.h"
#include "atomics.h"

int ahci_read_sectors_sync(uint8_t port, uint32_t lba,
                           uint16_t sector_count, void* buffer) {
    if (!buffer || sector_count == 0) return -1;

    ahci_port_t* port_state = ahci_get_port_state(port);
    if (!port_state) {
        debug_printf("[AHCI Sync] Invalid port %u\n", port);
        return -1;
    }

    /* Allocate a DMA-safe buffer (physical address guaranteed below 4GB from PMM) */
    uint32_t pages_needed = (sector_count * 512 + 4095) / 4096;
    if (pages_needed == 0) pages_needed = 1;
    void* dma_buffer = pmm_alloc(pages_needed);
    if (!dma_buffer) {
        debug_printf("[AHCI Sync] Failed to allocate DMA buffer for read\n");
        return -1;
    }
    memset(dma_buffer, 0, pages_needed * 4096);

    uintptr_t dma_phys = (uintptr_t)dma_buffer;

    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        int slot = ahci_alloc_slot(port);
        if (slot < 0) {
            debug_printf("[AHCI Sync] No free slots (retry %d/%d)\n", retry + 1, AHCI_MAX_RETRIES);
            continue;
        }

        port_state->stats.cmd_count++;

        error_t err = ahci_build_ncq_read(port, slot, lba, sector_count, (void*)dma_phys);
        if (err != OK) {
            ahci_free_slot(port, slot);
            pmm_free(dma_buffer, pages_needed);
            return -1;
        }

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        regs->sact |= (1U << slot);
        regs->ci |= (1U << slot);

        uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_CMD_DEFAULT);

        while (rdtsc() < timeout_tsc) {
            uint32_t ci = regs->ci;
            uint32_t sact = regs->sact;

            if (!(ci & (1U << slot)) && !(sact & (1U << slot))) {
                ahci_free_slot(port, slot);
                memcpy(buffer, dma_buffer, sector_count * 512);
                pmm_free(dma_buffer, pages_needed);
                return 0;
            }

            uint32_t is = regs->is;
            if (is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
                ahci_log_error(port_state, is);
                regs->is = is;
                ahci_free_slot(port, slot);

                if (retry < AHCI_MAX_RETRIES - 1) {
                    debug_printf("[AHCI Sync] Error on read, retrying (%d/%d)...\n", retry + 1, AHCI_MAX_RETRIES);
                    ahci_port_recover(port_state);
                }
                break;
            }

            cpu_pause();
        }

        if (rdtsc() >= timeout_tsc) {
            debug_printf("[AHCI Sync] Timeout on slot %d (retry %d/%d)\n", slot, retry + 1, AHCI_MAX_RETRIES);
            port_state->stats.timeout_count++;
            ahci_free_slot(port, slot);
            if (retry < AHCI_MAX_RETRIES - 1) {
                ahci_port_recover(port_state);
            }
        }
    }

    pmm_free(dma_buffer, pages_needed);
    debug_printf("[AHCI Sync] Read failed after %d retries\n", AHCI_MAX_RETRIES);
    return -1;
}

int ahci_write_sectors_sync(uint8_t port, uint32_t lba,
                            uint16_t sector_count, const void* buffer) {
    if (!buffer || sector_count == 0) return -1;

    ahci_port_t* port_state = ahci_get_port_state(port);
    if (!port_state) {
        debug_printf("[AHCI Sync] Invalid port %u\n", port);
        return -1;
    }

    /* Allocate a DMA-safe buffer and copy data into it */
    uint32_t pages_needed = (sector_count * 512 + 4095) / 4096;
    if (pages_needed == 0) pages_needed = 1;
    void* dma_buffer = pmm_alloc(pages_needed);
    if (!dma_buffer) {
        debug_printf("[AHCI Sync] Failed to allocate DMA buffer for write\n");
        return -1;
    }
    memcpy(dma_buffer, buffer, sector_count * 512);
    mfence();

    uintptr_t dma_phys = (uintptr_t)dma_buffer;

    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        int slot = ahci_alloc_slot(port);
        if (slot < 0) {
            debug_printf("[AHCI Sync] No free slots (retry %d/%d)\n", retry + 1, AHCI_MAX_RETRIES);
            continue;
        }

        port_state->stats.cmd_count++;

        error_t err = ahci_build_ncq_write(port, slot, lba, sector_count, (void*)dma_phys);
        if (err != OK) {
            ahci_free_slot(port, slot);
            pmm_free(dma_buffer, pages_needed);
            return -1;
        }

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        regs->sact |= (1U << slot);
        regs->ci |= (1U << slot);

        uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_CMD_DEFAULT);

        while (rdtsc() < timeout_tsc) {
            if (!(regs->ci & (1U << slot)) && !(regs->sact & (1U << slot))) {
                ahci_free_slot(port, slot);
                pmm_free(dma_buffer, pages_needed);
                return 0;
            }

            uint32_t is = regs->is;
            if (is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
                ahci_log_error(port_state, is);
                regs->is = is;
                ahci_free_slot(port, slot);

                if (retry < AHCI_MAX_RETRIES - 1) {
                    debug_printf("[AHCI Sync] Error on write, retrying (%d/%d)...\n", retry + 1, AHCI_MAX_RETRIES);
                    ahci_port_recover(port_state);
                }
                break;
            }

            cpu_pause();
        }

        if (rdtsc() >= timeout_tsc) {
            debug_printf("[AHCI Sync] Timeout on slot %d (retry %d/%d)\n", slot, retry + 1, AHCI_MAX_RETRIES);
            port_state->stats.timeout_count++;
            ahci_free_slot(port, slot);
            if (retry < AHCI_MAX_RETRIES - 1) {
                ahci_port_recover(port_state);
            }
        }
    }

    pmm_free(dma_buffer, pages_needed);
    debug_printf("[AHCI Sync] Write failed after %d retries\n", AHCI_MAX_RETRIES);
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
        cmdfis->command = 0xEA; /* FLUSH CACHE EXT (48-bit LBA) */
        cmdfis->device = 0;

        mfence();

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        regs->ci |= (1U << slot);

        uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_CMD_DEFAULT);

        while (rdtsc() < timeout_tsc) {
            if (!(regs->ci & (1U << slot))) {
                ahci_free_slot(port, slot);
                return 0;
            }

            uint32_t is = regs->is;
            if (is & (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)) {
                ahci_log_error(port_state, is);
                regs->is = is;
                ahci_free_slot(port, slot);
                if (retry < AHCI_MAX_RETRIES - 1) {
                    ahci_port_recover(port_state);
                }
                break;
            }

            cpu_pause();
        }

        if (rdtsc() >= timeout_tsc) {
            debug_printf("[AHCI Sync] Flush cache timeout (retry %d/%d)\n", retry + 1, AHCI_MAX_RETRIES);
            ahci_free_slot(port, slot);
            if (retry < AHCI_MAX_RETRIES - 1) {
                ahci_port_recover(port_state);
            }
        }
    }

    debug_printf("[AHCI Sync] Flush cache failed after %d retries\n", AHCI_MAX_RETRIES);
    return -1;
}
