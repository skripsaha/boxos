#include "ahci_sync.h"
#include "ahci.h"
#include "vmm.h"
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

    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        int slot = ahci_alloc_slot(port);
        if (slot < 0) {
            debug_printf("[AHCI Sync] No free slots (retry %d/%d)\n", retry + 1, AHCI_MAX_RETRIES);
            continue;
        }

        port_state->stats.cmd_count++;

        uintptr_t buffer_phys = (uintptr_t)buffer;
        boxos_error_t err = ahci_build_ncq_read(port, slot, lba, sector_count, (void*)buffer_phys);
        if (err != BOXOS_OK) {
            ahci_free_slot(port, slot);
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

    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        int slot = ahci_alloc_slot(port);
        if (slot < 0) {
            debug_printf("[AHCI Sync] No free slots (retry %d/%d)\n", retry + 1, AHCI_MAX_RETRIES);
            continue;
        }

        port_state->stats.cmd_count++;

        uintptr_t buffer_phys = (uintptr_t)buffer;
        boxos_error_t err = ahci_build_ncq_write(port, slot, lba, sector_count, (void*)buffer_phys);
        if (err != BOXOS_OK) {
            ahci_free_slot(port, slot);
            return -1;
        }

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        regs->sact |= (1U << slot);
        regs->ci |= (1U << slot);

        uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(AHCI_TIMEOUT_CMD_DEFAULT);

        while (rdtsc() < timeout_tsc) {
            if (!(regs->ci & (1U << slot)) && !(regs->sact & (1U << slot))) {
                ahci_free_slot(port, slot);
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

    debug_printf("[AHCI Sync] Write failed after %d retries\n", AHCI_MAX_RETRIES);
    return -1;
}

int ahci_flush_cache_sync(uint8_t port) {
    (void)port;
    return 0;
}
