#ifndef TAGFS_MOUNT_UNDER_GLASS_H
#define TAGFS_MOUNT_UNDER_GLASS_H

#include "../../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"

/*
 * ‼ UNDER GLASS — A KERNEL WRITES ONLY WHAT IT CAN CARRY ON.
 *
 * A volume whose book carries something this kernel's writer could not carry
 * on — a record of a kind it does not know and may not step over, a body
 * longer than it knows, a run's extension, a bookmark, a tag wider than it
 * holds, a FRAME longer than its own, ground off the medium's physical
 * blocks — is mounted to be read, and nothing of it is written: what this
 * kernel wrote over it would lose what it does not understand, in silence.
 *
 * TagFSUnderGlass says why, aloud, each reason once, and puts the volume
 * under glass for the rest of its mount — no Ink, no cycle, no write of its
 * metadata or its data, which every door that writes refuses before a byte
 * (and a STRICT kernel stops at as a defect); NULL takes the glass away, as
 * the volume is let go of. Neither the mount's reset of Ink nor a returning
 * medium lifts it: it is a verdict on the book, not on the medium. It is part
 * of what TagFSReadOnly (tagfs.h) answers.
 */
void     TagFSUnderGlass(const char *why);
bool     TagFSIsUnderGlass(void);

/* A door that writes the medium — a block of metadata, a run of data, the
 * port's write — asked to write a volume under glass: a defect of the road
 * that reached it, which every road shuts before (TagFSReadOnly). Said once
 * a mount and stopped at in a STRICT kernel; ERR_TAGFS_READ_ONLY, before a
 * byte. `door` names it. */
error_t  TagFSGlassRefused(const char *door);

/* The halt's word on a volume that stood under glass and wrote its records:
 * this kernel gave it no write — its book holds what this kernel could not
 * carry on, or is one it reads and does not write — so there is nothing to
 * make durable, and no flush is asked of a medium nothing was given to
 * (system_halt.c, which goes no further with the volume). */
void     TagFSGlassHaltSaid(uint8_t seat);

#ifdef CONFIG_VOLUME_TESTS
/*
 * The stand's own knock at those doors: while the core that calls Knocking
 * with true knocks, a door it reaches under glass is refused as every one is
 * — before a byte — and counted, and said as the stand's knock rather than a
 * defect, in any build: the test that knocks is what holds every door shut
 * even where no road above it lets a change through. A door reached by any
 * other core meanwhile is the defect it always is.
 */
void     TagFSGlassKnocking(bool on);
uint32_t TagFSGlassKnocks(void);
#endif

#endif /* TAGFS_MOUNT_UNDER_GLASS_H */
