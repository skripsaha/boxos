#ifndef TAGBOOT_LECTERN_H
#define TAGBOOT_LECTERN_H

/*
 * The Lectern — the UEFI loader's reader of a format-v2 volume's book, and
 * Attest, its holding of the kernel's blocks to the digests the book keeps of
 * them before the loader jumps (spec_v2 §16.7 пп.4, 6; §9.2 п.3).
 *
 * It reads the book as the kernel's mount stands on it (§1.4): every slot of
 * the Spine judged by the one rule every reader of a slot keeps
 * (volume_spine.h, SpineSlotStep), those that hold tried newest first
 * (SpineSlotNewer), the first whose authoritative roots all read
 * (SpineTreeAuthoritative) stood on. Then, for each block of the kernel, the
 * DIGESTS tree of that slot's Tally walked from its root to the leaf whose
 * range holds the block (§2.9, §2.5: a node from the first copy that holds,
 * its seq the one its pointer names, its keys inside the range its parent
 * gives it), the block's digest found in the leaf (volume_tally.h,
 * TallyDigestIn), and BoxHashIntegrity of the block as loaded — all 4096
 * bytes, the zeros past the kernel's end too, as mkfs took it — held to it.
 *
 * NOTHING OF UEFI HERE. The medium, memory and the console are the caller's,
 * handed in a desk: TagBoot reads by Block IO and gives pages
 * (tagboot_attest.c), the host stand reads an image file and gives the heap
 * (tools/tagboot_stand.c) — one text, so what the stand is held to against
 * volume-read's oracle (tools/volumeread/bootread.py) is what TagBoot does.
 * The loop that walks the book is the loader's own: the kernel's mount reads
 * on a heap and holds every file in memory, and a loader needs a few nodes
 * (§16.7 п.2, F1); the decisions — a slot's shape, the order of slots, the
 * trees that must read, the element that names a block — are the shared
 * headers', as the kernel takes them.
 *
 * Integer types come from the includer, as volume_deed.h says.
 */

#include "volume_spine.h"
#include "volume_digest.h"

/*
 * What the Lectern is handed: how to read the volume — `sectors` 512-byte
 * sectors from `vlba`, counted from the ground's first, into `into`; 1 read,
 * 0 the medium would not — room for what it reads, given back when it is done
 * (take answers NULL for none: the read is then said not to be made), and a
 * line said, with no prefix and no end of line. `uuid` seeds every sector's
 * sum and every block's digest; `frame` is the Deed's, already held to
 * VolumeFrameCheck; `block_end` the block where the far copy of the Deed sits
 * — no copy of a node is read at or past it.
 */
typedef struct {
    void          *ctx;
    int          (*read)(void *ctx, uint64_t vlba, uint32_t sectors, uint8_t *into);
    uint8_t     *(*take)(void *ctx, uint64_t bytes);
    void         (*give)(void *ctx, uint8_t *room, uint64_t bytes);
    void         (*say)(void *ctx, const char *line);
    const uint8_t *uuid;
    VolumeFrame    frame;
    uint64_t       block_end;
} LecternDesk;

/* What Attest found (§16.7 п.4, F2). */
typedef enum {
    ATTEST_HELD,        /* every block matches the digest the book keeps of it */
    ATTEST_BROKEN,      /* a block does not: the loader does not jump into it */
    ATTEST_UNCHECKED,   /* nothing to hold some block to, and none found broken: not attested, and loaded */
} AttestSays;

#define LECTERN_LINE_BYTES 256u

typedef struct {
    AttestSays says;
    uint64_t   block;                      /* BROKEN, UNCHECKED: the kernel's block it is about */
    char       line[LECTERN_LINE_BYTES];   /* the verdict as it was said: what a loader stops with */
} AttestVerdict;

/*
 * The kernel `who` names — `blocks` blocks of the volume from `first`, loaded
 * at `image`, 4096 bytes each — held to the book on the desk: every line said
 * as it is read, the verdict last, and kept in *verdict. The blocks are taken
 * in order; the first that does not match decides at once (BROKEN). A block
 * the book cannot vouch for — no slot stands, its DIGESTS range does not read,
 * its Tally names no digest of it — leaves the kernel UNCHECKED, the first
 * such reason kept, only when no later block is found broken.
 */
void LecternAttest(const LecternDesk *desk, const char *who, uint64_t first, uint64_t blocks,
                   const uint8_t *image, AttestVerdict *verdict);

#endif /* TAGBOOT_LECTERN_H */
