#include "binder_internal.h"
#include "tagfs_volume.h"

#if BINDER_CHECKS

/*
 * The Binder's self-check (binder_internal.h), a STRICT kernel's and the
 * stand's: a whole Tally held to the one it is laid against.
 *
 * ‼ DOGEAR IS PROVED BEFORE ANYTHING IS SPARED ON IT (§16.5 п.9). A Tally
 * written whole lays every element of the state after its stitch; each one
 * whose key DogEar did not take is the element the Tally before holds under
 * that key, byte for byte — runs, digests, tags — and every element of the
 * Tally before whose key it did not take is in this one too. Anything else is
 * a key that changed and was never put in DogEar: a DEFECT, and no Tally is
 * written over a DogEar the state gives the lie to — the Tally a later step
 * lays of the changed keys alone would keep the old element.
 *
 * Elements, not nodes: greedy packing moves the edges of leaves as what lies
 * before them grows, and a child's BookPtr carries the seq of the Tally that
 * laid it — two nodes over one range differ by their making. And a Parcel's
 * runs count their births from the seq of the node they lie in (§2.9
 * birth_delta), so the Tally before's Parcel is spelled again from this
 * Tally's seq before the two are held to each other.
 *
 * A Tally laid in among the nodes of the one before it (TipIn) is held the
 * same way, every element: where it checks, its slice is every file and its
 * walk hands every element of the state — the ones under a node it names as
 * it lies too, held here and then not laid (binder_nodes.c) — so a key that
 * changed outside DogEar is found under a node a Tally laid in would keep.
 *
 * The Tally before is read through the Foliation — its leaves of FILES, TAGS
 * and USE — and the reads' own table of DIGESTS leaves, a leaf at a time as
 * the keys come to it: every leaf of it read once a Tally, the cost of reading
 * that Tally whole, one leaf in memory, and one read a turn of the Settle's
 * holder at the most — an element whose key lies past the leaf read this turn
 * waits for the next (BinderCheckOwed), so a Tally laid over the ruins of a
 * volume emptied reads the leaves of what it was a turn each, not all of them
 * in one. A leaf no copy of which reads — a range the new Tally names as it
 * lies is one — cannot be held: passed over, said; so is a Parcel of it that
 * does not read again. A copy of a leaf that does not hold is said: the Tally
 * checked lays the leaf's range anew. Memory or the medium refusing a leaf,
 * or memory the room to spell a Parcel again, stands the check aside for the
 * Tally, said: it is a proof, not a part of the Tally — it changes nothing a
 * Tally lays, DogEar least of all.
 */


static void CheckAside(TagFSVolume* vol, const char* why)
{
    vol->binder_check.on = false;
    kprintf("[TagFS] the Tally's self-check stands aside: %s\n", why);
}

static error_t CheckDefect(const BinderWork* w, TallyKey key, const char* what)
{
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: the %s element (%llu, %llu) %s, and DogEar does not hold its key", TallyTreeName(w->tree),
          (unsigned long long)key.k1, (unsigned long long)key.k2, what);
#endif
    kprintf("[TagFS] DEFECT: the %s element (%llu, %llu) %s, and DogEar does not hold its key — Tally %llu is not "
            "written over a DogEar the state gives the lie to\n", TallyTreeName(w->tree), (unsigned long long)key.k1,
            (unsigned long long)key.k2, what, (unsigned long long)w->seq);
    return ERR_TAGFS_CORRUPTED;
}

// ----------------------------------------------------------------------------
// The Tally before, a leaf at a time
// ----------------------------------------------------------------------------

static error_t CheckOldItem(TagFSVolume* vol, void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    (void)vol;
    BinderCheck* c = ctx;
    (void)seq;
    if (c->n == c->capn) {
        uint32_t  cap  = c->capn ? c->capn * 2u : 64u;
        CheckOld* more = kmalloc(sizeof(CheckOld) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (c->n)
            memcpy(more, c->old, sizeof(CheckOld) * c->n);
        kfree(c->old);
        c->old  = more;
        c->capn = cap;
    }
    if ((uint64_t)c->len + len > c->cap) {
        uint64_t cap  = ((uint64_t)c->len + len) * 2u + TALLY_SHEET_PAYLOAD;
        uint8_t* more = cap <= UINT32_MAX ? kmalloc(cap) : NULL;
        if (!more)
            return ERR_NO_MEMORY;
        if (c->len)
            memcpy(more, c->bytes, c->len);
        kfree(c->bytes);
        c->bytes = more;
        c->cap   = (uint32_t)cap;
    }
    memcpy(c->bytes + c->len, body, len);
    c->old[c->n++] = (CheckOld){ key, c->len, len };
    c->len += len;
    return OK;
}

static error_t CheckOldRotten(TagFSVolume* vol, void* ctx, const TallyPlace* at, const char* why)
{
    (void)vol;
    BinderCheck* c = ctx;
    c->rotten = true;
    kprintf("[TagFS] the Tally's self-check passes over a %s leaf of the Tally before at block %llu: it reads in "
            "no copy (%s)\n", TallyTreeName(c->tree), (unsigned long long)(at->ptr.copies ? at->ptr.block[0] : 0),
            why);
    return OK;
}

static void CheckOldMended(TagFSVolume* vol, void* ctx, uint64_t tree, const TallyPlace* at, uint32_t failed)
{
    (void)vol;
    (void)ctx;
    kprintf("[TagFS] the Tally's self-check read the %s leaf of the Tally before at block %llu from its next "
            "copy: %u of its copies do not hold — the Tally it checks lays the leaf's range anew\n",
            TallyTreeName(tree), (unsigned long long)at->ptr.block[0], failed);
}

/* The next leaf of the tree's in hand — read now, the turn's one read, unless
 * the reads know its range rotten — or none left. False with the check stood
 * aside. */
static bool CheckLoad(TagFSVolume* vol)
{
    BinderCheck* c   = &vol->binder_check;
    bool         rot = false;
    c->n = c->next = c->len = 0;
    c->rotten = false;
    c->loaded = c->tree == TALLY_DIGESTS ? TallyDigestLeafAt(vol, c->leaf, &c->at, &rot)
                                         : FoliationLeafAt(vol, c->tree, c->leaf, &c->at);
    c->ended  = !c->loaded;
    if (!c->loaded)
        return true;
    c->leaf++;
    if (rot) {
        c->rotten = true;
        return true;
    }
    c->turn_read = true;
    TallySink sink = { .ctx = c, .item = CheckOldItem, .rotten = CheckOldRotten, .mend = CheckOldMended,
                       .said = false };
    error_t   rc   = TallyLeafRead(vol, c->tree, &c->at, &sink);
    if (rc != OK)
        CheckAside(vol, rc == ERR_NO_MEMORY ? "no memory to read a leaf of the Tally before"
                                       : "the medium would not give up a leaf of the Tally before");
    return rc == OK;
}

/* The cursor begun on `tree`'s leaves. */
static void CheckTree(TagFSVolume* vol, uint64_t tree)
{
    BinderCheck* c = &vol->binder_check;
    c->tree   = tree;
    c->leaf   = 0;
    c->loaded = c->ended = c->rotten = c->draining = false;
    c->n = c->next = c->len = 0;
}

// ----------------------------------------------------------------------------
// An element held to the Tally before's
// ----------------------------------------------------------------------------

/* The Parcel `b` of `len` bytes, in a node of `seq`, spelled again in a node
 * of `seq_new` into the check's room, its length in *out: OK; ERR_NO_MEMORY
 * when memory refuses the room, ERR_TAGFS_CORRUPTED when it does not read. */
static error_t ParcelRespelled(TagFSVolume* vol, const uint8_t* b, uint32_t len, uint64_t seq, uint64_t seq_new, uint32_t* out)
{
    BinderCheck* c  = &vol->binder_check;
    uint32_t     at = 0, put = 0;
    uint64_t     n = 0, from = 0, to = 0;
    if (!TallyTake(b, len, &at, &n) || n > len)
        return ERR_TAGFS_CORRUPTED;
    uint64_t room = (uint64_t)len + 10u * n + 10u;
    if (room > c->spell_room) {
        uint8_t* more = room <= UINT32_MAX ? kmalloc(room) : NULL;
        if (!more)
            return ERR_NO_MEMORY;
        kfree(c->spell);
        c->spell      = more;
        c->spell_room = (uint32_t)room;
    }
    put = BookVarintPut(c->spell, n);
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        if (!TallyRunGet(b, len, &at, &from, seq, &r))
            return ERR_TAGFS_CORRUPTED;
        put += TallyRunPut(c->spell + put, &r, &to, seq_new);
    }
    *out = put;
    return at == len ? OK : ERR_TAGFS_CORRUPTED;
}

/* The new element `it`, whose key DogEar did not take, held to the Tally
 * before's `o`: counted held when the two are one, a DEFECT when they differ.
 * A Parcel of the Tally before that does not read again is passed over, said
 * — the mount read it or this kernel laid it, so it is no question of
 * DogEar's; memory refusing the room to spell it stands the check aside. */
static error_t CheckHeldTo(TagFSVolume* vol, const BinderWork* w, const BinderItem* it, const CheckOld* o)
{
    BinderCheck*   c   = &vol->binder_check;
    const uint8_t* old = c->bytes + o->at;
    uint32_t       len = o->len;
    if (w->tree == TALLY_FILES && it->key.k2 != 0) {
        error_t rc = ParcelRespelled(vol, old, o->len, c->at.ptr.seq, w->seq, &len);
        if (rc == ERR_NO_MEMORY) {
            CheckAside(vol, "no memory to spell a Parcel of the Tally before again");
            return OK;
        }
        if (rc != OK) {
            kprintf("[TagFS] the Tally's self-check passes over the FILES element (%llu, %llu) of the Tally "
                    "before at block %llu: its Parcel does not read again\n", (unsigned long long)it->key.k1,
                    (unsigned long long)it->key.k2, (unsigned long long)c->at.ptr.block[0]);
            return OK;
        }
        old = c->spell;
    }
    if (len != it->len || memcmp(it->body, old, len) != 0)
        return CheckDefect(w, it->key, "differs from the Tally before's");
    c->held++;
    return OK;
}

/* An element of the Tally before that this one does not hold. */
static error_t CheckGone(TagFSVolume* vol, const BinderWork* w, TallyKey key)
{
    return DogEarTakenHolds(vol, w->tree, key) ? OK : CheckDefect(w, key, "is in the Tally before and not in this one");
}

/* The rest of the leaf in hand: elements this Tally does not hold. */
static error_t CheckLeafRest(TagFSVolume* vol, const BinderWork* w)
{
    BinderCheck* c  = &vol->binder_check;
    error_t      rc = OK;
    while (rc == OK && !c->rotten && c->next < c->n)
        rc = CheckGone(vol, w, c->old[c->next++].key);
    c->loaded = false;
    return rc;
}

/* `it` against the leaf in hand, whose range holds its key. */
static error_t CheckInLeaf(TagFSVolume* vol, const BinderWork* w, const BinderItem* it)
{
    BinderCheck* c  = &vol->binder_check;
    error_t      rc = OK;
    while (rc == OK && c->next < c->n && BinderKeyLess(w->tree, c->old[c->next].key, it->key))
        rc = CheckGone(vol, w, c->old[c->next++].key);
    if (rc != OK)
        return rc;
    bool eared = DogEarTakenHolds(vol, w->tree, it->key);
    if (c->next < c->n && !BinderKeyLess(w->tree, it->key, c->old[c->next].key)) {
        const CheckOld* o = &c->old[c->next++];
        return eared ? OK : CheckHeldTo(vol, w, it, o);
    }
    return eared ? OK : CheckDefect(w, it->key, "is not in the Tally before");
}

void BinderCheckBegin(TagFSVolume* vol)
{
    BinderCheckLetGo(vol);
    vol->binder_check.held = 0;
    vol->binder_check.on   = true;
    CheckTree(vol, 0);
    if (FoliationLost(vol))
        CheckAside(vol, FoliationLost(vol));
    else if (DogEarTakenLost(vol))
        CheckAside(vol, "DogEar was lost, and every key is held changed");
}

error_t BinderCheckItem(TagFSVolume* vol, const BinderWork* w, const BinderItem* it)
{
    BinderCheck* c = &vol->binder_check;
    c->owed = false;
    if (c->on && c->tree != w->tree)
        CheckTree(vol, w->tree);
    while (c->on) {
        if (!c->loaded && !c->ended) {
            if (c->turn_read) {
                c->owed = true;
                return OK;
            }
            if (!CheckLoad(vol))
                return OK;
        }
        if (c->ended)
            return DogEarTakenHolds(vol, w->tree, it->key) ? OK : CheckDefect(w, it->key, "is not in the Tally before");
        if (c->at.has_hi && !BinderKeyLess(w->tree, it->key, c->at.hi)) {
            error_t rc = CheckLeafRest(vol, w);
            if (rc != OK)
                return rc;
            continue;
        }
        if (c->at.has_lo && BinderKeyLess(w->tree, it->key, c->at.lo))
            return DogEarTakenHolds(vol, w->tree, it->key) ? OK : CheckDefect(w, it->key, "is not in the Tally before");
        return c->rotten ? OK : CheckInLeaf(vol, w, it);
    }
    return OK;
}

bool BinderCheckOwed(TagFSVolume* vol)
{
    return vol->binder_check.on && vol->binder_check.owed;
}

error_t BinderCheckTreeEnd(TagFSVolume* vol, const BinderWork* w, bool* more)
{
    BinderCheck* c = &vol->binder_check;
    *more = false;
    if (c->on && c->tree != w->tree)
        CheckTree(vol, w->tree);
    if (!c->on)
        return OK;
    error_t rc = c->loaded ? CheckLeafRest(vol, w) : OK;
    if (rc != OK || c->ended || (!c->turn_read && !CheckLoad(vol)))
        return rc;
    *more       = !c->ended;
    c->draining = *more;
    return OK;
}

void BinderCheckTurn(TagFSVolume* vol)
{
    vol->binder_check.turn_read = false;
}

bool BinderCheckDraining(TagFSVolume* vol)
{
    return vol->binder_check.on && vol->binder_check.draining;
}

uint64_t BinderCheckHeld(TagFSVolume* vol)
{
    return vol->binder_check.held;
}

void BinderCheckLetGo(TagFSVolume* vol)
{
    uint64_t held = vol->binder_check.held;
    kfree(vol->binder_check.bytes);
    kfree(vol->binder_check.old);
    kfree(vol->binder_check.spell);
    memset(&vol->binder_check, 0, sizeof(vol->binder_check));
    vol->binder_check.held = held;
}

#endif /* BINDER_CHECKS */
