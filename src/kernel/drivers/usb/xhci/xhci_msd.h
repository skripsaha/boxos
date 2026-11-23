#ifndef XHCI_MSD_H
#define XHCI_MSD_H

#include "ktypes.h"
#include "error.h"
#include "xhci.h"
#include "roadstead.h"

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
 * Most calls here are synchronous, because the thing above them is a
 * filesystem mounting at boot with nothing else to do until the sector
 * arrives. The ones that are not are a read and a write for a caller that
 * went away (xhci_msd_read_async, xhci_msd_write_async).
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
/*
 * The largest run of sectors this unit moves in one pass over the bus.
 *
 * Bulk-Only Transport is a strictly serial conversation and every command goes
 * through one bounce buffer, so this is that buffer in sectors. Bigger
 * requests are not refused — msd_rw splits them — but each extra pass is
 * another command wrapper, another data stage and another status wrapper, and
 * on a real flash drive those are what the time goes on.
 */
uint32_t    xhci_msd_unit_max_run(uint8_t unit);

uint64_t    xhci_msd_unit_sectors(uint8_t unit);
uint32_t    xhci_msd_unit_logical_bytes(uint8_t unit);   /* its own block: 512, or 4096 on 4Kn */
uint32_t    xhci_msd_unit_physical_bytes(uint8_t unit);
uint64_t    xhci_msd_unit_first_aligned(uint8_t unit);
/* Whether the unit refused a write with DATA PROTECT (sense key 7): it takes
 * no write for as long as it is seated. */
bool        xhci_msd_unit_write_protected(uint8_t unit);
const char* xhci_msd_unit_name(uint8_t unit);

/* Sector I/O. Counts are in 512-byte sectors; a request larger than the
 * bounce buffer is split, so there is no size a caller has to know about.
 * Returns 0 on success, -1 on a refusal — and a read the device itself
 * refused with MEDIUM ERROR (sense key 3) answers XHCI_MSD_ERR_MEDIUM: its own
 * word that the sector cannot be read, which the Boardroom names
 * ERR_BAD_SECTOR. */
#define XHCI_MSD_ERR_MEDIUM (-2)

int xhci_msd_read (uint8_t unit, uint64_t lba, uint32_t count, void* buffer);
int xhci_msd_write(uint8_t unit, uint64_t lba, uint32_t count, const void* buffer);

/*
 * A read for a caller that is not going to stand and watch, and a write the
 * same way.
 *
 * The request is the berth (core/boardroom/roadstead.h): lba, sectors, the DMA
 * page and `answered` with its ctx. The sectors land straight in the page — no
 * bounce, because the whole point is that nobody is here to copy them out
 * afterwards — and a write's sectors go out of it the same way — and
 * `answered` is told when the device has answered. It runs on a K-Core, out of
 * the guide loop, so it may take locks, free memory and wake whoever was
 * waiting — but it never stands over this unit itself: it may run beneath a
 * caller standing over the same unit on the drain core, which pumps the steps
 * of the command ahead of it, and a command of its own would wait behind that
 * caller for ever. It is shaped exactly like the SATA one, so that the
 * Boardroom can hand the same function to either kind of seat without an
 * adapter standing in between: `index` is the USB unit, `slot` is meaningless
 * here and is zero.
 *
 * The request must cover whole device blocks and start on one. Everything
 * BoxOS reads or writes asynchronously is a filesystem block, which does; a
 * request that does not is REFUSED rather than quietly widened, because
 * widening it would mean a bounce buffer and a copy, and that is the sync
 * path's job. A command that ends because the device left the socket is
 * answered ERR_DEVICE_NOT_READY, never as a medium error.
 *
 * Returns OK when the command is on its way, or moored behind the commands
 * ahead of it on the same device, and `answered` WILL run; the berth is the
 * device's until it has. On any other return it will not. Nothing is
 * allocated: the berth is the caller's, and the device runs one command at a
 * time on a job of its own.
 */
error_t xhci_msd_read_async(uint8_t unit, RoadsteadBerth* berth);
error_t xhci_msd_write_async(uint8_t unit, RoadsteadBerth* berth);

/* True when this unit can take the calls above — it exists, it is ready, and
 * the geometry of a filesystem block suits it. */
bool xhci_msd_unit_can_async(uint8_t unit);

/*
 * Look over the asynchronous commands in flight, and end one that is not going
 * to be answered.
 *
 * A command somebody is standing over asks the facts itself, in the loop that
 * is standing there: is the device still in the socket, is its pipe well, is
 * the controller running. One nobody is standing over has nobody to ask them —
 * so this asks on its behalf, and the FIRST of those facts is what ends nearly
 * every such command: a device that has been pulled is finished with here and
 * now, not after a budget. Only a device that is present, well and silent
 * reaches the clock, and on bulk that means it has been saying "not yet".
 *
 * Called from the guide loop and the idle loop, where the reset that follows a
 * give-up — three control transfers — is allowed to happen. Measured by
 * mutation: with the completion suppressed, the machine did not reach the
 * shell at all.
 */
void xhci_msd_watchdog(void);

/* Push the device's own write cache to the medium. A write that has completed
 * is a write the device has accepted, not necessarily one it has kept. 0 when
 * it committed — or when it said, this seating, that it has no such command
 * (ILLEGAL REQUEST): it is then not asked again. A device with no medium in it
 * has committed nothing, and is a refusal. */
int xhci_msd_flush(uint8_t unit);

/* The transport reset — the recovery this driver runs when the two ends have
 * lost track of each other — for a seat the Boardroom asks to reset
 * (BoardroomSeatReset), the device taken whole for it: 0 when the device is
 * there and ready after it. */
int XhciMsdUnitReset(uint8_t unit);

#ifdef CONFIG_VOLUME_TESTS
/* The cache commit answered as if the device had refused SYNCHRONIZE CACHE
 * with this sense — BLOCKFAIL's flush with a sense (block_fail.h): nothing is
 * sent, and the answer is judged and remembered exactly as the device's own
 * would be; a state change is asked again, of the device itself. A device
 * that already said it has no such command is not asked, and neither is this.
 * And that memory forgotten, so the unit is asked again. */
int  xhci_msd_flush_answered(uint8_t unit, uint8_t key, uint8_t asc, uint8_t ascq);
void xhci_msd_forget_flush_answer(uint8_t unit);
#endif

#endif
