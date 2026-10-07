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

// The on-disk map taken up: OK. A volume with no map has one laid by the
// Settle cycles (below) and answers ERR_WOULD_BLOCK, checking nothing until it
// stands; one that grew past its map is checked by it while a bigger one is
// laid. The refusal otherwise.
error_t  IntegrityInit(void);
void     IntegrityShutdown(void);

// A map laid a page at a time by the Settle cycles (RT-15), each owned by the
// cycle's holder with no TagFS lock held unless said:
//   Owed    a map is being laid — a debt the cadence cycles for;
//   Step    pages laid for up to `budget_us`, at least one;
//   Need    the flush need the pages laid so far hold (the cycle's first flush);
//   Name    S0, under the state lock: every page carried and flushed — the
//           Ledger's fields name the run; true when named;
//   Stands  S6 of a settled cycle, under the state lock: the named run is the
//           map from here, the old one rests; true when it stood;
//   Say     after the lock, when it stood;
//   Drop    the machine's last cycle, under the state lock: an unfinished run
//           given back, since no one will finish it;
//   Blocks  the blocks a run being laid holds, for the stand's count.
bool     IntegrityLayOwed(void);
void     IntegrityLayStep(uint64_t budget_us);
uint64_t IntegrityLayNeed(void);
bool     IntegrityLayName(void);
bool     IntegrityLayStands(void);
void     IntegrityLaySay(void);
void     IntegrityLayDrop(void);
uint32_t IntegrityLayBlocks(void);

// Record a block's digest after writing it (sync + async write paths).
// Never waits: it is called under the locks of whoever wrote the block. A
// page of the map it needs moves to or from the medium with the map's own
// lock let go.
void     IntegrityUpdate(uint32_t block, const void *data);

// The digests of the run zeroed — the blocks came to rest (Fallow) — a page at
// a time, paged in as a write's digest would be. With no TagFS lock held.
void     IntegrityForget(uint32_t start, uint32_t count);

// Re-check a block just read from disk. Returns true if OK or unknown; on a
// mismatch it counts the rot, names the block — one line and one Touch on the
// "integrity" tag, once until the block is written again — and returns false,
// which the read answers ERR_CHECKSUM. Never waits for the map, as above;
// called with no TagFS lock held.
bool     IntegrityVerify(uint32_t block, const void *data);

// Persist the dirty map pages, waiting for a page another core has out, after
// laying again as zeros every page whose digests went unrecorded.
// OK; ERR_WRITE_FAILED when the medium refused a page, which stays owed;
// ERR_WOULD_BLOCK when a page is out with a transfer this call cannot wait
// for — one beneath it on this very core, or one a stopped core left.
error_t  IntegrityFlush(void);

bool     IntegrityIsInitialized(void);
uint32_t IntegrityErrorCount(void);  // bit-rot detections this session

// The mount's census: the map's own run, which no file names, claimed.
void     IntegrityClaimGround(void);

#ifdef CONFIG_VOLUME_TESTS
// For the stand, with no TagFS lock held and no cycle running: a lay of the map
// as it stands begun into a new run and every page carried (OK, ERR_BUSY when
// a lay is up or no map stands, or why not); the new run's first block while it
// is laid, else 0; and the lay let go of unnamed, its run given back. And the
// deepest any core stood inside IntegrityUpdate since the last ask.
error_t  IntegrityStandLayBegin(void);
uint32_t IntegrityStandLayFirst(void);
void     IntegrityStandLayEnd(void);
uint32_t IntegrityStandUpdateDeepest(void);
#endif

#endif // TAGFS_INTEGRITY_H
