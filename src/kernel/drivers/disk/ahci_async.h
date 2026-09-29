#ifndef AHCI_ASYNC_H
#define AHCI_ASYNC_H

#include "ktypes.h"
#include "error.h"
#include "roadstead.h"

/*
 * AHCI async submission — a read or a write for a caller that does not stay.
 *
 * The caller fills the berth's request — lba, sectors (at most 65535), the
 * DMA page (below 4 GiB unless the controller advertises S64A), answered and
 * ctx — and hands it over. The berth is the port's from an OK answer until
 * `answered` has run, and is not touched by the driver after that: the owner
 * may hand the same berth over again from inside the answer.
 *
 * OK: the command is issued, or moored in the port's roadstead behind the
 * commands ahead of it, and `answered` WILL run — from the completion
 * interrupt (keep it short, allocation-free), from the port's recovery, or
 * from the port leaving the machine (ERR_DEVICE_NOT_READY for a berth that
 * never reached it). A port with every slot taken, or its door shut for a
 * cache flush, is not a refusal.
 *
 * Anything else: `answered` will not run, and the caller still owns the page.
 *   ERR_DEVICE_NOT_READY  — no such port in service
 *   ERR_INVALID_ARGUMENT  — no berth, no callback, no page, or a bad count
 *   ERR_INVALID_ADDRESS   — a page above 4 GiB on a controller without S64A
 */
error_t ahci_submit_read_async(uint8_t port, RoadsteadBerth* berth);
error_t ahci_submit_write_async(uint8_t port, RoadsteadBerth* berth);

#endif /* AHCI_ASYNC_H */
