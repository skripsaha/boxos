#ifndef TAGFS_INTEGRITY_H
#define TAGFS_INTEGRITY_H

#include "../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"

// ============================================================================
// TagFS data integrity — silent bit-rot / torn-write detection on data blocks.
//
// A per-block 64-bit BoxHashIntegrity digest (seeded by the volume fs_uuid, so
// it is stable across reboots). The map lives in data blocks allocated lazily
// on first mount and named by the volume's Ledger.
//
// Semantics: a digest of 0 means "unknown" (block not hashed in this volume's
// lifetime yet) and is never flagged — so pre-existing data is not false-
// positived; coverage grows as blocks are written. Every block write records
// its digest, and one that cannot be recorded — no page of the map to be had
// for it — makes the whole page it belongs to unknown until the next flush
// lays it again as zeros; every block read re-checks it. The map's own blocks
// and any block whose digest is unknown are skipped.
// ============================================================================

error_t  IntegrityInit(void);        // load the on-disk map, or lazily create it
void     IntegrityShutdown(void);

// Record a block's digest after writing it (sync + async write paths).
// Never waits: it is called under the locks of whoever wrote the block. A
// page of the map it needs moves to or from the medium with the map's own
// lock let go.
void     IntegrityUpdate(uint32_t block, const void *data);

// The digests of the run zeroed — the blocks came to rest (Fallow) — a page at
// a time, paged in as a write's digest would be. With no TagFS lock held.
void     IntegrityForget(uint32_t start, uint32_t count);

// Re-check a block just read from disk. Returns true if OK or unknown; on a
// mismatch it logs + counts the rot and returns false. Never waits, as above.
bool     IntegrityVerify(uint32_t block, const void *data);

// Persist the dirty map pages, waiting for a page another core has out, after
// laying again as zeros every page whose digests went unrecorded.
// OK; ERR_WRITE_FAILED when the medium refused a page, which stays owed;
// ERR_WOULD_BLOCK when a page is out with a transfer this call cannot wait
// for — one beneath it on this very core, or one a stopped core left.
error_t  IntegrityFlush(void);

// Publish a Touch event on the well-known "integrity" tag for each bit-rot
// detection recorded since the last drain. Called from a lock-free context
// (the Settle cycle) — never from the read path, which runs under g_state.lock.
void     IntegrityDrainReports(void);

bool     IntegrityIsInitialized(void);
uint32_t IntegrityErrorCount(void);  // bit-rot detections this session

// The mount's census: the map's own run, which no file names, claimed.
void     IntegrityClaimGround(void);

#endif // TAGFS_INTEGRITY_H
