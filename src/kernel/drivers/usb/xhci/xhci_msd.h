#ifndef XHCI_MSD_H
#define XHCI_MSD_H

#include "ktypes.h"
#include "error.h"
#include "xhci.h"

/*
 * USB Mass Storage, over Bulk-Only Transport, speaking SCSI.
 *
 * This is what makes a flash drive a place BoxOS can keep a filesystem. The
 * whole conversation is three bulk transfers: a 31-byte command wrapper out, a
 * data stage in whichever direction the command needs, and a 13-byte status
 * wrapper back. The SCSI command sits inside the first of those, and the
 * device's answer to "how big are you" and "give me these sectors" comes back
 * through the other two.
 *
 * Every call here is synchronous, because the thing above it is a filesystem
 * mounting at boot with nothing else to do until the sector arrives.
 */

/* The sector size BoxOS keeps filesystems in. A device reporting anything else
 * is reported and refused rather than silently misread. */
#define XHCI_MSD_SECTOR_BYTES 512

/* Claim a device the enumerator has configured for us: ask how many logical
 * units it has, wait for the medium, and learn its size. Returns 0 when the
 * device answered and is readable. */
int  xhci_msd_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

/* True when this device has already been claimed, so a second sweep does not
 * start a second conversation with it. */
bool xhci_msd_slot_attached(xhci_device_slot_t* slot);

/* Let go of a device that has been unplugged. */
void xhci_msd_release(xhci_device_slot_t* slot);

/* How many USB disks are attached, and what each one is. Units are numbered in
 * the order they attached and keep their number until they leave. */
uint8_t     xhci_msd_unit_count(void);
bool        xhci_msd_unit_present(uint8_t unit);
uint64_t    xhci_msd_unit_sectors(uint8_t unit);
uint32_t    xhci_msd_unit_physical_bytes(uint8_t unit);
const char* xhci_msd_unit_name(uint8_t unit);

/* Sector I/O. Counts are in 512-byte sectors; a request larger than the
 * bounce buffer is split, so there is no size a caller has to know about.
 * Returns 0 on success. */
int xhci_msd_read (uint8_t unit, uint64_t lba, uint32_t count, void* buffer);
int xhci_msd_write(uint8_t unit, uint64_t lba, uint32_t count, const void* buffer);

/*
 * A read for a caller that is not going to stand and watch.
 *
 * The sectors land straight in `dma_phys` — no bounce, because the whole point
 * is that nobody is here to copy them out afterwards — and `cb` is told when
 * the device has answered. It runs on a K-Core, out of the guide loop, so it
 * may take locks, free memory and wake whoever was waiting.
 *
 * The callback is shaped exactly like the SATA one, so that the Boardroom can
 * hand the same function to either kind of seat without an adapter standing in
 * between: `index` is the USB unit, `slot` is meaningless here and is zero.
 *
 * The request must cover whole device blocks and start on one. Everything
 * BoxOS reads asynchronously is a filesystem block, which does; a request that
 * does not is REFUSED rather than quietly widened, because widening it would
 * mean a bounce buffer and a copy, and that is the sync path's job.
 *
 * Returns OK when the command is on its way (or queued behind one on the same
 * device), and the callback WILL run. On any other return it will not.
 */
typedef void (*XhciMsdAsyncCb)(uint8_t index, uint8_t slot,
                               error_t status, void* ctx);

error_t xhci_msd_read_async(uint8_t unit, uint64_t lba, uint32_t count,
                            void* dma_phys, XhciMsdAsyncCb cb, void* ctx);

/* True when this unit can take the call above — it exists, it is ready, and
 * the geometry of a filesystem block suits it. */
bool xhci_msd_unit_can_read_async(uint8_t unit);

/*
 * Look over the asynchronous reads in flight, and give up on one that has
 * stopped being answered.
 *
 * A read somebody is standing over carries its own deadline, in the loop that
 * is standing there. One nobody is standing over has nobody to carry it — and
 * a device that goes quiet mid-command would otherwise hold its turn for ever,
 * which is not one lost read but every read after it on that device. Measured
 * by mutation: with the completion suppressed, the machine did not reach the
 * shell at all.
 *
 * Called from the tick, beside this driver's other watchdogs.
 */
void xhci_msd_watchdog(void);

/* Push the device's own write cache to the medium. A write that has completed
 * is a write the device has accepted, not necessarily one it has kept. */
int xhci_msd_flush(uint8_t unit);

#endif
