#ifndef TAGFS_INTEGRITY_H
#define TAGFS_INTEGRITY_H

#include "../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"

// ============================================================================
// TagFS data integrity — silent bit-rot / torn-write detection on data blocks.
//
// A per-block 64-bit BoxHashIntegrity digest (seeded by the volume fs_uuid, so
// it is stable across reboots). The map lives in data blocks allocated lazily
// on first mount and recorded in the superblock reserved area — no on-disk
// layout change, no host-tool or bootloader change.
//
// Semantics: a digest of 0 means "unknown" (block not hashed in this volume's
// lifetime yet) and is never flagged — so pre-existing data is not false-
// positived; coverage grows as blocks are written. Every block write records
// its digest; every block read re-checks it. The map's own blocks and any
// block whose digest is unknown are skipped.
// ============================================================================

error_t  IntegrityInit(void);        // load the on-disk map, or lazily create it
void     IntegrityShutdown(void);

// Record a block's digest after writing it (sync + async write paths).
void     IntegrityUpdate(uint32_t block, const void *data);

// Re-check a block just read from disk. Returns true if OK or unknown; on a
// mismatch it logs + counts the rot and returns false.
bool     IntegrityVerify(uint32_t block, const void *data);

error_t  IntegrityFlush(void);       // persist dirty map blocks

// Publish a Touch event on the well-known "integrity" tag for each bit-rot
// detection recorded since the last drain. Called from a lock-free context
// (tagfs_sync) — never from the read path, which runs under g_state.lock.
void     IntegrityDrainReports(void);

bool     IntegrityIsInitialized(void);
uint32_t IntegrityErrorCount(void);  // bit-rot detections this session

// Mark the map's own blocks in an fsck computed-bitmap so they are not seen as
// orphans (and are protected from reuse even if the on-disk bitmap is stale).
void     IntegrityMarkMapBlocks(uint8_t *computed_bm, uint32_t total_blocks);

#endif // TAGFS_INTEGRITY_H
