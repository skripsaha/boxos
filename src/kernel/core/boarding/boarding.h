#ifndef BOARDING_H
#define BOARDING_H

#include "ktypes.h"
#include "boarding_pass.h"

/*
 * Reading the Boarding Pass the loader left.
 *
 * The block lives at a fixed physical address in memory the loaders own, which
 * means the kernel reads it before it has a filesystem, an allocator or a
 * driver — and also that on a machine where the loader failed halfway, or an
 * older loader ran, or the address moved, whatever is sitting there is not a
 * boarding pass and must not be treated as one. Everything below is bounded
 * against the block's own stated lengths, and a pass that does not survive that
 * is reported absent rather than partly believed.
 *
 * Absent is a normal answer. It means the loader did not say, and whoever
 * asked falls back to what it did before there was a pass — which for the
 * Boardroom is a rule it prints while it applies it.
 */

/* Look at the block once, decide whether it is a pass, copy it, and put the
 * loader's journal into the log ring. Called first of all — before memory is
 * managed, because the journal lies in the loader's own pages — and before
 * anything asks. */
void BoardingPassTake(void);

/* Say what is on the pass, or that there is none. */
void BoardingPassSay(void);

/* Did a loader leave one. */
bool BoardingPassPresent(void);

/*
 * The stamp of this kind, or NULL when the pass does not carry one.
 *
 * This is the whole of the reading interface, and it is deliberately generic:
 * a stamp kind this kernel has never heard of is stepped over by its own
 * stated length, so a newer loader cannot break an older kernel and adding a
 * kind needs no change here at all.
 */
const void* BoardingPassStamp(uint16_t kind, uint16_t* out_bytes);

/*
 * The volume this kernel was read out of.
 *
 * True when the loader said, and `out_uuid` is filled with the sixteen bytes
 * the filesystem keeps as its own identity. False when it did not, and the
 * caller has to fall back to guessing — and to saying that it is.
 */
bool BoardingPassVolume(uint8_t out_uuid[16]);

/* Which firmware the loader ran under, and the BIOS drive number it was handed
 * where there was one. False when the pass carries no medium stamp. */
bool BoardingPassMedium(uint8_t* out_firmware, uint8_t* out_bios_drive);

/*
 * What the person asked of the kernel — in gangway.settings, or in Gangway's
 * panel for one start (boarding_pass.h STARTUP, CORES, KEEP_LOG); which of
 * the two, the loader's journal says. With no stamp, the kernel's own way: the
 * screen quiet, every core, the log kept through a reset only in a kernel
 * built to (PRINTTOFILE). Asked as early as BoardingPassTake has run.
 */
uint8_t  BoardingPassStartup(void);    /* BOARDING_STARTUP_* */
uint32_t BoardingPassCores(void);      /* the most cores started, 0 for every one */
bool     BoardingPassKeepLog(void);

/* This start's crossing on its medium (boxos_crossings.h): the sector its slot
 * begins at and its number. False when the pass carries none — a BIOS boot,
 * or a medium whose front keeps no crossings. */
bool BoardingPassCrossing(uint64_t* out_sector, uint64_t* out_number);

/* The scene whose first frame the loader left on the screen (boxos_scene.h):
 * the fid of its file and its bytes. False when the pass carries none — the
 * scene was off or not drawn, or a BIOS boot. */
bool BoardingPassScene(uint64_t* out_fid, uint64_t* out_bytes);

#endif /* BOARDING_H */
