#ifndef TAGFS_INTEGRITY_INTERNAL_H
#define TAGFS_INTEGRITY_INTERNAL_H

#include "integrity.h"
#include "../tagfs.h"
#include "amp.h"

/*
 * What the map's page cache (integrity.c) and the map laid by the Settle
 * cycles (integrity_lay.c) share, and nobody else.
 */

/* Where the map is, and how much of it there is, are two fields of the volume's
 * Ledger (volume_ledger.h). They used to be two four-byte windows into the
 * superblock's reserved[] area, at offsets this file chose and the CoW layer
 * had to be told to stay out of. They are the volume's state, changed under
 * its lock like every other field of the Ledger. */
#define SB_MAP_BLOCK(fs)  ((fs)->ledger.integrity_map_block)
#define SB_MAP_COUNT(fs)  ((fs)->ledger.integrity_map_blocks)

#define ENTRIES_PER_BLOCK (TAGFS_BLOCK_SIZE / (uint32_t)sizeof(uint64_t))  // 512
#define MAP_CACHE_SLOTS   16          // bounded RAM: 16 * 4 KB = 64 KB, ANY disk size
#define SLOT_EMPTY        0xFFFFFFFFu

#define MAP_IO_NONE   0u   /* the slot's page is nobody's transfer */
#define MAP_IO_READ   1u   /* the page is on its way from the medium into g_map_page[slot] */
#define MAP_IO_WRITE  2u   /* g_map_page[slot] is on its way to the medium */

/*
 * One page of the map held in memory: 512 per-block digests. The map is paged
 * on demand: only MAP_CACHE_SLOTS pages live in RAM, so memory is constant
 * regardless of disk size (the old design held total_blocks*8 bytes resident,
 * ~2 GB on a 1 TB disk).
 *
 * ‼ A PAGE GOES TO AND FROM THE MEDIUM WITH THE MAP'S LOCK LET GO.
 *
 * The lock is an interrupts-off spinlock, taken from inside every block read
 * and write. Held across a page's transfer, it kept every core that read or
 * wrote a block spinning with interrupts off for as long as the medium took.
 * So a slot records whose transfer it is out with (`io`, `owner`), and every
 * decision about it is made under the lock while the bytes move without it:
 *
 *   `entries` are the truth in memory, and never a transfer's buffer — a
 *   transfer moves g_map_page[slot], which only the slot's transfer touches;
 *
 *   a digest recorded while the slot's page is still coming in is written
 *   into `entries` at once and marked in `heard`, so the page that lands is
 *   laid UNDER it rather than over it;
 *
 *   `turn` moves whenever what the slot holds changes, so a transfer that
 *   comes back to a slot emptied under it applies nothing.
 *
 * No block read or write ever waits for a slot: they are called under the
 * locks of whoever reads or writes the block, and a wait there would be a
 * wait with interrupts off. A slot out with a transfer is simply not taken.
 *
 * ‼ A DIGEST THAT COULD NOT BE RECORDED MAKES ITS PAGE UNKNOWN, NOT WRONG.
 *
 * When no slot can be had for a block's page — every one out with a transfer,
 * or pushed aside dirty again and again, or a page that would not go out or
 * come in — the block's new digest is lost, and the medium's page still holds
 * the digest of what the block held before. Checked against that, a healthy
 * block would read as rot. So the page is marked forgotten (g_map_forgot): until
 * it is laid again every digest in it reads as unknown, and it is laid again
 * as zeros under whatever was recorded since — the next time it is taken into
 * a slot, which the next flush does for every page still forgotten — so the
 * medium loses the stale digests too.
 *
 * AND A TRANSFER IS A HELD SPAN. A page on its way to or from the medium
 * belongs to its slot until the transfer comes back, and a core stopped by an
 * orderly halt in between would fence that slot for good — its digests never
 * written. So the core holds its scheduler across the transfer
 * (SchedulerHoldHere): the halt lets it finish, and it stops only once the
 * map's lock is let go (HaltStopIfOwedHere).
 */
typedef struct {
    uint32_t          map_idx;                        /* the page it holds; SLOT_EMPTY if none */
    uint32_t          lru;
    uint32_t          turn;                           /* +1 whenever what it holds changes */
    volatile uint8_t  io;                             /* MAP_IO_*: out with a transfer */
    volatile uint32_t owner;                          /* 0, or the transfer's core index + 1 */
    bool              dirty;                          /* entries the medium does not have */
    uint64_t          heard[ENTRIES_PER_BLOCK / 64];  /* recorded while MAP_IO_READ travels */
    uint64_t          entries[ENTRIES_PER_BLOCK];
} MapSlot;

extern volatile bool  g_map_initialized;
extern TagFSLock      g_map_lock;
extern uint64_t       g_map_entries;       /* the blocks the map covers */
extern uint32_t       g_map_first_block;
extern uint32_t       g_map_count;         /* its blocks on the medium */
extern MapSlot        g_map_cache[MAP_CACHE_SLOTS];
extern uint64_t       g_map_page[MAP_CACHE_SLOTS][ENTRIES_PER_BLOCK];
extern uint32_t       g_map_epoch;         /* +1 each IntegrityShutdown */
extern uint64_t      *g_map_forgot;        /* a bit per map page: its digests are unknown */
extern uint32_t       g_map_forgot_count;  /* bits set in g_map_forgot */

static inline uint32_t MapCoreHere(void)
{
    return (uint32_t)amp_get_core_index() + 1u;
}

/* Under g_map_lock. */
static inline bool MapForgotten(uint32_t idx)
{
    return g_map_forgot && (g_map_forgot[idx / 64] & (1ull << (idx % 64)));
}

/* The page cache's own steps (integrity.c), each as it says there. */
MapSlot *MapSlotFor(uint32_t idx, uint32_t epoch, const char *site, bool *mine, bool *no_room);
bool     MapPageIn(MapSlot *m, const char *site);
void     MapTransferDone(MapSlot *m);
bool     MapOwnerStopped(const MapSlot *m);
bool     MapSlotSettled(const void *ctx);

/* The lay's (integrity_lay.c). Under g_map_lock: the new run's block that
 * holds page idx, when the page is carried already and every page-out of it
 * goes there too, else 0; a page-out the new run missed, so the page is
 * carried again by a later step; and whether a block is one of the new run's,
 * which are the map's own (in_map_region). With no TagFS lock held, from the
 * mount: the lay begun — OK, or why not, said. The lay let go of with the
 * volume. */
uint32_t MapLayCarried(uint32_t idx);
void     MapLayCarryAgain(uint32_t idx);
bool     MapLayRunHolds(uint64_t block);
error_t  MapLayBegin(uint32_t count, uint32_t entries, uint32_t old_first, uint32_t old_count);
void     MapLayLetGo(void);

/* The rotten blocks a mount names (integrity_rot.c): begun at IntegrityInit on
 * the "integrity" tag (TAGFS_INVALID_TAG_ID: named in the log only), a
 * mismatch IntegrityVerify found named — said and published once per rot —
 * and ended at IntegrityShutdown, saying how many were named and how many
 * more the set had no room for. A block written again or given back is
 * forgotten (IntegrityUpdate, IntegrityForget): rot in what it holds next is
 * named again. Name and Forget are called with no TagFS lock held. */
void     IntegrityRotBegin(uint32_t tag);
void     IntegrityRotName(uint64_t block, uint64_t stored, uint64_t read);
/* A block of a format-v2 volume no DIGESTS element names, named as rot is:
 * said and published once (IntegrityVerify's road on such a volume). */
void     IntegrityUndigestedName(uint64_t block);
void     IntegrityRotForget(uint64_t start, uint32_t count);
void     IntegrityRotEnd(void);

#endif /* TAGFS_INTEGRITY_INTERNAL_H */
