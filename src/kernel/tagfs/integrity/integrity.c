#include "integrity_internal.h"
#include "../box_hash/box_hash.h"
#include "../mount/tally_read.h"
#include "gloss.h"
#include "../../lib/kernel/klib.h"
#include "tagfs_volume.h"

/*
 * ‼ A BLOCK'S DIGEST IS THE BOOK'S. The digest a block was written with is the
 * one the Tally's DIGESTS names (tally_read.h, TallyDigestFind — a leaf read
 * off the medium when it is not held, with no lock held here), the function
 * seeded with the volume.
 *
 * A block a record of the ribbon put in is held to the digest that record
 * carries (gloss.h) before the Tally is asked: the Tally learns it only at
 * the next Tally, and a later write of the block stands over the Tally's.
 *
 * Every block is written with its digest (§2.8: a Born or a Shaped carries
 * them), and a run SHARED reads the block's one digest, as the DATA run that
 * wrote it does (§8.2) — a block, not a run, is what a digest names; and 0 is
 * a digest like any other.
 * So a block no element names is refused as one whose digest does not match
 * (volume-read's census says the same of it: bookcensus.py _Digests); only
 * a block whose DIGESTS leaf rots reads unchecked, as §4.5 п.3 has it, said
 * once for the leaf. A leaf that cannot be read now — no memory, a medium
 * that would not answer — is said, and the block refused rather than given
 * unchecked.
 */
bool IntegrityVerify(TagFSVolume* vol, uint64_t block, const void *data)
{
    uint64_t       stored  = 0;
    error_t        refused = OK;
    BoxHashContext ctx;

    if (!data)
        return true;
    if (GlossFind(vol, block, &stored))
        goto named;
    switch (TallyDigestFind(vol, block, &stored, &refused)) {
    case TALLY_DIGEST_ROTTEN:
        return true;
    case TALLY_DIGEST_UNNAMED:
        __atomic_fetch_add(&vol->integrity.errors, 1, __ATOMIC_RELAXED);
        IntegrityUndigestedName(vol, block);
        return false;
    case TALLY_DIGEST_UNREAD:
        kprintf("[Integrity] block %llu cannot be held to its digest now: %s the DIGESTS leaf that names "
                "it — a read of it is refused ERR_CHECKSUM rather than answered unchecked\n",
                (unsigned long long)block, refused == ERR_NO_MEMORY ? "there is no memory to read"
                                                                    : "the medium would not give up");
        return false;
    case TALLY_DIGEST_NAMED:
        break;
    }
named:
    BoxHashInit(&ctx, vol->state.uuid, 16);
    uint64_t h = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &ctx);
    if (h == stored)
        return true;
    __atomic_fetch_add(&vol->integrity.errors, 1, __ATOMIC_RELAXED);
    IntegrityRotName(vol, block, stored, h);
    return false;
}

uint32_t IntegrityErrorCount(TagFSVolume* vol) { return vol->integrity.errors; }
