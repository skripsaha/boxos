#ifndef TAGFS_INTEGRITY_H
#define TAGFS_INTEGRITY_H

#include "../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"

// ============================================================================
// TagFS data integrity — silent bit-rot / torn-write detection on data blocks.
//
// A per-block 64-bit BoxHashIntegrity digest (seeded by the volume's uuid, so
// it is stable across reboots), carried by the book: the record of the ribbon
// that put the block in (gloss.h), then the Tally's DIGESTS (tally_read.h).
// Every block is written with one, so every block read is held to one.
// ============================================================================

// Re-check a block just read from disk. Returns true if it holds what was
// written to it; on a mismatch, or a block the book names no digest for, it
// counts the rot, names the block — one line and one Touch on the "integrity"
// tag, once a mount — and returns false, which the read answers ERR_CHECKSUM.
// Called with no TagFS lock held.
bool     IntegrityVerify(uint64_t block, const void *data);

uint32_t IntegrityErrorCount(void);  // bit-rot detections this session

// The rot this mount names begun afresh, each rotten block said once and
// published on `tag` (TAGFS_INVALID_TAG_ID: said in the log only) — by the
// book's mount; and ended as the volume is let go, saying how many were named
// and how many more the set had no room for. With no TagFS lock held.
void     IntegrityRotBegin(uint32_t tag);
void     IntegrityRotEnd(void);

#endif // TAGFS_INTEGRITY_H
