#ifndef TAGFS_MOUNT_TALLY_READ_H
#define TAGFS_MOUNT_TALLY_READ_H

#include "../tagfs.h"
#include "volume_tally.h"

/*
 * TallyRead — the Tally a slot names, walked down from its roots (spec_v2 §2.5,
 * §2.9, §4.5 п.3): every node by the BookPtr its parent gives it, the copies
 * in their order, a copy held to the pointer's seq and to the range of keys
 * its parent's separators give it (§16.3 п.2), the leaves' items handed to
 * their reader in the order of the tree's keys.
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
 * question on a format-v2 volume — and every leaf read only when a FILES
 * range rots, for the blocks with a digest that no living file names are
 * held then (§16.1 п.5, TallyDigestsEach).
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
uint64_t TallyPtrBlock(const TallyPtr* p, uint32_t k);

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
 * not, so the ground every copy of it takes is claimed. A callback's refusal
 * ends the walk with it. */
typedef struct {
    void*   ctx;
    error_t (*item)(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq);
    error_t (*rotten)(void* ctx, const TallyPlace* at, const char* why);
    error_t (*node)(void* ctx, const TallyPlace* at);
    bool    said;      /* the Tally the mount stands on: what the walk finds against the
                        * format is said, and what it cannot carry on puts the volume
                        * under glass — of any other slot's Tally, neither */
    uint32_t* mended;  /* when `said`: counts each copy that did not hold, the next read in its place */
    uint32_t* shapes;  /* when `said`: counts each shape said against the format */
} TallySink;

/* The place of a tree's root, out of its slot's SpineRoot. OK, or
 * ERR_NO_MEMORY for no room for the copies past the two a place holds. */
error_t TallyRootPlace(const SpineRoot* root, TallyPlace* out);

/* Whether the root of `tree` reads in some copy, as a node of that tree and
 * of the level its slot names: OK, or ERR_TAGFS_CORRUPTED with why in `why`
 * (each copy's), or ERR_NO_MEMORY, or the medium's refusal. Nothing said. */
error_t TallyRootReads(uint64_t tree, const SpineRoot* root, char* why, size_t room);

/* The tree walked from its root: its leaves read and handed over when
 * `leaves`, else named by their places alone. OK, or a refusal — the medium's,
 * memory's, or a callback's. A root that does not read here is a rotten
 * range of the whole tree: the mount asked TallyRootReads first. */
error_t TallyRead(uint64_t tree, const SpineRoot* root, bool leaves, const TallySink* sink);

/* A tree's name, as the book's lines say it. */
const char* TallyTreeName(uint64_t tree);

/* --- DIGESTS ------------------------------------------------------------ */

/* A DIGESTS leaf the mount's walk named, kept by place for the reads to come
 * — or, `rotten`, a range of the tree no copy of whose node reads, whose
 * blocks read unchecked (TallyRead's callbacks over DIGESTS). OK or
 * ERR_NO_MEMORY. */
error_t TallyDigestsKeep(const TallyPlace* leaf, bool rotten);

/* Every block a DIGESTS leaf names, and every range of blocks a rotten leaf
 * — or an element of one that does not read — would have named, read now off
 * every kept leaf: `run` for each digested run, `rot` for each range, with
 * `ctx`. Said for each leaf that rots. OK, or the refusal of a leaf that
 * could not be read — memory's or the medium's. The mount's, when a FILES
 * range rots (§16.1 п.5). */
error_t TallyDigestsEach(void (*run)(void* ctx, uint64_t start, uint64_t count),
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
TallyDigestSays TallyDigestFind(uint64_t block, uint64_t* digest, error_t* refused);

/* The kept leaves, the leaves read and the copies of pointers past the two a
 * place holds, let go — the volume's let go of. */
void    TallyDigestsLetGo(void);

#ifdef CONFIG_VOLUME_TESTS
/* The `index`-th node of `tree` the mount's walk of the Tally it stands on
 * reached, in the order it reached them — the root first: its place. False
 * past the last. */
bool    TallyStandNodeWhere(uint64_t tree, uint32_t index, TallyPlace* out);
#endif

#endif /* TAGFS_MOUNT_TALLY_READ_H */
