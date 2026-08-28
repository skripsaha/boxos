#include "ahci_sync.h"
#include "ahci.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu_calibrate.h"
#include "klib.h"
#include "atomics.h"

/*
 * ‼ HOW LONG A DISK IS GIVEN TO ANSWER, AND WHY IT IS NOT TWO SECONDS.
 *
 * It was two — CONFIG_AHCI_CMD_TIMEOUT_MS — and a spinning disk does not obey
 * it. A drive that meets a marginal sector retries the head internally before
 * it answers, and how long it may spend doing that is a property of the drive:
 * desktop drives without configurable error recovery routinely take seven
 * seconds and are permitted far more. Under a two-second clock every one of
 * those reads was declared a timeout, the port was recovered, and the read was
 * tried again — three times, and then reported as a failure on a disk that
 * would have answered.
 *
 * Every host stack gives a disk command tens of seconds: Linux's SCSI layer
 * uses thirty (drivers/scsi/sd.h, SD_TIMEOUT) and this kernel already had that
 * number written down, in CONFIG_AHCI_IO_TIMEOUT_MS, and did not use it here.
 *
 * And it is the LAST RESORT rather than the test. What ends the wait below in
 * every case anybody can name is a FACT the port reports: the slot clearing,
 * an error bit, or the link no longer being established.
 */
#define AHCI_CMD_PATIENCE_MS  CONFIG_AHCI_IO_TIMEOUT_MS

/*
 * ‼ A DISK THAT STOPS ANSWERING MUST SAY SO OUT LOUD.
 *
 * Every give-up on this path used to be a debug_printf, which compiles to
 * NOTHING in every build anybody makes — so on a machine whose only diagnostic
 * is its screen, a disk that had stopped answering was completely silent, and
 * no oracle could ask about it either. The chatter stays where it was; the
 * moments where the driver gives up on a command are said.
 */

/* What ended the wait for a command slot. Four answers, and only the last one
 * is a clock. */
typedef enum {
    AHCI_SLOT_DONE,      /* the controller cleared it: the command ran */
    AHCI_SLOT_FAULTED,   /* the port reported an error, and PxIS is cleared */
    AHCI_SLOT_GONE,      /* the link is not established any more */
    AHCI_SLOT_SILENT     /* attached, no error, and still not finished */
} AhciSlotEnd;

/*
 * Wait for one command slot, and say WHY the wait ended.
 *
 * This used to be written out three times — read, write, flush — with the same
 * two facts and the same clock in each, which is three places for the third
 * fact to be missing from. It is missing from all three: PxSSTS says whether
 * there is still a device on the other end of the cable and whether the PHY is
 * talking to it (AHCI 1.3.1 §3.3.10, DET), and a command outstanding on a link
 * that has dropped is a command nobody is going to answer. Waiting out the
 * whole budget for it is the disk equivalent of waiting for a stick that has
 * been pulled.
 */
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

        /* The cable, asked before the clock. Anything other than "device
         * present and communication established" means the answer this is
         * waiting for cannot arrive. */
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

    // DMA buffer: AHCI PRDT addresses must be below 4GB unless s64a is confirmed.
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
        /* Submit per AHCI 1.3.1 §10.3.2: write CI / SACT as a direct
         * store with ONLY the new slot bit set, never RMW. The HBA
         * clears bits independently as commands complete; a `|=` reads
         * a possibly-stale value (a bit the HW just cleared for
         * another slot) and writes it back, re-arming a completed slot
         * to be re-executed with stale FIS/PRDT. The `port_state->lock`
         * still serialises *software* writers to keep two SW-side
         * stores from racing each other. */
        spin_lock(&port_state->lock);
        if (port_state->ncq) {
            regs->sact = (1U << slot);   // NCQ only; non-queued uses PxCI alone
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

        /* A link that is not there is not a command to retry: recovering the
         * port and asking again three times spends the whole of a boot on a
         * cable somebody has unplugged. */
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

    // DMA buffer: must be below 4GB.
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
        /* Direct store, not RMW — see read path above for the spec
         * reference. Re-arming a completed slot via stale-read OR
         * corrupts NCQ on real Intel/AMD HBAs (hidden on QEMU). */
        spin_lock(&port_state->lock);
        if (port_state->ncq) {
            regs->sact = (1U << slot);   // NCQ only; non-queued uses PxCI alone
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
        /* FLUSH CACHE EXT is always non-queued — issue via PxCI only with a
         * direct single-bit store (PxCI is write-1-to-set; an RMW could
         * re-arm a slot the HBA just cleared). */
        spin_lock(&port_state->lock);
        regs->ci = (1U << slot);
        spin_unlock(&port_state->lock);

        /* FLUSH CACHE EXT is never queued, so PxSACT says nothing about it. */
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
