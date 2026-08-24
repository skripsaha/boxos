#ifndef XHCI_MSD_H
#define XHCI_MSD_H

#include "ktypes.h"
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
const char* xhci_msd_unit_name(uint8_t unit);

/* Sector I/O. Counts are in 512-byte sectors; a request larger than the
 * bounce buffer is split, so there is no size a caller has to know about.
 * Returns 0 on success. */
int xhci_msd_read (uint8_t unit, uint64_t lba, uint32_t count, void* buffer);
int xhci_msd_write(uint8_t unit, uint64_t lba, uint32_t count, const void* buffer);

/* Push the device's own write cache to the medium. A write that has completed
 * is a write the device has accepted, not necessarily one it has kept. */
int xhci_msd_flush(uint8_t unit);

#endif
