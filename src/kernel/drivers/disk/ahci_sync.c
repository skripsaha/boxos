#include "ahci_sync.h"
#include "ahci.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu_calibrate.h"
#include "klib.h"
#include "atomics.h"
#include "hourglass.h"
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

/*
 * Draw a command slot — WAITED FOR, not gambled on.
 *
 * `ahci_alloc_slot` answers about this instant only, and both callers below
 * used to treat "no free slot right now" as a failed attempt: `continue`, one
 * of AHCI_MAX_RETRIES spent in nanoseconds, and a debug_printf that compiles
 * to nothing. Three of those in a row and a perfectly healthy disk reported a
 * read failure without a single command ever reaching it. Two things fill a
 * port: thirty-two NCQ tags all outstanding at once, and — since FLUSH CACHE
 * EXT started taking the port whole, as AHCI 1.3.1 §1.7 requires — a cache
 * flush running on it.
 *
 * Both of those are transient by construction: they end when commands
 * complete. So this waits for the FACT of a slot coming free, and the clock is
 * the last resort behind it — the same AHCI_CMD_PATIENCE_MS one command is
 * given, because what is being waited out is exactly one command's worth of
 * port. A slot that never comes is said out loud and refused; it is not a
 * retry, and it does not spend one.
 */
static int ahci_draw_slot(uint8_t port)
{
    int slot = ahci_alloc_slot(port);
    if (slot >= 0) return slot;

    /* ‼ A PORT THAT HAS LEFT THE MACHINE IS NOT A BUSY PORT.
     *
     * ahci_alloc_slot answers -1 for both, and once ahci_port_recover could
     * retire a dead port the second meaning arrived here: the glass below
     * would be turned and spun out in full on a port that has no device, no
     * link and no slots, once per command, for as long as anything kept
     * asking. The two are told apart by asking the room the port is in, which
     * is one look and no clock at all. */
    if (!ahci_get_port_state(port)) {
        kprintf("[AHCI] port %u has left the machine — the command is not "
                "issued\n", port);
        return -1;
    }

    HourGlass glass;
    if (!HourGlassTurn(&glass, (uint64_t)AHCI_CMD_PATIENCE_MS * 1000ULL)) {
        kprintf("[AHCI] port %u: every command slot is busy and there is no "
                "clock to wait one out on — the command is not issued\n", port);
        return -1;
    }

    for (;;) {
        slot = ahci_alloc_slot(port);
        if (slot >= 0) return slot;
        if (!ahci_get_port_state(port)) {
            kprintf("[AHCI] port %u left the machine while a command waited "
                    "for a slot on it — the command is not issued\n", port);
            return -1;
        }
        if (HourGlassRunOut(&glass)) {
            kprintf("[AHCI] port %u: every command slot was still busy after "
                    "%lu us on the %s — the command is not issued\n",
                    port, (unsigned long)HourGlassElapsedUs(&glass),
                    HourGlassRulerName(&glass));
            return -1;
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
        /* A busy port is not a failed attempt — see ahci_draw_slot. */
        int slot = ahci_draw_slot(port);
        if (slot < 0) {
            pmm_free(dma_page, pages_needed);
            return -1;
        }

        __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);

        error_t err = ahci_build_io(port, slot, lba, sector_count, (void*)dma_phys, false);
        if (err != OK) {
            ahci_free_slot(port, slot);
            pmm_free(dma_page, pages_needed);
            return -1;
        }

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        if (!regs) {
            /* The port left the machine between drawing the slot and arming
             * it — ahci_port_recover can retire one now, from this core or
             * from the watchdog's. */
            kprintf("[AHCI] port %u left the machine before its command could "
                    "be issued\n", port);
            ahci_free_slot(port, slot);
            pmm_free(dma_page, pages_needed);
            return -1;
        }
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
            /* A recovery that could not get the link back has taken the port
             * out of the machine (see ahci_port_recover). Asking it again
             * would be asking a port that is no longer there. */
            if (ahci_port_recover(port_state) != OK) break;
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
        /* A busy port is not a failed attempt — see ahci_draw_slot. */
        int slot = ahci_draw_slot(port);
        if (slot < 0) {
            pmm_free(dma_page, pages_needed);
            return -1;
        }

        __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);

        error_t err = ahci_build_io(port, slot, lba, sector_count, (void*)dma_phys, true);
        if (err != OK) {
            ahci_free_slot(port, slot);
            pmm_free(dma_page, pages_needed);
            return -1;
        }

        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        if (!regs) {
            /* The port left the machine between drawing the slot and arming
             * it — ahci_port_recover can retire one now, from this core or
             * from the watchdog's. */
            kprintf("[AHCI] port %u left the machine before its command could "
                    "be issued\n", port);
            ahci_free_slot(port, slot);
            pmm_free(dma_page, pages_needed);
            return -1;
        }
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
            /* A recovery that could not get the link back has taken the port
             * out of the machine (see ahci_port_recover). Asking it again
             * would be asking a port that is no longer there. */
            if (ahci_port_recover(port_state) != OK) break;
        }
    }

    pmm_free(dma_page, pages_needed);
    kprintf("[AHCI] port %u: a write failed after %d attempts\n",
            port, AHCI_MAX_RETRIES);
    return -1;
}

/*
 * ‼ WHAT THIS RETURNS, AND WHY IT IS NOT A BARE -1 ANY MORE.
 *
 * Zero on success; otherwise an `error_t` value, carried out through the
 * `int` that ahci_sync.h declares and BoardroomFlush hands along untouched.
 * error_t values are positive and the other two media answer with zero or a
 * negative code of their own, so the two vocabularies cannot be confused
 * where they meet (tagfs_flush_cache).
 *
 *   OK                    the drive's write cache has reached the media.
 *   ERR_BUSY              another whole-port command holds this port AT THIS
 *                         INSTANT. The medium is healthy, nothing was issued,
 *                         and the caller — who still holds its bytes — may
 *                         come again. This is the outcome the whole change
 *                         exists for: it used to arrive as ERR_IO, and every
 *                         caller above read that as a failing disk.
 *   ERR_DEVICE_NOT_READY  there is no such port in this machine any more.
 *   ERR_IO                the device did not answer, or answered with an
 *                         error, or the link went away under the command.
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
     * So the port is taken WHOLE: this waits for the port to go quiet — every
     * slot free, PxSACT and PxCI both empty — and only then closes it. The
     * slot below is one this caller alone owns. On a non-NCQ port the port's
     * only slot is the whole of it, so this is the same walk it always was.
     *
     * It is taken OUTSIDE the retry loop, and §1.7 is the first reason: a
     * retry, and the COMRESET between it and the attempt that failed, are as
     * much "this command alone on the port" as the first attempt was, and
     * re-opening the door between attempts would let the queue refill under
     * the one command that must not share the list. The second reason is the
     * clock. Waiting for a device to retire what it was given is bounded by
     * how long one command may take, which is the same AHCI_CMD_PATIENCE_MS
     * the command itself gets; paid once per call the worst case of this
     * function is one wait plus AHCI_MAX_RETRIES commands, where paying it
     * per attempt would have made it AHCI_MAX_RETRIES of each — and this runs
     * at halt_sync_storage(), where every second is a second the machine will
     * not shut down in. */
    uint8_t slot = 0;
    AhciTake took = ahci_port_take_whole(port, AHCI_CMD_PATIENCE_MS, &slot);
    if (took != AHCI_TAKE_GOT) {
        /* Nothing is held and nothing was issued in any of the three cases.
         * AHCI_TAKE_HELD said nothing on the console on purpose — two flushes
         * of one medium meeting is ordinary traffic, and the caller is the
         * one who knows whether coming back is possible where it stands. The
         * other two have already said what they could not do. */
        switch (took) {
        case AHCI_TAKE_HELD:    return ERR_BUSY;
        case AHCI_TAKE_NO_PORT: return ERR_DEVICE_NOT_READY;
        default:                return ERR_IO;
        }
    }

    error_t result = ERR_IO;
    for (int retry = 0; retry < AHCI_MAX_RETRIES; retry++) {
        volatile ahci_port_regs_t* regs = ahci_get_port_regs_pub(port);
        if (!regs) {
            /* The port was taken out of the machine under this command —
             * by this loop's own recovery, or by the watchdog on another
             * core. Its command list is still mapped, but nothing may be
             * issued into it. */
            kprintf("[AHCI] port %u left the machine while its cache flush "
                    "was being retried\n", port);
            break;
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

        /* FLUSH CACHE EXT is always non-queued — issue via PxCI only with a
         * direct single-bit store (PxCI is write-1-to-set; an RMW could
         * re-arm a slot the HBA just cleared). PxSACT is not touched, and now
         * that is true of the register as well as of this store: the hold
         * above ended on PxSACT reading zero. */
        spin_lock(&port_state->lock);
        regs->ci = (1U << slot);
        spin_unlock(&port_state->lock);

        /* FLUSH CACHE EXT is never queued, so PxSACT says nothing about it. */
        AhciSlotEnd how = ahci_wait_for_slot(port_state, regs, slot, false);

        if (how == AHCI_SLOT_DONE) {
            result = OK;
            break;
        }
        if (how == AHCI_SLOT_GONE) {
            break;
        }
        if (how == AHCI_SLOT_SILENT) {
            kprintf("[AHCI] port %u: the disk did not answer a cache flush in "
                    "%u ms (attempt %d of %d)\n", port, AHCI_CMD_PATIENCE_MS,
                    retry + 1, AHCI_MAX_RETRIES);
        }
        /* The hold survives the recovery: ahci_port_recover drives PxCMD.ST
         * and PxSCTL.DET and never touches the slot pool, so the port comes
         * back still closed to everyone but this command — unless it comes
         * back not at all, which is the one case that ends the retries. */
        if (retry < AHCI_MAX_RETRIES - 1) {
            if (ahci_port_recover(port_state) != OK) break;
        }
    }

    ahci_port_give_back(port);

    if (result != OK) {
        kprintf("[AHCI] port %u: a cache flush failed after %d attempts — the "
                "writes behind it are not proven to have reached the media\n",
                port, AHCI_MAX_RETRIES);
    }
    return (int)result;
}

/* 128 KiB — thirty-two contiguous DMA32 pages, an order the buddy allocator
 * satisfies routinely, and sixteen times the four-kilobyte block everything
 * above this reads in. */
uint32_t ahci_max_run_sectors(uint8_t port)
{
    (void)port;
    return 256u;
}
