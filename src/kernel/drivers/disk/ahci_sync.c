#include "ahci_sync.h"
#include "ahci.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu_calibrate.h"
#include "klib.h"
#include "atomics.h"
#include "error.h"

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
 * an error bit, the engine being stopped under it, or the link no longer being
 * established.
 */
#define AHCI_CMD_PATIENCE_MS  CONFIG_AHCI_IO_TIMEOUT_MS

#define AHCI_FATAL_BITS (AHCI_PIS_TFES | AHCI_PIS_HBFS | AHCI_PIS_IFS)

/*
 * ‼ A DISK THAT STOPS ANSWERING MUST SAY SO OUT LOUD.
 *
 * Every give-up on this path used to be a debug_printf, which compiles to
 * NOTHING in every build anybody makes — so on a machine whose only diagnostic
 * is its screen, a disk that had stopped answering was completely silent, and
 * no oracle could ask about it either. The chatter stays where it was; the
 * moments where the driver gives up on a command are said.
 */

/* What ended the wait for a command stood over. Four answers, and only the
 * last one is a clock. */
typedef enum {
    AHCI_SLOT_DONE,      /* the controller cleared it, with the engine running throughout */
    AHCI_SLOT_DROPPED,   /* the engine was interrupted under it: ask again once it runs */
    AHCI_SLOT_GONE,      /* the port or its link is not there any more */
    AHCI_SLOT_SILENT     /* attached, no error, and still not finished */
} AhciSlotEnd;

/*
 * Wait for one command slot a caller stands over, and say WHY the wait ended.
 *
 * ‼ A SLOT BIT THAT CLEARED IS NOT A COMMAND THAT FINISHED.
 *
 * AHCI 1.3.1 §3.3.14: clearing PxCMD.ST clears PxCI. So a recovery stopping
 * the engine — for a fault on this command or on any other, run on this core
 * or another — makes a command the engine dropped look exactly like one it
 * completed, and this poll used to answer "done" with whatever the page held.
 * The completion interrupt made it worse: it clears PxIS before anybody here
 * looks, so the error bit this was watching for was gone by the time it looked.
 *
 * The port counts every interruption of its engine (ahci_port_t.trouble),
 * before the engine can stop, and the count this command was armed under is
 * the one it must still see after its bit clears: read in that order, the bit
 * then the count, a cleared bit under an unchanged count is a completion.
 * An error seen here is handed to the port's recovery the way the interrupt
 * would have handed it (AhciPortReap), and moves the count itself.
 *
 * And PxSSTS says whether there is still a device on the other end of the
 * cable (AHCI 1.3.1 §3.3.10, DET): a command outstanding on a link that has
 * dropped is a command nobody is going to answer.
 */
static AhciSlotEnd ahci_wait_for_slot(ahci_port_t* port, uint8_t slot, bool queued,
                                      uint32_t trouble_at_arm)
{
    volatile ahci_port_regs_t* regs = port->regs;
    uint32_t bit        = 1U << slot;
    uint64_t give_up_at = rdtsc() + cpu_ms_to_tsc(AHCI_CMD_PATIENCE_MS);

    for (;;) {
        uint32_t ci      = regs->ci;
        uint32_t sact    = queued ? regs->sact : 0;
        uint32_t trouble = __atomic_load_n(&port->trouble, __ATOMIC_ACQUIRE);
        if (!((ci | sact) & bit)) {
            return trouble == trouble_at_arm ? AHCI_SLOT_DONE : AHCI_SLOT_DROPPED;
        }
        if (trouble != trouble_at_arm) {
            return AHCI_SLOT_DROPPED;
        }
        if (!__atomic_load_n(&port->active, __ATOMIC_ACQUIRE)) {
            return AHCI_SLOT_GONE;
        }
        if (regs->is & AHCI_FATAL_BITS) {
            AhciPortReap(port);
            continue;
        }

        /* The cable, asked before the clock. Anything other than "device
         * present and communication established" means the answer this is
         * waiting for cannot arrive. */
        if ((regs->ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PRESENT) {
            kprintf("[AHCI] port %u: the link is no longer established "
                    "(SSTS=0x%08x) — the command on slot %u has nobody to "
                    "answer it\n", port->port_num, regs->ssts, slot);
            return AHCI_SLOT_GONE;
        }

        if ((int64_t)(rdtsc() - give_up_at) >= 0) {
            return AHCI_SLOT_SILENT;
        }
        cpu_pause();
    }
}

/* Arm a slot stood over, waiting out a recovery the port is in: false only
 * when the port left the machine. */
static bool ahci_arm_when_running(uint8_t port, uint8_t slot, bool queued,
                                  uint32_t* trouble_at_arm)
{
    while (!AhciArmStanding(port, slot, queued, trouble_at_arm)) {
        if (!AhciAwaitEngine(port)) {
            return false;
        }
    }
    return true;
}

/*
 * One read or write, stood over: a slot from the port's roadstead — WAITED
 * FOR, never gambled on, and never refused for being busy — the command built
 * into it, armed, and asked again when the engine was interrupted under it.
 *
 * `what` names the command in the lines this says.
 */
static int ahci_stand_over_io(uint8_t port, uint64_t lba, uint16_t sector_count,
                              uintptr_t dma_phys, bool write, const char* what)
{
    ahci_port_t* port_state = ahci_get_port_state(port);
    if (!port_state) {
        return -1;
    }
    int drawn = AhciStandForSlot(port);
    if (drawn < 0) {
        return -1;
    }
    uint8_t     slot    = (uint8_t)drawn;
    AhciSlotEnd how     = AHCI_SLOT_GONE;
    int         aborted = 0;    /* attempts a task-file error with ABRT ended */
    int         unread  = 0;    /* attempts of a read a task-file error with UNC ended */

    for (int attempt = 0; attempt < AHCI_MAX_RETRIES; attempt++) {
        if (ahci_build_io(port, slot, lba, sector_count, (void*)dma_phys, write) != OK) {
            how = AHCI_SLOT_GONE;
            break;
        }
        __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);

        uint32_t tfes_at_arm    = __atomic_load_n(&port_state->stats.tfes_count, __ATOMIC_ACQUIRE);
        uint32_t trouble_at_arm = 0;
        if (!ahci_arm_when_running(port, slot, port_state->ncq, &trouble_at_arm)) {
            how = AHCI_SLOT_GONE;
            break;
        }
        how = ahci_wait_for_slot(port_state, slot, port_state->ncq, trouble_at_arm);
        if (how == AHCI_SLOT_DONE || how == AHCI_SLOT_GONE) {
            break;
        }
        if (how == AHCI_SLOT_SILENT) {
            kprintf("[AHCI] port %u: the disk did not answer a %s in %u ms "
                    "(attempt %d of %d)\n", port, what, AHCI_CMD_PATIENCE_MS,
                    attempt + 1, AHCI_MAX_RETRIES);
            __atomic_fetch_add(&port_state->stats.timeout_count, 1, __ATOMIC_RELAXED);
            AhciPortTroubled(port, trouble_at_arm);
        }
        /* The slot is asked again only once the engine runs: the recovery the
         * interruption owes stops it first, and that stop is the one proof the
         * HBA let the command and its page go. */
        if (!AhciAwaitEngine(port)) {
            how = AHCI_SLOT_GONE;
            break;
        }
        /* The recovery has recorded the fault by now: was it this write's,
         * aborted by the drive rather than failed on the medium — or this
         * read's, a sector the drive reached and could not read? */
        uint32_t error = (__atomic_load_n(&port_state->stats.last_tfd, __ATOMIC_ACQUIRE) >>
                          AHCI_PTFD_ERR_SHIFT) & AHCI_PTFD_ERR_MASK;
        if (__atomic_load_n(&port_state->stats.tfes_count, __ATOMIC_ACQUIRE) != tfes_at_arm) {
            if (write && (error & AHCI_PTFD_ERR_ABRT))
                aborted++;
            if (!write && (error & AHCI_PTFD_ERR_UNC))
                unread++;
        }
    }

    AhciLeaveSlot(port, slot);
    if (how == AHCI_SLOT_DONE) {
        return 0;
    }
    if (write && aborted == AHCI_MAX_RETRIES &&
        !__atomic_exchange_n(&port_state->write_refused, 1u, __ATOMIC_ACQ_REL)) {
        kprintf("[AHCI] port %u: the drive aborted every attempt of a write — it refuses "
                "writes, and is taken as a medium that takes none while it stands\n", port);
    }
    if (how == AHCI_SLOT_GONE) {
        kprintf("[AHCI] port %u: a %s of %u sector(s) at LBA %llu was not "
                "completed — the port or its link is gone\n",
                port, what, (unsigned)sector_count, (unsigned long long)lba);
    } else if (unread == AHCI_MAX_RETRIES) {
        kprintf("[AHCI] port %u: the drive could not read %u sector(s) at LBA %llu "
                "(UNC, every one of %d attempts) — a sector it cannot read\n",
                port, (unsigned)sector_count, (unsigned long long)lba, AHCI_MAX_RETRIES);
        return AHCI_ERR_MEDIUM;
    } else {
        kprintf("[AHCI] port %u: a %s failed after %d attempts (%u sector(s) at "
                "LBA %llu)\n", port, what, AHCI_MAX_RETRIES,
                (unsigned)sector_count, (unsigned long long)lba);
    }
    return -1;
}

int ahci_read_sectors_sync(uint8_t port, uint64_t lba,
                           uint16_t sector_count, void* buffer) {
    if (!buffer || sector_count == 0) return -1;

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

    int rc = ahci_stand_over_io(port, lba, sector_count, dma_phys, false, "read");
    if (rc == 0) {
        memcpy(buffer, dma_virt, sector_count * 512);
    }
    pmm_free(dma_page, pages_needed);
    return rc;
}

int ahci_write_sectors_sync(uint8_t port, uint64_t lba,
                            uint16_t sector_count, const void* buffer) {
    if (!buffer || sector_count == 0) return -1;

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

    int rc = ahci_stand_over_io(port, lba, sector_count, dma_phys, true, "write");
    pmm_free(dma_page, pages_needed);
    return rc;
}

/* FLUSH CACHE EXT into the slot the door gave: no data, never queued. */
static void ahci_build_flush(ahci_port_t* port_state, uint8_t slot)
{
    ahci_cmd_header_t* cmdheader = (ahci_cmd_header_t*)port_state->clb_virt;
    cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmdheader[slot].w = 0;
    cmdheader[slot].prdtl = 0;
    cmdheader[slot].prdbc = 0;

    ahci_cmd_table_t* cmdtbl = (ahci_cmd_table_t*)port_state->ctba_virt[slot];
    memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)&cmdtbl->cfis[0];
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_FLUSH_CACHE_EXT;
    cmdfis->device = 0;

    mfence();
}

/*
 * ‼ WHAT THIS RETURNS.
 *
 * Zero on success; otherwise an `error_t` value, carried out through the
 * `int` that ahci_sync.h declares and BoardroomFlush hands along untouched.
 * error_t values are positive and the other two media answer with zero or a
 * negative code of their own, so the two vocabularies cannot be confused
 * where they meet (tagfs_flush_cache).
 *
 *   OK                    the drive's write cache has reached the media.
 *   ERR_DEVICE_NOT_READY  there is no such port in this machine any more.
 *   ERR_IO                the device did not answer, or answered with an
 *                         error, or the link went away under the command.
 *
 * A port another flush holds, or with every slot out, is not among them: that
 * used to come back as ERR_BUSY, and every caller above span on it. The flush
 * now waits in the port's roadstead, and its want of the whole port stops
 * anything queued behind it from being admitted, so it is never starved.
 */
int ahci_flush_cache_sync(uint8_t port) {
    ahci_port_t* port_state = ahci_get_port_state(port);
    if (!port_state) {
        return ERR_DEVICE_NOT_READY;
    }

    /* THE PORT, NOT A SLOT — AND ONCE FOR THE WHOLE CALL, NOT PER ATTEMPT.
     *
     * FLUSH CACHE EXT has no queued form, and AHCI 1.3.1 §1.7 is flat about
     * what that means: "System software is responsible to ensure that queued
     * and non-queued commands are not mixed in the command list for the same
     * device with the exception of the NCQ Unload command." This used to draw
     * an ordinary slot, which on an NCQ port put a non-queued command in the
     * list beside live FPDMA tags — and that is the one thing the commit
     * point of a write must not do: the drive aborts the flush and aborts the
     * queue with it, so every write in flight on the port fails and the link
     * is reset, at the exact moment those writes were being made durable.
     *
     * So the port is taken WHOLE: this waits in the roadstead until every slot
     * is free and PxSACT and PxCI are both empty, and only then is the door
     * closed. The slot below is one this caller alone owns. On a non-NCQ port
     * the port's only slot is the whole of it, so this is the same walk.
     *
     * It is taken OUTSIDE the retry loop, and §1.7 is the reason: a retry, and
     * the recovery between it and the attempt that failed, are as much "this
     * command alone on the port" as the first attempt was, and re-opening the
     * door between attempts would let the queue refill under the one command
     * that must not share the list. The recovery leaves a held door held. */
    uint8_t slot = 0;
    AhciTake took = ahci_port_take_whole(port, AHCI_CMD_PATIENCE_MS, &slot);
    if (took == AHCI_TAKE_NO_PORT) {
        return ERR_DEVICE_NOT_READY;
    }
    if (took != AHCI_TAKE_GOT) {
        return ERR_IO;
    }

    AhciSlotEnd how = AHCI_SLOT_GONE;
    for (int attempt = 0; attempt < AHCI_MAX_RETRIES; attempt++) {
        ahci_build_flush(port_state, slot);

        /* PxSACT is not touched: FLUSH CACHE EXT is never queued, and the
         * door closed on PxSACT reading zero. */
        uint32_t trouble_at_arm = 0;
        if (!ahci_arm_when_running(port, slot, false, &trouble_at_arm)) {
            how = AHCI_SLOT_GONE;
            break;
        }
        how = ahci_wait_for_slot(port_state, slot, false, trouble_at_arm);
        if (how == AHCI_SLOT_DONE || how == AHCI_SLOT_GONE) {
            break;
        }
        if (how == AHCI_SLOT_SILENT) {
            kprintf("[AHCI] port %u: the disk did not answer a cache flush in "
                    "%u ms (attempt %d of %d)\n", port, AHCI_CMD_PATIENCE_MS,
                    attempt + 1, AHCI_MAX_RETRIES);
            AhciPortTroubled(port, trouble_at_arm);
        }
        if (!AhciAwaitEngine(port)) {
            how = AHCI_SLOT_GONE;
            break;
        }
    }

    ahci_port_give_back(port);

    if (how == AHCI_SLOT_DONE) {
        return OK;
    }
    if (how == AHCI_SLOT_GONE) {
        kprintf("[AHCI] port %u: a cache flush was not completed — the port or "
                "its link is gone, and the writes behind it are not proven to "
                "have reached the media\n", port);
    } else {
        kprintf("[AHCI] port %u: a cache flush failed after %d attempts — the "
                "writes behind it are not proven to have reached the media\n",
                port, AHCI_MAX_RETRIES);
    }
    return (int)(ahci_get_port_state(port) ? ERR_IO : ERR_DEVICE_NOT_READY);
}

/* 128 KiB — thirty-two contiguous DMA32 pages, an order the buddy allocator
 * satisfies routinely, and sixteen times the four-kilobyte block everything
 * above this reads in. */
uint32_t ahci_max_run_sectors(uint8_t port)
{
    (void)port;
    return 256u;
}
