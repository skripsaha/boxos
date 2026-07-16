#ifndef ATA_ASYNC_H
#define ATA_ASYNC_H

#include "ata.h"
#include "error.h"

/* ===========================================================================
 *  ATA / BMIDE async submit + IRQ-driven completion
 *
 *  This is the "real-HW" path for legacy PATA: every disk transfer is issued
 *  as a Bus-Master IDE DMA command and completes via the channel's INTRQ
 *  (BMISR.IRQ + drive STATUS read), never by polling the device. Bottom-half
 *  work (memcpy of read data into the caller buffer, callback invocation,
 *  Touch publish, free of per-cmd state) runs in K-Core context via the
 *  shared irq_defer ring (no kmalloc / sleeping locks from IRQ).
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
 *    - irq_defer slot per cmd      (heavy completion work)
 *
 *  Init order:
 *    ata_init -> ata_discover_channels -> ata_async_init()
 *                  -> per-channel BMIDE bring-up
 *                  -> irq_register_handler + irqchip_enable_irq
 *                  -> clear nIEN on each present drive
 *                ata_identify  -> ata_async_negotiate_dma_mode()
 * =========================================================================*/

/* Per-cmd completion callback. Always invoked from K-Core deferred context
 * (irq_defer pump), never from IRQ. May kmalloc, take sleeping locks,
 * publish Touch events. `status` is OK on success or one of:
 *   ERR_IO            — drive ERR/DF or BMIDE error latched
 *   ERR_DEVICE_NOT_READY — channel went away mid-flight
 *   ERR_BUSY          — channel queue overflow at submit time
 */
typedef void (*AtaAsyncCb)(uint8_t drive_idx, error_t status, void *ctx);

/* Maximum sectors per submit — one 4 KB DMA staging page per channel. */
#define ATA_ASYNC_MAX_SECTORS 8

/* Initialise async DMA engine + IRQ wiring for every BMIDE-capable channel.
 * Idempotent. Called once from ata_init after channel discovery and per-
 * channel BMIDE register-file probe. When called pre-multicore (single-core
 * boot, init paths) the IRQ side stays armed but the sync wrappers detect
 * irq_defer is not ready and route to the polled fallback. */
void ata_async_init(void);

/* Per-drive DMA-mode negotiation (SET FEATURES 0xEF/0x03). Walks UDMA 6..0
 * then MDMA 2..0, stops on the first the drive accepts. Called from
 * ata_identify after PIO mode is set so the DMA-fast-path gate
 * (ata_async_usable) flips true on success. */
void ata_async_negotiate_dma_mode(uint8_t drive_idx, const uint16_t *id);

/* True when both controller-side BMIDE and the drive's IDENTIFY advertise
 * DMA, AND the channel finished ata_async_init successfully. */
bool ata_async_usable(uint8_t drive_idx);

/* Non-blocking submit. `count` must be in 1..8 (one 4 KB staging page).
 * Write path: `user_buf` is copied into the staging buffer up front.
 * Read  path: the IRQ-deferred completion does the memcpy back.
 *
 * Returns OK on accepted (callback will fire later) or:
 *   ERR_INVALID_ARGUMENT — bad drive_idx / nul cb / count out of range
 *   ERR_DEVICE_NOT_READY  — async path not initialised on this channel
 *   ERR_BUSY              — per-channel queue saturated (see CONFIG below)
 *   ERR_IO               — kmalloc failed for per-cmd state */
error_t ata_submit_read_async(uint8_t drive_idx, uint64_t lba,
                              uint16_t count, void *user_buf,
                              AtaAsyncCb cb, void *cb_ctx);

error_t ata_submit_write_async(uint8_t drive_idx, uint64_t lba,
                               uint16_t count, const void *user_buf,
                               AtaAsyncCb cb, void *cb_ctx);

/* Sync wrapper — submit then wait on the cmd's done flag using sti;hlt
 * (IRQ fires, deferred bottom-half flips the flag). Returns 0 on success
 * or negative ATA_ERR_*. Safe to call from kernel context when the
 * caller is NOT a K-Core that other code expects to keep pumping
 * (init/mount paths, sync legacy callers). Internally short-circuits to
 * the polled BMIDE engine when irq_defer isn't ready yet. */
int ata_dma_sync(uint8_t drive_idx, uint64_t lba, uint16_t count,
                 bool is_write, void *buf);

/* BMIDE completion watchdog — a safety BACKSTOP, not the delivery path. Glanced
 * at once per PIT tick on the BSP (idt.c), mutually exclusive with the BMIDE
 * IRQ handler (both need the channel's irqsave cmd_lock). Recovers, purely by
 * hardware event, the failure modes a lost/misrouted legacy IDE INTRQ leaves
 * behind: a latched-but-undelivered interrupt (TIER 1, reconciled inline via
 * the same `landing` seam the IRQ uses), an engine that quit or a device that
 * vanished with no interrupt at all, and — only for a DMA engine frozen with
 * ACTIVE stuck (the one software-unobservable case) — a liveness-of-last-resort
 * bound (TIER 2). A genuine wedge is failed ERR_IO + SRST-recovered on a K-Core
 * (never-drop), so a lost completion can never hang a BMIDE sync waiter forever.
 * No-op unless multi-core BMIDE async I/O is actually in flight. */
void bmide_watchdog_scan(void);

/* Boot self-test — proves the watchdog's TIER-1 lost-INTRQ reconcile on the
 * real BMIDE engine by masking a channel's IOAPIC pin so a genuine completion
 * latches BMISR.IRQ with no CPU IRQ, then confirming the scan retires it.
 * Read-only, bounded, multi-core + BMIDE only (skips gracefully otherwise).
 * Emits "[BMIDE-WD] TIER-1 ... PASS/FAIL" for the phase matrix to assert. */
error_t bmide_watchdog_selftest(void);

/* Stats accessors (boot/diag prints, debug commands). */
uint64_t ata_async_cmds_submitted(uint8_t channel);
uint64_t ata_async_cmds_completed(uint8_t channel);
uint64_t ata_async_cmds_failed(uint8_t channel);
uint64_t ata_async_spurious_irqs(uint8_t channel);
uint32_t ata_async_queue_depth(uint8_t channel);

#endif /* ATA_ASYNC_H */
