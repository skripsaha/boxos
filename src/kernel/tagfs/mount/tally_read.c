#include "tally_read_internal.h"
#include "../settle/dog_ear.h"
#include "tagfs_volume.h"

/*
 * The Tally's reader, the parts a mount keeps (tally_read.h): a node's pointer
 * held as numbers — the copies past the two a place holds in the mount's own
 * list — the nodes the walk reached, for the stand, and the DIGESTS tree, kept
 * by leaf at the mount and read by leaf when a block is. The walk itself — a
 * node read from the first of its copies that holds, a tree walked from its
 * root — is tally_walk.c (tally_read_internal.h).
 */

static const char* const g_tree_name[] = {
    [TALLY_FILES] = "FILES", [TALLY_TAGS] = "TAGS",       [TALLY_USE] = "USE",
    [TALLY_BOOKMARKS] = "BOOKMARKS", [TALLY_LIEN] = "LIEN", [TALLY_DIGESTS] = "DIGESTS",
    [TALLY_MAP] = "MAP",     [TALLY_SHARED] = "SHARED",
};

const char* TallyTreeName(uint64_t tree)
{
    return tree >= 1 && tree <= TALLY_SHARED ? g_tree_name[tree] : "unknown";
}


uint64_t TallyPtrBlock(TagFSVolume* vol, const TallyPtr* p, uint32_t k)
{
    return k < TALLY_COPIES_HELD ? p->block[k] : vol->tally_read.copies[p->more + (k - TALLY_COPIES_HELD)];
}

/* Room in the list for `n` copies more: false when the heap will not give it. */
static bool TallyCopiesRoom(TagFSVolume* vol, uint64_t n)
{
    if (vol->tally_read.copies_count + n <= vol->tally_read.copies_cap)
        return true;
    if (n > UINT32_MAX - vol->tally_read.copies_count)
        return false;
    uint64_t  want = vol->tally_read.copies_count + n;
    uint64_t  cap  = vol->tally_read.copies_cap ? vol->tally_read.copies_cap : 8u;
    while (cap < want)
        cap *= 2u;
    if (cap > UINT32_MAX)
        cap = want;
    uint64_t* more = kmalloc(sizeof(uint64_t) * cap);
    if (!more)
        return false;
    if (vol->tally_read.copies_count)
        memcpy(more, vol->tally_read.copies, sizeof(uint64_t) * vol->tally_read.copies_count);
    kfree(vol->tally_read.copies);
    vol->tally_read.copies     = more;
    vol->tally_read.copies_cap = (uint32_t)cap;
    return true;
}

/* A pointer as numbers, every copy taken in its order (volume_book.h): the
 * first two into the place, the rest into the list. ERR_NO_MEMORY when the
 * list has no room for them. */
error_t TallyPtrOf(TagFSVolume* vol, const BookPtr* p, TallyPtr* out)
{
    uint32_t at = 0;
    memset(out, 0, sizeof(*out));
    out->seq    = p->seq;
    out->sheets = p->sheets;
    out->copies = (uint32_t)p->copies;        /* BookPtrGet: no more than the bytes they take */
    if (p->copies > TALLY_COPIES_HELD) {
        if (!TallyCopiesRoom(vol, p->copies - TALLY_COPIES_HELD))
            return ERR_NO_MEMORY;
        out->more = vol->tally_read.copies_count;
    }
    for (uint32_t k = 0; k < out->copies; k++) {
        uint64_t block = 0;
        at += BookVarintGet(p->blocks + at, p->blocks_bytes - at, &block);
        if (k < TALLY_COPIES_HELD)
            out->block[k] = block;
        else
            vol->tally_read.copies[vol->tally_read.copies_count++] = block;
    }
    return OK;
}

error_t TallyRootPlace(TagFSVolume* vol, const SpineRoot* root, TallyPlace* out)
{
    memset(out, 0, sizeof(*out));
    out->level = root->level;
    out->count = root->items;
    return TallyPtrOf(vol, &root->ptr, &out->ptr);
}

// ----------------------------------------------------------------------------
// The nodes the walk reached, for the stand
// ----------------------------------------------------------------------------

#ifdef CONFIG_VOLUME_TESTS
/* The nodes the walk of the Tally the mount stands on reached — the trees it
 * reads the leaves of — by tree, in the order it reached them. */
typedef struct {
    uint64_t   tree;
    TallyPlace place;
} TallyStandNode;

static TallyStandNode* s_stand_nodes;
static uint32_t        s_stand_count;
static uint32_t        s_stand_cap;

void TallyStandKeep(uint64_t tree, const TallyPlace* p)
{
    if (s_stand_count == s_stand_cap) {
        uint32_t        cap  = s_stand_cap ? s_stand_cap * 2u : 32u;
        TallyStandNode* more = kmalloc(sizeof(TallyStandNode) * cap);
        if (!more)
            return;
        if (s_stand_count)
            memcpy(more, s_stand_nodes, sizeof(TallyStandNode) * s_stand_count);
        kfree(s_stand_nodes);
        s_stand_nodes = more;
        s_stand_cap   = cap;
    }
    s_stand_nodes[s_stand_count++] = (TallyStandNode){ tree, *p };
}

bool TallyStandNodeWhere(uint64_t tree, uint32_t index, TallyPlace* out)
{
    for (uint32_t i = 0; i < s_stand_count; i++) {
        if (s_stand_nodes[i].tree == tree && index-- == 0) {
            *out = s_stand_nodes[i].place;
            return true;
        }
    }
    return false;
}
#endif

// ----------------------------------------------------------------------------
// DIGESTS: kept by leaf at the mount, read by leaf when a block is
// ----------------------------------------------------------------------------



/* A leaf more into table `t`. ERR_NO_MEMORY. */
static error_t TallyDigestPut(TallyDigestTable* t, const TallyPlace* leaf, bool rotten)
{
    if (t->count == t->cap) {
        uint32_t         cap  = t->cap ? t->cap * 2u : 32u;
        TallyDigestLeaf* more = kmalloc(sizeof(TallyDigestLeaf) * cap);
        uint8_t*         said = kmalloc(cap);
        if (!more || !said) {
            kfree(more);
            kfree(said);
            return ERR_NO_MEMORY;
        }
        memset(said, 0, cap);
        if (t->count) {
            memcpy(more, t->leaves, sizeof(TallyDigestLeaf) * t->count);
            memcpy(said, (const uint8_t*)t->said, t->count);
        }
        kfree(t->leaves);
        kfree((void*)t->said);
        t->leaves = more;
        t->said   = said;
        t->cap    = cap;
    }
    t->leaves[t->count++] = (TallyDigestLeaf){ *leaf, rotten };
    return OK;
}

error_t TallyDigestsKeep(TagFSVolume* vol, const TallyPlace* leaf, bool rotten)
{
    return TallyDigestPut(&vol->tally_read.tables[vol->tally_read.table_at], leaf, rotten);
}

/* The table a read takes, counted into it until TallyDigestLeave. */
static TallyDigestTable* TallyDigestEnter(TagFSVolume* vol)
{
    for (;;) {
        uint32_t          at = __atomic_load_n(&vol->tally_read.table_at, __ATOMIC_SEQ_CST);
        TallyDigestTable* t  = &vol->tally_read.tables[at];
        __atomic_add_fetch(&t->readers, 1u, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&vol->tally_read.table_at, __ATOMIC_SEQ_CST) == at)
            return t;
        __atomic_sub_fetch(&t->readers, 1u, __ATOMIC_SEQ_CST);
    }
}

static void TallyDigestLeave(TallyDigestTable* t)
{
    __atomic_sub_fetch(&t->readers, 1u, __ATOMIC_SEQ_CST);
}

static void TallyDigestTableLetGo(TallyDigestTable* t)
{
    kfree(t->leaves);
    kfree((void*)t->said);
    t->leaves = NULL;
    t->said   = NULL;
    t->count = t->cap = 0;
}

/* Every sheet of table `table` — or of both, UINT32_MAX — let go. */
static void TallyDigestSheetsLetGo(TagFSVolume* vol, uint32_t table)
{
    uint8_t* gone[TALLY_DIGEST_SHEETS];
    TagFSLockTake(&vol->tally_read.digest_lock);
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++) {
        gone[i] = NULL;
        if (table != UINT32_MAX && vol->tally_read.sheets[i].table != table)
            continue;
        gone[i]     = vol->tally_read.sheets[i].stream;
        vol->tally_read.sheets[i] = (TallyDigestSheet){ 0, UINT32_MAX, NULL, 0, 0 };
    }
    TagFSLockDrop(&vol->tally_read.digest_lock);
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++)
        kfree(gone[i]);
}

error_t TallyDigestsTurn(TagFSVolume* vol, const TallyPlace* leaves, const bool* rotten, uint32_t n)
{
    uint32_t          other = __atomic_load_n(&vol->tally_read.table_at, __ATOMIC_SEQ_CST) ^ 1u;
    TallyDigestTable* t     = &vol->tally_read.tables[other];
    if (__atomic_load_n(&t->readers, __ATOMIC_SEQ_CST))
        return ERR_BUSY;
    TallyDigestSheetsLetGo(vol, other);
    TallyDigestTableLetGo(t);
    for (uint32_t i = 0; i < n; i++) {
        error_t rc = TallyDigestPut(t, &leaves[i], rotten[i]);
        if (rc != OK) {
            TallyDigestTableLetGo(t);
            return rc;
        }
    }
    __atomic_store_n(&vol->tally_read.table_at, other, __ATOMIC_SEQ_CST);
    return OK;
}

/* The leaf of table `t` whose range holds `block` — the last whose first key
 * is not past it — or UINT32_MAX. Kept in the keys' order. */
static uint32_t TallyDigestLeafOf(const TallyDigestTable* t, uint64_t block)
{
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!t->leaves[mid].place.has_lo || t->leaves[mid].place.lo.k1 <= block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo == 0)
        return UINT32_MAX;
    const TallyPlace* p = &t->leaves[lo - 1u].place;
    return !p->has_hi || block < p->hi.k1 ? lo - 1u : UINT32_MAX;
}

/* What is said of a leaf, once each: its rot, a copy of it that did not hold. */
#define TALLY_DIGEST_SAID_ROT   1u
#define TALLY_DIGEST_SAID_MEND  2u
#define TALLY_DIGEST_SAID_NONE  4u      /* the leaf itself read in no copy: Escheat's to lay anew */

/* A leaf's rot, said once — by why, its blocks read unchecked (§4.5 п.3). */
static void TallyDigestRotSay(TagFSVolume* vol, TallyDigestTable* t, uint32_t leaf, const char* why)
{
    const TallyPlace* p = &t->leaves[leaf].place;
    if (__atomic_fetch_or(&t->said[leaf], TALLY_DIGEST_SAID_ROT, __ATOMIC_ACQ_REL) & TALLY_DIGEST_SAID_ROT)
        return;
    kprintf("[TagFS] the DIGESTS range of blocks [%llu, %s%llu) rots in every copy (%s): those blocks "
            "read unchecked\n", (unsigned long long)(p->has_lo ? p->lo.k1 : 0), p->has_hi ? "" : "the end, ",
            (unsigned long long)(p->has_hi ? p->hi.k1 : vol->state.block_end), why);
}

/*
 * ‼ A LEAF READ PAST A COPY THAT DID NOT HOLD IS WRITTEN AGAIN (§2.5, §16.5
 * п.9). The mount reads the inner nodes of DIGESTS alone, so a leaf's first
 * copy is first found wanting by the read of a block it names: its range goes
 * into DogEar here — the next Tally writes the leaf again — and the copy is
 * said once.
 */
static void TallyDigestMended(TagFSVolume* vol, TallyDigestTable* t, uint32_t leaf, TallyReading* r)
{
    const TallyPlace* p = &t->leaves[leaf].place;
    DogEarMended(vol, TALLY_DIGESTS, p);
    if (__atomic_fetch_or(&t->said[leaf], TALLY_DIGEST_SAID_MEND, __ATOMIC_ACQ_REL) & TALLY_DIGEST_SAID_MEND)
        return;
    TallyCopiesWords(&r->said, r->words, sizeof(r->words));
    kprintf("[TagFS] the DIGESTS leaf of blocks [%llu, %s%llu): %u of its copies do not hold (%s) — read from "
            "the next, and the next Tally writes the leaf again\n", (unsigned long long)(p->has_lo ? p->lo.k1 : 0),
            p->has_hi ? "" : "the end, ", (unsigned long long)(p->has_hi ? p->hi.k1 : vol->state.block_end),
            r->said.failed, r->words);
}

/* Leaf `leaf` read off the medium: OK with its stream in *stream — NULL when
 * it does not read in any copy, said — or the refusal: memory's, or the
 * medium's. */
static error_t TallyDigestLeafRead(TagFSVolume* vol, TallyDigestTable* t, uint32_t leaf, uint8_t** stream, uint32_t* room)
{
    TallyReading* r = kmalloc(sizeof(TallyReading));
    *stream = NULL;
    if (!r)
        return ERR_NO_MEMORY;
    error_t rc = TallyNodeRead(vol, &t->leaves[leaf].place, &r->node, &r->said);
    if (rc == OK && r->node.unit.whole && r->said.failed)
        TallyDigestMended(vol, t, leaf, r);
    if (rc == OK && r->node.unit.whole) {
        *stream = r->node.unit.stream;
        *room   = r->node.unit.count * BOOK_PAYLOAD_BYTES;
        r->node.unit.stream = NULL;
    } else if (rc == OK) {
        TallyCopiesWords(&r->said, r->words, sizeof(r->words));
        TallyDigestRotSay(vol, t, leaf, r->words);
    }
    TallyNodeLetGo(&r->node);
    kfree(r);
    return rc;
}

/* Whether leaf `leaf` of table `table` is held, its digest of `block` asked
 * under the lock: 1 found, 0 not named, -1 its element does not read, -2 not
 * held. */
static int TallyDigestCached(TagFSVolume* vol, uint32_t table, uint32_t leaf, uint64_t block, uint64_t* digest)
{
    int found = -2;
    TagFSLockTake(&vol->tally_read.digest_lock);
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++) {
        if (vol->tally_read.sheets[i].table == table && vol->tally_read.sheets[i].leaf == leaf && vol->tally_read.sheets[i].stream) {
            vol->tally_read.sheets[i].used = ++vol->tally_read.sheet_tick;
            found = TallyDigestIn(vol->tally_read.sheets[i].stream, vol->tally_read.sheets[i].room, block, digest);
            break;
        }
    }
    TagFSLockDrop(&vol->tally_read.digest_lock);
    return found;
}

/* A leaf read with no lock held, put in in place of the least lately used —
 * the one it pushes out let go of after the lock. */
static void TallyDigestCache(TagFSVolume* vol, uint32_t table, uint32_t leaf, uint8_t* stream, uint32_t room)
{
    uint8_t* gone = stream;
    TagFSLockTake(&vol->tally_read.digest_lock);
    uint32_t oldest = 0;
    bool     held   = false;
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++) {
        held |= vol->tally_read.sheets[i].table == table && vol->tally_read.sheets[i].leaf == leaf && vol->tally_read.sheets[i].stream;
        if (vol->tally_read.sheets[i].used < vol->tally_read.sheets[oldest].used)
            oldest = i;
    }
    if (!held) {
        gone = vol->tally_read.sheets[oldest].stream;
        vol->tally_read.sheets[oldest] = (TallyDigestSheet){ table, leaf, stream, room, ++vol->tally_read.sheet_tick };
    }
    TagFSLockDrop(&vol->tally_read.digest_lock);
    kfree(gone);
}

/* TallyDigestFind on table `t`, the read counted into it. */
static TallyDigestSays TallyDigestFindIn(TagFSVolume* vol, TallyDigestTable* t, uint64_t block, uint64_t* digest, error_t* refused)
{
    uint32_t table = (uint32_t)(t - vol->tally_read.tables);
    uint32_t leaf  = TallyDigestLeafOf(t, block);
    if (leaf == UINT32_MAX)
        return TALLY_DIGEST_UNNAMED;
    if (t->leaves[leaf].rotten) {
        TallyDigestRotSay(vol, t, leaf, "a node above it does not read");
        return TALLY_DIGEST_ROTTEN;
    }
    int found = TallyDigestCached(vol, table, leaf, block, digest);
    if (found == -2) {
        uint32_t room   = 0;
        uint8_t* stream = NULL;
        error_t  rc     = TallyDigestLeafRead(vol, t, leaf, &stream, &room);
        if (rc != OK) {
            *refused = rc;
            return TALLY_DIGEST_UNREAD;
        }
        if (!stream) {
            __atomic_fetch_or(&t->said[leaf], TALLY_DIGEST_SAID_NONE, __ATOMIC_ACQ_REL);
            return TALLY_DIGEST_ROTTEN;
        }
        found = TallyDigestIn(stream, room, block, digest);
        TallyDigestCache(vol, table, leaf, stream, room);
    }
    if (found < 0) {
        TallyDigestRotSay(vol, t, leaf, "its element does not read");
        return TALLY_DIGEST_ROTTEN;
    }
    return found == 1 ? TALLY_DIGEST_NAMED : TALLY_DIGEST_UNNAMED;
}

TallyDigestSays TallyDigestFind(TagFSVolume* vol, uint64_t block, uint64_t* digest, error_t* refused)
{
    TallyDigestTable* t    = TallyDigestEnter(vol);
    TallyDigestSays   says = TallyDigestFindIn(vol, t, block, digest, refused);
    TallyDigestLeave(t);
    return says;
}

bool TallyDigestLeafAt(TagFSVolume* vol, uint32_t index, TallyPlace* out, bool* rotten)
{
    const TallyDigestTable* t = &vol->tally_read.tables[__atomic_load_n(&vol->tally_read.table_at, __ATOMIC_ACQUIRE)];
    if (index >= t->count)
        return false;
    *out    = t->leaves[index].place;
    *rotten = t->leaves[index].rotten;
    return true;
}

uint32_t TallyDigestLeaves(TagFSVolume* vol)
{
    return vol->tally_read.tables[__atomic_load_n(&vol->tally_read.table_at, __ATOMIC_ACQUIRE)].count;
}

/* The leaf of table `t` whose range holds `block`, or else the first whose
 * range begins past it — t->count when none does. */
static uint32_t TallyDigestLeafFrom(const TallyDigestTable* t, uint64_t block)
{
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!t->leaves[mid].place.has_lo || t->leaves[mid].place.lo.k1 <= block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo > 0 && (!t->leaves[lo - 1u].place.has_hi || block < t->leaves[lo - 1u].place.hi.k1))
        return lo - 1u;
    return lo;
}

/* A leaf's elements from block `from` on, each run of digested blocks handed
 * to `run`: an element that does not read ends the hand — the blocks past it
 * carry no digest this leaf can give. */
static void TallyDigestsHand(TagFSVolume* vol, const uint8_t* s, uint32_t room, uint64_t from, TallyDigestsRun run, void* ctx)
{
    NodeHead       h;
    TallyKey       prev, key;
    uint32_t       at = 0, body, len;
    uint64_t       n, skip;
    const uint8_t* d;
    if (NodeHeadGet(s, room, &at, &h) == TALLY_BODY_BROKEN)
        return;
    prev = h.first;
    for (uint64_t i = 0; i < h.items && TallyItemGet(s, room, &at, TALLY_DIGESTS, prev, &key, &body, &len); i++) {
        prev = key;
        if (TallyDigestsGet(s + body, len, &n, &d) == TALLY_BODY_BROKEN)
            return;
        if (n == 0 || key.k1 + n <= from)
            continue;
        skip = key.k1 < from ? from - key.k1 : 0;
        run(vol, ctx, key.k1 + skip, n - skip, d + skip * 8u);
    }
}

error_t TallyDigestsFrom(TagFSVolume* vol, uint64_t block, TallyDigestsRun run, void* ctx, uint64_t* next, uint64_t* sheets)
{
    TallyDigestTable* t    = TallyDigestEnter(vol);
    uint32_t          leaf = TallyDigestLeafFrom(t, block);
    error_t           rc   = OK;
    *next   = UINT64_MAX;
    *sheets = 0;
    if (leaf < t->count) {
        const TallyPlace* p    = &t->leaves[leaf].place;
        uint8_t*          s    = NULL;
        uint32_t          room = 0;
        *next   = p->has_hi ? p->hi.k1 : UINT64_MAX;
        *sheets = p->ptr.sheets;
        if (t->leaves[leaf].rotten)
            TallyDigestRotSay(vol, t, leaf, "a node above it does not read");
        else
            rc = TallyDigestLeafRead(vol, t, leaf, &s, &room);
        if (rc == OK && s)
            TallyDigestsHand(vol, s, room, block, run, ctx);
        kfree(s);
    }
    TallyDigestLeave(t);
    return rc;
}

uint32_t TallyDigestsRotKnown(TagFSVolume* vol)
{
    TallyDigestTable* t = TallyDigestEnter(vol);
    uint32_t          n = 0;
    for (uint32_t i = 0; i < t->count; i++)
        n += t->leaves[i].rotten || (t->said && (t->said[i] & TALLY_DIGEST_SAID_NONE));
    TallyDigestLeave(t);
    return n;
}

error_t TallyDigestsRottenNext(TagFSVolume* vol, uint32_t* leaf, error_t (*rot)(TagFSVolume* vol, void* ctx, const TallyPlace* at), void* ctx,
                               bool* done)
{
    TallyDigestTable* t  = TallyDigestEnter(vol);
    error_t           rc = OK;
    if (*leaf < t->count) {
        bool rotten = t->leaves[*leaf].rotten;
        if (!rotten) {
            uint8_t* s    = NULL;
            uint32_t room = 0;
            rc     = TallyDigestLeafRead(vol, t, *leaf, &s, &room);
            rotten = rc == OK && s == NULL;
            kfree(s);
        }
        if (rc == OK && rotten)
            rc = rot(vol, ctx, &t->leaves[*leaf].place);
        *leaf += rc == OK;
    }
    *done = rc == OK && *leaf >= t->count;
    TallyDigestLeave(t);
    return rc;
}

/* One leaf's elements, each a run of digested blocks — and one that does not
 * read, the range up to the next element's key, or the leaf's end. */
static void TallyDigestsOf(TagFSVolume* vol, TallyDigestTable* t, uint32_t leaf, const uint8_t* s, uint32_t room,
                           void (*run)(TagFSVolume* vol, void* ctx, uint64_t start, uint64_t count),
                           void (*rot)(TagFSVolume* vol, void* ctx, uint64_t start, uint64_t count, bool to_end), void* ctx)
{
    const TallyPlace* p = &t->leaves[leaf].place;
    NodeHead          h;
    TallyKey          prev, key;
    uint32_t          at = 0, body, len;
    uint64_t          n, broken = 0;
    bool              open = false;
    const uint8_t*    d;

    NodeHeadGet(s, room, &at, &h);
    prev = h.first;
    for (uint64_t i = 0; i < h.items && TallyItemGet(s, room, &at, TALLY_DIGESTS, prev, &key, &body, &len); i++) {
        prev = key;
        if (open) {
            rot(vol, ctx, broken, key.k1 - broken, false);
            open = false;
        }
        if (TallyDigestsGet(s + body, len, &n, &d) != TALLY_BODY_BROKEN) {
            run(vol, ctx, key.k1, n);
        } else {
            TallyDigestRotSay(vol, t, leaf, "its element does not read");
            broken = key.k1;
            open   = true;
        }
    }
    if (open)
        rot(vol, ctx, broken, p->has_hi && p->hi.k1 > broken ? p->hi.k1 - broken : 0, !p->has_hi);
}

error_t TallyDigestsEach(TagFSVolume* vol, void (*run)(TagFSVolume* vol, void* ctx, uint64_t start, uint64_t count),
                         void (*rot)(TagFSVolume* vol, void* ctx, uint64_t start, uint64_t count, bool to_end), void* ctx)
{
    TallyDigestTable* t = &vol->tally_read.tables[vol->tally_read.table_at];
    for (uint32_t leaf = 0; leaf < t->count; leaf++) {
        const TallyPlace* p    = &t->leaves[leaf].place;
        uint64_t          lo   = p->has_lo ? p->lo.k1 : 0;
        uint32_t          room = 0;
        uint8_t*          s    = NULL;
        error_t           rc   = t->leaves[leaf].rotten ? OK : TallyDigestLeafRead(vol, t, leaf, &s, &room);
        if (rc != OK)
            return rc;
        if (!s) {
            if (t->leaves[leaf].rotten)
                TallyDigestRotSay(vol, t, leaf, "a node above it does not read");
            rot(vol, ctx, lo, p->has_hi && p->hi.k1 > lo ? p->hi.k1 - lo : 0, !p->has_hi);
            continue;
        }
        TallyDigestsOf(vol, t, leaf, s, room, run, rot, ctx);
        kfree(s);
    }
    return OK;
}

void TallyDigestsLetGo(TagFSVolume* vol)
{
    TallyDigestSheetsLetGo(vol, UINT32_MAX);
    TallyDigestTableLetGo(&vol->tally_read.tables[0]);
    TallyDigestTableLetGo(&vol->tally_read.tables[1]);
    vol->tally_read.table_at = 0;
    kfree(vol->tally_read.copies);
    vol->tally_read.copies       = NULL;
    vol->tally_read.copies_count = vol->tally_read.copies_cap = 0;
#ifdef CONFIG_VOLUME_TESTS
    kfree(s_stand_nodes);
    s_stand_nodes = NULL;
    s_stand_count = s_stand_cap = 0;
#endif
}

/* The volume's tally_read part as it begins: what was not nothing. */
void TallyReadVolumeBegin(TagFSVolume* vol)
{
    vol->tally_read.digest_lock = (TagFSLock)TAGFS_LOCK_INIT("digests", LOCK_RANK_LEAF);
    for (uint32_t i = 0; i < TALLY_DIGEST_SHEETS; i++)
        vol->tally_read.sheets[i] = (TallyDigestSheet){ 0, UINT32_MAX, NULL, 0, 0 };
}
