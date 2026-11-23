#ifndef TAGFS_MOUNT_TALLY_READ_H
#define TAGFS_MOUNT_TALLY_READ_H

#include "../tagfs.h"
#include "volume_tally.h"

/*
 * TallyRead — the Tally a slot names, walked down from its roots: every node
 * by the BookPtr its parent gives it, the copies in their order, a copy held
 * to the pointer's seq and to the range of keys its parent's separators give
 * it (§16.3 п.2), the leaves' items handed to their reader in the order of the
 * tree's keys.
 *
 * A copy that does not hold is said, with why, and the next copy read — the
 * first part of Self-Heal, which the next Tally's writer finishes by writing
 * the node again (§2.5). A node no copy of which holds is the end of the
 * walk below it: a ROOT that does not read refuses the slot (the mount reads
 * the next, §1.4); a node BELOW a root is a rotten range of keys, handed to
 * the reader with the count its parent carries — never the volume refused
 * (H-4). What a node says against the format's own shape — a lead its
 * records do not give a sector, a count that is not what lies under it
 * (§16.3 п.3-4) — is said, and the node read all the same: the two readers of
 * a book, this and volume-read's (tally.py TreeRead), judge a node alike.
 *
 * And the DIGESTS tree, read the way §4.5 п.3 and §8.1 ask: its inner nodes
 * at the mount, each leaf kept by the range of blocks it covers; the leaf a
 * block lies in read when the block is (TallyDigestFind) — IntegrityVerify's
 * question — and every leaf read only when a FILES range rots, for the blocks
 * with a digest that no living file names are held then (§16.1 п.5,
 * TallyDigestsEach).
 *
 * The walk is the mount's, with the door shut; TallyDigestFind is any
 * reader's, inside the door, with no TagFS lock held — it may read the
 * medium.
 */

/*
 * A node by its pointer, held as numbers: its Sheets, its seq, and every copy
 * the pointer names, in the order they are tried (§16.2 п.17) — each one
 * read when the ones before it do not hold, and every one claimed as the
 * Tally's ground: a copy left out would read as nobody's, and be handed out
 * under the node it is. The two a writer writes (§2.5) lie in the place
 * itself; a pointer naming more keeps the rest in the mount's own list, which
 * lives as long as the volume's mount (TallyDigestsLetGo).
 */
#define TALLY_COPIES_HELD  2u

typedef struct {
    uint64_t seq;
    uint64_t sheets;
    uint32_t copies;                     /* the pointer's own count, every one tried */
    uint32_t more;                       /* where the copies past TALLY_COPIES_HELD begin
                                          * in the mount's list */
    uint64_t block[TALLY_COPIES_HELD];
} TallyPtr;

/* The block copy `k` of a pointer lies at, k < p->copies. */
uint64_t TallyPtrBlock(TagFSVolume* vol, const TallyPtr* p, uint32_t k);

/* Where a node lies in its tree: its pointer, its level, the count its parent
 * carries for it (its slot's SpineRoot, for a root), and the keys its parent's
 * separators give it — [lo, hi), no lo for a tree's first node, no hi for
 * its last. */
typedef struct {
    TallyPtr ptr;
    uint64_t level;
    uint64_t count;
    TallyKey lo;
    TallyKey hi;
    bool     has_lo;
    bool     has_hi;
} TallyPlace;

/* What a walk hands its reader: every leaf item, in key order, with the seq
 * of the node it lies in (its Parcel's runs count their births from it,
 * §2.9); every rotten range; every node it reaches, by its place — read or
 * not, so the ground every copy of it takes is claimed; and every node read
 * from a copy past one that did not hold, by its place — `failed` copies before
 * it did not, and the next Tally writes it again (§2.5). A callback's refusal
 * ends the walk with it. */
typedef struct {
    void*   ctx;
    error_t (*item)(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq);
    error_t (*rotten)(void* ctx, const TallyPlace* at, const char* why);
    error_t (*node)(void* ctx, const TallyPlace* at);
    void    (*mend)(void* ctx, uint64_t tree, const TallyPlace* at, uint32_t failed);
    bool    said;      /* the Tally the mount stands on: what the walk finds against the
                        * format is said, and what it cannot carry on puts the volume
                        * under glass — of any other slot's Tally, neither */
    uint32_t* shapes;  /* when `said`: counts each shape said against the format */
} TallySink;

/* The place of a tree's root, out of its slot's SpineRoot. OK, or
 * ERR_NO_MEMORY for no room for the copies past the two a place holds. */
error_t TallyRootPlace(TagFSVolume* vol, const SpineRoot* root, TallyPlace* out);

/* Whether the root of `tree` reads in some copy, as a node of that tree and
 * of the level its slot names: OK, or ERR_TAGFS_CORRUPTED with why in `why`
 * (each copy's), or ERR_NO_MEMORY, or the medium's refusal. Nothing said. */
error_t TallyRootReads(TagFSVolume* vol, uint64_t tree, const SpineRoot* root, char* why, size_t room);

/* The tree walked from its root: its leaves read and handed over when
 * `leaves`, else named by their places alone. OK, or a refusal — the medium's,
 * memory's, or a callback's. A root that does not read here is a rotten
 * range of the whole tree: the mount asked TallyRootReads first. */
error_t TallyRead(TagFSVolume* vol, uint64_t tree, const SpineRoot* root, bool leaves, const TallySink* sink);

/* The leaf at `at` of `tree` read alone, from the first of its copies that
 * holds: its items handed to sink->item in key order and the copies before it
 * to sink->mend — or, no copy holding, the leaf handed to sink->rotten. OK, or
 * memory's, the medium's or a callback's refusal. Nothing said: the Binder's
 * self-check reads the Tally before the one it lays so (settle/binder_check.c). */
error_t TallyLeafRead(TagFSVolume* vol, uint64_t tree, const TallyPlace* at, const TallySink* sink);

/* Whether copy `k` of the node at `p` holds — a whole SHEET unit of its seq,
 * parsed as a node in its range — *holds, and why not in `why`: OK, or
 * memory's or the medium's refusal. The Binder reads both copies of a leaf no
 * Tally lays again so (settle/binder_proof.c). */
error_t TallyCopyHolds(TagFSVolume* vol, const TallyPlace* p, uint32_t k, bool* holds, char* why, size_t room);

/* A tree's name, as the book's lines say it. */
const char* TallyTreeName(uint64_t tree);

/* --- DIGESTS ------------------------------------------------------------ */

/* A DIGESTS leaf the mount's walk named, kept by place for the reads to come
 * — or, `rotten`, a range of the tree no copy of whose node reads, whose
 * blocks read unchecked (TallyRead's callbacks over DIGESTS). OK or
 * ERR_NO_MEMORY. */
error_t TallyDigestsKeep(TagFSVolume* vol, const TallyPlace* leaf, bool rotten);

/* Every block a DIGESTS leaf names, and every range of blocks a rotten leaf
 * — or an element of one that does not read — would have named, read now off
 * every kept leaf: `run` for each digested run, `rot` for each range, with
 * `ctx`. Said for each leaf that rots. OK, or the refusal of a leaf that
 * could not be read — memory's or the medium's. The mount's, when a FILES
 * range rots (§16.1 п.5). */
error_t TallyDigestsEach(TagFSVolume* vol, void (*run)(void* ctx, uint64_t start, uint64_t count),
                         void (*rot)(void* ctx, uint64_t start, uint64_t count, bool to_end), void* ctx);

/* What the Tally says of one block's digest (TallyDigestFind). */
typedef enum {
    TALLY_DIGEST_NAMED,     /* an element names the block: its digest given */
    TALLY_DIGEST_UNNAMED,   /* the leaf that would name it reads, and no element of it does */
    TALLY_DIGEST_ROTTEN,    /* that leaf, or its element, reads in no copy — said once for
                             * the leaf: its blocks read unchecked (§4.5 п.3) */
    TALLY_DIGEST_UNREAD,    /* that leaf could not be read now: *refused says why — memory, or
                             * the medium */
} TallyDigestSays;

/* The digest the Tally holds for `block`, in *digest when it is NAMED. */
TallyDigestSays TallyDigestFind(TagFSVolume* vol, uint64_t block, uint64_t* digest, error_t* refused);

/* The nodes of the DIGESTS the reads stand on that no copy of holds — a leaf
 * read now to know it, one already known so, an inner node the mount's walk
 * found so — weighed one at a time: the one at *leaf handed to `rot` by its
 * place when it is so, and *leaf moved past it; *done once none is left. The
 * nodes a Tally names as they lie (§16.4 п.5, settle/binder.c), a leaf a turn
 * of the Settle's holder, with no TagFS lock held. OK, or the medium's,
 * memory's or the callback's refusal. */
error_t TallyDigestsRottenNext(TagFSVolume* vol, uint32_t* leaf, error_t (*rot)(void* ctx, const TallyPlace* at), void* ctx,
                               bool* done);

/* The `index`-th leaf of the DIGESTS the reads stand on, in key order —
 * *rotten for a range of it no copy of whose node reads: false past the last;
 * and how many there are.
 * By the Settle's holder, who alone turns them (TallyDigestsTurn). */
bool    TallyDigestLeafAt(TagFSVolume* vol, uint32_t index, TallyPlace* out, bool* rotten);
uint32_t TallyDigestLeaves(TagFSVolume* vol);

/* The leaf of the DIGESTS the reads stand on whose range holds `block` — or,
 * none holding it, the first whose range begins past it — read off the medium
 * as a read of a digest reads it, with no TagFS lock held: each element's run
 * of digested blocks from `block` on handed to `run` (its first block, how
 * many, their digests little-endian, TallyDigestAt) in key order, an element
 * that does not read ending the hand; *next the block past the leaf's range —
 * UINT64_MAX past the book's last leaf, or with no leaf at or past `block` —
 * and *sheets the Sheets the leaf takes. A leaf no copy of which reads hands
 * nothing, said once as a read says it. OK, or memory's or the medium's
 * refusal. Dedup's walk of the book (dedup/dedup.h), from any core, inside
 * the door. */
typedef void (*TallyDigestsRun)(void* ctx, uint64_t start, uint64_t n, const uint8_t* digests);
error_t TallyDigestsFrom(TagFSVolume* vol, uint64_t block, TallyDigestsRun run, void* ctx, uint64_t* next, uint64_t* sheets);

/* How many ranges of the DIGESTS the reads stand on are known to read in no
 * copy — a leaf, or an inner node's whole range — named so by the mount's
 * walk, or found so by a read since: each one Escheat lays anew of the data.
 * From any core: Escheat's answer (settle/escheat.c). */
uint32_t TallyDigestsRotKnown(TagFSVolume* vol);

/* The reads of a digest turned to a Tally this mount wrote: its DIGESTS
 * leaves `leaves`, in key order — `rotten` those named as they lie — in place
 * of the Tally's they read before (tally_read.c). The table they go in is the
 * one the turn before left: ERR_BUSY, nothing changed, while a read that
 * entered it then is still in its leaf — the holder turns them at its next
 * turn rather than wait here. By the Settle's holder. OK, or ERR_NO_MEMORY
 * with the reads left where they were. */
error_t TallyDigestsTurn(TagFSVolume* vol, const TallyPlace* leaves, const bool* rotten, uint32_t n);

/* The kept leaves, the leaves read and the copies of pointers past the two a
 * place holds, let go — the volume's let go of. */
void    TallyDigestsLetGo(TagFSVolume* vol);

#ifdef CONFIG_VOLUME_TESTS
/* The `index`-th node of `tree` the mount's walk of the Tally it stands on
 * reached, in the order it reached them — the root first: its place. False
 * past the last. */
bool    TallyStandNodeWhere(uint64_t tree, uint32_t index, TallyPlace* out);
#endif

#endif /* TAGFS_MOUNT_TALLY_READ_H */
