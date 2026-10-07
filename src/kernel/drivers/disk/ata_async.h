#ifndef ATA_ASYNC_H
#define ATA_ASYNC_H

#include "ata.h"
#include "error.h"

/* ===========================================================================
 *  ATA / BMIDE async submit + IRQ-driven completion
 *
 *  This is the "real-HW" path for legacy PATA: every disk transfer is issued
 *  as a Bus-Master IDE DMA command and completes via the channel's INTRQ
 *  (BMISR.IRQ + drive STATUS read), never by polling the device. The latch is
 *  read by the IRQ handler and by the waiter itself, whichever comes first,
 *  so no completion waits for the core the interrupt is routed to. Bottom-half
 *  work (memcpy of read data into the caller buffer, the error report) runs
 *  on the waiting caller, which claims the channel's `landing` slot the
 *  completion stamped (no kmalloc / sleeping locks from IRQ).
 *
 *  Channel model: PATA is master/slave on a single bus, so only one command
 *  can be in flight per channel at any time. Submits past that point are
 *  linked onto a per-channel FIFO and dispatched in order by the completion
 *  bottom-half. The two channels run independently.
 *
 *  Backing primitives:
 *    - BMIDE register file        (Intel BMIDE Spec Rev 1.0 §3)
 *    - One pre-allocated DMA32     staging page per channel (4 KB, 8 sectors)
 *    - One PRD entry per command   (PRD table page-aligned, BSS)
 *    - One `landing` slot per channel (the completion, never dropped)
 *
 *  Init order:
 *    ata_init -> ata_discover_channels -> ata_async_init()
 *                  -> per-channel BMIDE bring-up
 *                  -> irq_register_handler + irqchip_enable_irq
 *                  -> clear nIEN on each present drive
 *                ata_identify  -> ata_async_negotiate_dma_mode()
 * =========================================================================*/

/* Maximum sectors per submit — one 4 KB DMA staging page per channel. */
#define ATA_ASYNC_MAX_SECTORS 8

/* Initialise async DMA engine + IRQ wiring for every BMIDE-capable channel.
 * Idempotent. Called once from ata_init after channel discovery and per-
 * channel BMIDE register-file probe. When called pre-multicore (single-core
 * boot, init paths) the IRQ side stays armed but the sync wrapper detects
 * the IRQ path is not viable and routes to the polled fallback. */
void ata_async_init(void);

/* Per-drive DMA-mode negotiation (SET FEATURES 0xEF/0x03). Walks UDMA down
 * from the highest mode the CABLE is proven to carry, then MDMA 2..0, and
 * stops on the first the drive accepts. The walk starts at UDMA 6 only when
 * an 80-conductor cable assembly is established — ATA/ATAPI-6 (T13/1410D r3a)
 * §8.46.11: "the host shall detect that an 80-conductor cable assembly is
 * connecting the host with the device(s) before enabling any Ultra DMA mode
 * greater than 2" — and at UDMA 2 otherwise, which is the whole of the
 * difference between this and "walks UDMA 6..0". Called from ata_identify
 * after PIO mode is set so the DMA-fast-path gate (ata_async_usable) flips
 * true on success. */
void ata_async_negotiate_dma_mode(uint8_t drive_idx, const uint16_t *id);

/* True when both controller-side BMIDE and the drive's IDENTIFY advertise
 * DMA, AND the channel finished ata_async_init successfully. */
bool ata_async_usable(uint8_t drive_idx);

/* Sync wrapper — submit then wait on the cmd's done flag with interrupts as
 * the caller left them: the waiter reads the channel's interrupt latch itself
 * as well as claiming the `landing` the IRQ handler stamps, and runs the bottom
 * half, so the wait needs no interrupt taken on any core. A recovery the
 * channel owes is run by the waiter that finds it owed, so the wait needs no
 * other core's continuation either, and any caller may use it — a K-Core
 * pocket, the idle context, a caller holding a TagFS lock. `count` must be in
 * 1..8 (one 4 KB staging page). Returns 0 on success or negative ATA_ERR_*.
 * Short-circuits to the polled BMIDE engine, on the channel taken whole,
 * while the IRQ path is not viable. */
int ata_dma_sync(uint8_t drive_idx, uint64_t lba, uint16_t count,
                 bool is_write, void *buf);

/* The channel's command block for one command that is not the async engine's
 * — a PIO transfer, a FLUSH CACHE, a polled DMA, the reset between two flush
 * attempts. PATA gives a channel one command at a time, and a command written
 * while the drive is BSY with another is ignored by the drive (ATA/ATAPI-6
 * §7.15.3) — so the engine's DMA and these must never meet on a channel.
 * Take waits until nothing of the engine's is on the channel and keeps the
 * engine from starting anything more; it answers with interrupts off, and the
 * flags it returns are what Give puts back. Give starts the engine's next
 * command, if one queued meanwhile. One holder per channel; a second waits. */
uint64_t AtaChannelTake(uint8_t channel);
void     AtaChannelGive(uint8_t channel, uint64_t flags);

/* The drive's channel reset — SRST, the recovery the engine runs on a wedge —
 * for a seat the Boardroom asks to reset (BoardroomSeatReset): true when the
 * drive answers after it. Holds the channel whole, interrupts off, for the
 * reset's own patience (ATA_SRST_BSY_HELD_MS). */
bool     AtaDriveReset(uint8_t drive_idx);

/* BMIDE completion watchdog — a safety BACKSTOP, not the delivery path. Glanced
 * at once per PIT tick on the BSP (idt.c), mutually exclusive with the BMIDE
 * IRQ handler (both need the channel's irqsave cmd_lock). Recovers, purely by
 * hardware event, the failure modes a lost/misrouted legacy IDE INTRQ leaves
 * behind: a latched-but-undelivered interrupt (TIER 1, reconciled inline via
 * the same `landing` seam the IRQ uses), an engine that quit or a device that
 * vanished with no interrupt at all, and — only for a DMA engine frozen with
 * ACTIVE stuck (the one software-unobservable case) — a liveness-of-last-resort
 * bound (TIER 2). A genuine wedge is marked owed, and the waiter on the
 * channel that finds it owed fails the command ERR_IO and SRST-recovers the
 * channel itself. The waiter also holds its own command to the same liveness
 * bound, so a lost completion is waited for at most CONFIG_ATA_LIVENESS_MS and
 * a reset even while the BSP's tick does not run. No-op unless multi-core
 * BMIDE async I/O is actually in flight. */
void bmide_watchdog_scan(void);

/* Boot self-test — proves the watchdog's TIER-1 lost-INTRQ reconcile on the
 * real BMIDE engine by masking a channel's IOAPIC pin so a genuine completion
 * latches BMISR.IRQ with no CPU IRQ, then confirming the scan retires it.
 * Read-only, bounded, multi-core + BMIDE only (skips gracefully otherwise).
 * Emits "[BMIDE-WD] TIER-1 ... PASS/FAIL" for the phase matrix to assert. */
error_t bmide_watchdog_selftest(void);

/* On-demand diagnostic (WEDGETEST=on only) — drives a synthetic wedge through
 * the real scan -> owed SRST recovery run by the waiting caller and asserts the
 * wedged cmd fails ERR_IO + the channel comes back. SRSTs the boot drive, so it
 * is NOT part of a normal boot. Emits "[BMIDE-WD] TIER-2 ... PASS/FAIL". */
error_t bmide_wedge_selftest(void);

/* Stats accessors (boot/diag prints, debug commands). */
uint64_t ata_async_cmds_submitted(uint8_t channel);
uint64_t ata_async_cmds_completed(uint8_t channel);
uint64_t ata_async_cmds_failed(uint8_t channel);
uint64_t ata_async_spurious_irqs(uint8_t channel);
uint32_t ata_async_queue_depth(uint8_t channel);

#endif /* ATA_ASYNC_H */
