#include "dog_ear.h"
#include "../lock_rank/lock_rank.h"
#include "../ink/commonhold.h"
#include "binder.h"
#include "tagfs_volume.h"

/*
 * DogEar (dog_ear.h): the spans of each tree in two sets — the ones held since
 * the last slice, and the ones the Tally under way took — each tree's an array
 * of disjoint spans in order of their keys, no two meeting.
 */




/* The place of `tree` among DogEar's — UINT32_MAX for a tree no Tally of this
 * kernel lays. */
static uint32_t TreeAt(uint64_t tree)
{
    switch (tree) {
    case TALLY_FILES:
        return 0;
    case TALLY_TAGS:
        return 1;
    case TALLY_USE:
        return 2;
    case TALLY_DIGESTS:
        return 3;
    default:
        return UINT32_MAX;
    }
}

static bool KeyLess(TallyKey a, TallyKey b)
{
    return a.k1 < b.k1 || (a.k1 == b.k1 && a.k2 < b.k2);
}

/* The first span of `t` that does not end before `key` — one ending at it
 * meets it. */
static uint32_t SpanFrom(const DogEarTree* t, TallyKey key)
{
    uint32_t lo = 0, hi = t->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (KeyLess(t->span[mid].hi, key))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/* `s` into `t`, which has room for one more: joined with every span it meets
 * or overlaps. */
static void SpanJoin(DogEarTree* t, DogEarSpan s)
{
    uint32_t i = SpanFrom(t, s.lo), j = i;
    while (j < t->n && !KeyLess(s.hi, t->span[j].lo)) {
        if (KeyLess(t->span[j].lo, s.lo))
            s.lo = t->span[j].lo;
        if (KeyLess(s.hi, t->span[j].hi))
            s.hi = t->span[j].hi;
        j++;
    }
    if (j == i) {
        memmove(&t->span[i + 1u], &t->span[i], sizeof(DogEarSpan) * (t->n - i));
        t->n++;
    } else {
        memmove(&t->span[i + 1u], &t->span[j], sizeof(DogEarSpan) * (t->n - j));
        t->n -= j - i - 1u;
    }
    t->span[i] = s;
}

static void SetLetGo(DogEarSet* set)
{
    for (uint32_t i = 0; i < DOG_EAR_TREES; i++)
        kfree(set->tree[i].span);
    memset(set, 0, sizeof(*set));
}

/* Every span held let go and every key held changed: whether that was so
 * already. */
static bool HeldLose(TagFSVolume* vol)
{
    DogEarSet gone;
    TagFSLockTake(&vol->dog_ear.lock);
    bool was    = vol->dog_ear.held.lost;
    gone        = vol->dog_ear.held;
    memset(&vol->dog_ear.held, 0, sizeof(vol->dog_ear.held));
    vol->dog_ear.held.lost = true;
    TagFSLockDrop(&vol->dog_ear.lock);
    gone.lost = false;
    SetLetGo(&gone);
    return was;
}

/* The heap refused DogEar room: said once a loss, and a Tally demanded. */
static void Lose(TagFSVolume* vol)
{
    if (HeldLose(vol))
        return;
    kprintf("[TagFS] no memory to keep the keys the book changed since its last Tally (DogEar): every key is held "
            "changed, and a Tally is demanded — the next is written whole\n");
    BinderDemanded(vol);
}

/* `count` spans into tree `at` of the held set: room for them asked of the
 * heap with no lock, and asked again should a put on another core have taken
 * it meanwhile. */
static void SpansPut(TagFSVolume* vol, uint32_t at, const DogEarSpan* in, uint32_t count)
{
    for (;;) {
        DogEarTree* t    = &vol->dog_ear.held.tree[at];
        uint32_t    n    = __atomic_load_n(&t->n, __ATOMIC_ACQUIRE);
        uint32_t    room = __atomic_load_n(&t->cap, __ATOMIC_ACQUIRE);
        DogEarSpan* more = NULL;
        if (__atomic_load_n(&vol->dog_ear.held.lost, __ATOMIC_ACQUIRE) || count == 0)
            return;
        if ((uint64_t)n + count > room) {
            uint64_t want = ((uint64_t)n + count) * 2u + 16u;
            more = want <= UINT32_MAX ? kmalloc(sizeof(DogEarSpan) * want) : NULL;
            if (!more) {
                Lose(vol);
                return;
            }
            room = (uint32_t)want;
        }
        TagFSLockTake(&vol->dog_ear.lock);
        if (!vol->dog_ear.held.lost && (uint64_t)t->n + count > t->cap) {
            if (!more || (uint64_t)t->n + count > room) {
                TagFSLockDrop(&vol->dog_ear.lock);
                kfree(more);
                continue;
            }
            if (t->n)
                memcpy(more, t->span, sizeof(DogEarSpan) * t->n);
            DogEarSpan* old = t->span;
            t->span = more;
            __atomic_store_n(&t->cap, room, __ATOMIC_RELEASE);
            more = old;
        }
        for (uint32_t i = 0; !vol->dog_ear.held.lost && i < count; i++)
            SpanJoin(t, in[i]);
        __atomic_store_n(&t->n, t->n, __ATOMIC_RELEASE);
        TagFSLockDrop(&vol->dog_ear.lock);
        kfree(more);
        return;
    }
}

void DogEarPut(TagFSVolume* vol, uint64_t tree, TallyKey lo, TallyKey hi)
{
    uint32_t   at = TreeAt(tree);
    DogEarSpan s  = { lo, hi };
    if (at != UINT32_MAX && KeyLess(lo, hi))
        SpansPut(vol, at, &s, 1);
    if (tree == TALLY_USE)
        __atomic_add_fetch(&vol->dog_ear.gone, 1u, __ATOMIC_RELEASE);     /* the word said may be none: its one key gone */
}

void DogEarMended(TagFSVolume* vol, uint64_t tree, const TallyPlace* at)
{
    DogEarPut(vol, tree, at->has_lo ? at->lo : (TallyKey){ 0, 0 }, at->has_hi ? at->hi : DOG_EAR_END);
}

// ----------------------------------------------------------------------------
// The ground born and gone
// ----------------------------------------------------------------------------

static void GroundLetGo(DogEarDelta* d)
{
    kfree(d->born.run);
    kfree(d->gone.run);
    memset(d, 0, sizeof(*d));
}

/* The first run of `g` that ends past `at`. */
static uint64_t GroundFrom(const DogEarGround* g, uint64_t at)
{
    uint64_t lo = 0, hi = g->n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (g->run[mid].start + g->run[mid].count <= at)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/* Room for `more` runs in `g`: false when the heap refuses it. */
static bool GroundRoom(DogEarGround* g, uint64_t more)
{
    if (g->n + more <= g->cap)
        return true;
    uint64_t      cap = (g->n + more) * 2u + 16u;
    BinderGround* run = kmalloc(sizeof(BinderGround) * cap);
    if (!run)
        return false;
    if (g->n)
        memcpy(run, g->run, sizeof(BinderGround) * g->n);
    kfree(g->run);
    g->run = run;
    g->cap = cap;
    return true;
}

/* [start, end) into `g`, joined with every run it meets. */
static bool GroundAdd(DogEarGround* g, uint64_t start, uint64_t end)
{
    if (start >= end || !GroundRoom(g, 1))
        return start >= end;
    uint64_t i = start ? GroundFrom(g, start - 1u) : 0, j = i;
    while (j < g->n && g->run[j].start <= end) {
        start = g->run[j].start < start ? g->run[j].start : start;
        end   = g->run[j].start + g->run[j].count > end ? g->run[j].start + g->run[j].count : end;
        j++;
    }
    memmove(g->run + i + 1u, g->run + j, sizeof(BinderGround) * (g->n - j));
    g->n       = g->n - (j - i) + 1u;
    g->run[i]  = (BinderGround){ start, end - start };
    return true;
}

/* [start, end) out of `g`; what of it `g` held, in order, into `cut` when
 * given. */
static bool GroundCut(DogEarGround* g, uint64_t start, uint64_t end, DogEarGround* cut)
{
    if (!GroundRoom(g, 1))
        return false;
    for (uint64_t i = GroundFrom(g, start); i < g->n && g->run[i].start < end;) {
        uint64_t rs = g->run[i].start, re = rs + g->run[i].count;
        uint64_t cs = rs > start ? rs : start, ce = re < end ? re : end;
        if (cut && !GroundAdd(cut, cs, ce))
            return false;
        if (rs < cs && ce < re) {
            memmove(g->run + i + 2u, g->run + i + 1u, sizeof(BinderGround) * (g->n - i - 1u));
            g->run[i]      = (BinderGround){ rs, cs - rs };
            g->run[i + 1u] = (BinderGround){ ce, re - ce };
            g->n++;
            return true;
        }
        if (rs < cs) {
            g->run[i++].count = cs - rs;
        } else if (ce < re) {
            g->run[i] = (BinderGround){ ce, re - ce };
            return true;
        } else {
            memmove(g->run + i, g->run + i + 1u, sizeof(BinderGround) * (g->n - i - 1u));
            g->n--;
        }
    }
    return true;
}

/*
 * ‼ A BLOCK LET GO AND TAKEN AGAIN HOLDS NEW DATA (fallow.h: on a book a block
 * goes back to the allocator a few stitches after it rests, never waiting for
 * a Tally). So `born` is the ground live now that was taken since the slice,
 * `gone` the ground live at the slice whose data has been let go since — a
 * block let go and taken again is in both: its old digest out, the Gloss's in
 * (§16.5 п.16). The live files took [start, end) (not `gone`): it is born,
 * whatever it was. They lost it (`gone`): what of it was born since comes out
 * of `born` — never live at the slice, or already gone — and the rest is gone.
 */
static bool DeltaMoved(DogEarDelta* d, uint64_t start, uint64_t end, bool gone)
{
    if (!gone)
        return GroundAdd(&d->born, start, end);
    DogEarGround was = { NULL, 0, 0 };
    bool         ok  = GroundCut(&d->born, start, end, &was);
    for (uint64_t i = 0, at = start; ok && at < end; i++) {
        uint64_t stop = i < was.n ? was.run[i].start : end;
        ok  = GroundAdd(&d->gone, at, stop);
        at  = i < was.n ? was.run[i].start + was.run[i].count : end;
    }
    kfree(was.run);
    return ok;
}

// ----------------------------------------------------------------------------
// A change of a file
// ----------------------------------------------------------------------------

/* What a change of a file puts in, gathered with no lock before it is: the
 * spans of FILES and of DIGESTS, and the DATA Strips of the Parcels it changed
 * on either side. */
typedef struct {
    DogEarSpan* files;
    uint32_t    nfiles, capfiles;
    DogEarSpan* digests;
    uint32_t    ndigests, capdigests;
    Strip*      ground[2];
    uint64_t    nground[2], capground[2];
    bool        refused;
    bool        went;       /* a key of the state may have gone: the file given up, or ground of it let go
                             * that may leave a leaf of DIGESTS with none (LeafMayEmpty) */
    uint64_t    slice;      /* the last Ink the change's stitch carries (commonhold.h) */
} DogEarChange;

static void SpanNoted(DogEarChange* c, bool digests, TallyKey lo, TallyKey hi)
{
    DogEarSpan** list = digests ? &c->digests : &c->files;
    uint32_t*    n    = digests ? &c->ndigests : &c->nfiles;
    uint32_t*    cap  = digests ? &c->capdigests : &c->capfiles;
    if (c->refused)
        return;
    if (*n == *cap) {
        uint32_t    room = *cap ? *cap * 2u : 16u;
        DogEarSpan* more = room > *cap ? kmalloc(sizeof(DogEarSpan) * room) : NULL;
        if (!more) {
            c->refused = true;
            return;
        }
        if (*n)
            memcpy(more, *list, sizeof(DogEarSpan) * *n);
        kfree(*list);
        *list = more;
        *cap  = room;
    }
    (*list)[(*n)++] = (DogEarSpan){ lo, hi };
}

/* The Strips of ground of `s` (`count`) — DATA and SHARED — onto side
 * `side`'s ground. */
static void GroundNoted(DogEarChange* c, uint32_t side, const Strip* s, uint64_t count)
{
    for (uint64_t i = 0; i < count && !c->refused; i++) {
        if (!StripOnGround(s[i].kind))
            continue;
        if (c->nground[side] == c->capground[side]) {
            uint64_t room = c->capground[side] ? c->capground[side] * 2u : 64u;
            Strip*   more = kmalloc(sizeof(Strip) * room);
            if (!more) {
                c->refused = true;
                return;
            }
            if (c->nground[side])
                memcpy(more, c->ground[side], sizeof(Strip) * c->nground[side]);
            kfree(c->ground[side]);
            c->ground[side]    = more;
            c->capground[side] = room;
        }
        c->ground[side][c->nground[side]++] = s[i];
    }
}

/* The keys of DIGESTS over the windows of DIGEST_ITEM_MOST blocks [start,
 * end) lies in: [*lo, *hi). */
static void WindowsOf(uint64_t start, uint64_t end, TallyKey* lo, TallyKey* hi)
{
    uint64_t last = (end - 1u) / DIGEST_ITEM_MOST * DIGEST_ITEM_MOST;
    *lo = (TallyKey){ start / DIGEST_ITEM_MOST * DIGEST_ITEM_MOST, 0 };
    *hi = last > UINT64_MAX - DIGEST_ITEM_MOST ? DOG_EAR_END : (TallyKey){ last + DIGEST_ITEM_MOST, 0 };
}

/* The windows of DIGEST_ITEM_MOST blocks [start, end) lies in. */
static void WindowsNoted(DogEarChange* c, uint64_t start, uint64_t end)
{
    TallyKey lo, hi;
    WindowsOf(start, end, &lo, &hi);
    SpanNoted(c, true, lo, hi);
}

/* The replay a mount made asked no count of owners — the census counts them
 * after it (ground_shares.c) — so the ground it put gone may be a block in
 * common another owner still names, and a block born since the slice and let
 * go by one of its owners came out of `born`: every block in common an owner
 * holds now is born and not gone, and its digest is found where a born
 * block's is (§16.5 п.16). The mount's, the door shut. */
void DogEarCommonheld(TagFSVolume* vol)
{
    uint64_t         n = 0;
    const CommonRun* r = CommonholdRuns(vol, &n);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t end = r[i].start + r[i].count;
        if (r[i].owners &&
            (!GroundCut(&vol->dog_ear.delta_held.gone, r[i].start, end, NULL) || !GroundAdd(&vol->dog_ear.delta_held.born, r[i].start, end))) {
            Lose(vol);
            return;
        }
    }
}

void DogEarGroundChanged(TagFSVolume* vol, uint64_t start, uint64_t count)
{
    TallyKey lo, hi;
    if (count == 0)
        return;
    WindowsOf(start, start + count, &lo, &hi);
    DogEarPut(vol, TALLY_DIGESTS, lo, hi);
}

/* The range [*lo, *hi) of the leaf of DIGESTS the reads stand on that `block`
 * lies in — the last whose lower key is not past it, the first from none, as
 * the next Tally is reckoned by them (binder_cost.c, LeafOf); false with no
 * leaf. */
static bool DigestLeafOf(TagFSVolume* vol, uint64_t block, uint64_t* lo, uint64_t* hi)
{
    uint32_t   a = 1, z = TallyDigestLeaves(vol);
    TallyPlace p;
    bool       rotten;
    while (a < z) {
        uint32_t mid = a + (z - a) / 2u;
        if (TallyDigestLeafAt(vol, mid, &p, &rotten) && (!p.has_lo || p.lo.k1 <= block))
            a = mid + 1u;
        else
            z = mid;
    }
    if (!TallyDigestLeafAt(vol, a - 1u, &p, &rotten))
        return false;
    *lo = a > 1u && p.has_lo ? p.lo.k1 : 0;
    *hi = p.has_hi ? p.hi.k1 : UINT64_MAX;
    return true;
}

/* Whether runs `g` (`n`, in order, none meeting) name a block of [lo, hi). */
static bool GroundIn(const BinderGround* g, uint64_t n, uint64_t lo, uint64_t hi)
{
    uint64_t a = 0, z = n;
    while (a < z) {
        uint64_t mid = a + (z - a) / 2u;
        if (g[mid].start + g[mid].count <= lo)
            a = mid + 1u;
        else
            z = mid;
    }
    return a < n && g[a].start < hi;
}

/*
 * Whether ground [start, end) a file let go may have left a leaf of DIGESTS
 * with no key of the state in it — the next Tally's cost may be less
 * (binder.c, TallyDue). Not where it lies inside one leaf that the file still
 * holds a run of ground in (`b`, `nb`: its ground after the change, laid as
 * ground) or that a file took ground in since the slice: a block a live file
 * holds is a key of that leaf. So a write over a block of a file, laid on
 * fresh ground, or a cut that leaves the file a block of the leaf, is no key
 * gone; a file given up, or cut short of every block of a leaf, may be.
 */
static bool LeafMayEmpty(TagFSVolume* vol, const Strip* b, uint64_t nb, uint64_t start, uint64_t end)
{
    uint64_t lo = 0, hi = 0, at = 0, z = nb;
    if (!DigestLeafOf(vol, start, &lo, &hi) || end > hi)
        return true;
    while (at < z) {
        uint64_t mid = at + (z - at) / 2u;
        if (b[mid].start + b[mid].count <= lo)
            at = mid + 1u;
        else
            z = mid;
    }
    return !(at < nb && b[at].start < hi) && !GroundIn(vol->dog_ear.delta_held.born.run, vol->dog_ear.delta_held.born.n, lo, hi);
}

/*
 * Ground [start, end) the file let go: its windows noted, and — ‼ A BLOCK
 * ANOTHER OWNER HELD AT THE STITCH'S SLICE IS NOT GONE (§8.2, §8.1: a block
 * keeps its one element while an owner lives) — what of it no owner held at
 * the slice of the change's stitch gone (commonhold.h, CommonholdOwnedAt):
 * the next Tally keeps the digest of a block in common its other owners
 * still name, and lets it go with the last. `b` (`nb`) the file's ground
 * after the change.
 */
static void GoneNoted(TagFSVolume* vol, DogEarChange* c, uint64_t start, uint64_t end, const Strip* b, uint64_t nb)
{
    uint64_t at = start, lo = 0, hi = 0;
    WindowsNoted(c, start, end);
    while (at < end && !c->refused) {
        uint64_t from  = at;
        uint64_t stop  = CommonholdOwnedAt(vol, &at, end, c->slice, &lo, &hi) ? lo : end;
        if (stop > from) {
            c->refused |= !DeltaMoved(&vol->dog_ear.delta_held, from, stop, true);
            c->went    |= LeafMayEmpty(vol, b, nb, from, stop);
        }
    }
}

/* The ground one side names and the other does not, as windows: both sides
 * laid as ground first — in order, the runs that meet joined. */
static void GroundDiffered(TagFSVolume* vol, DogEarChange* c)
{
    const Strip* a = c->ground[0];
    const Strip* b = c->ground[1];
    uint64_t     na = StripsAsGround(c->ground[0], c->nground[0]);
    uint64_t     nb = StripsAsGround(c->ground[1], c->nground[1]);
    uint64_t     i = 0, j = 0, x = 0;
    while (i < na || j < nb) {
        if (i < na && a[i].start + a[i].count <= x) {
            i++;
            continue;
        }
        if (j < nb && b[j].start + b[j].count <= x) {
            j++;
            continue;
        }
        uint64_t as = i < na ? (a[i].start > x ? a[i].start : x) : UINT64_MAX;
        uint64_t bs = j < nb ? (b[j].start > x ? b[j].start : x) : UINT64_MAX;
        uint64_t ae = i < na ? a[i].start + a[i].count : UINT64_MAX;
        uint64_t be = j < nb ? b[j].start + b[j].count : UINT64_MAX;
        if (as < bs) {
            x = ae < bs ? ae : bs;
            GoneNoted(vol, c, as, x, b, nb);
        } else if (bs < as) {
            x = be < as ? be : as;
            WindowsNoted(c, bs, x);
            c->refused |= !DeltaMoved(&vol->dog_ear.delta_held, bs, x, false);
        } else {
            x = ae < be ? ae : be;
        }
    }
}

/* One record's walk a Parcel at a time: the block the Parcel in hand begins
 * at, and its Strips — the Parcel itself, shared by every record that names
 * it. */
typedef struct {
    StripWalk    w;
    const Strip* s;
    uint32_t     count;
    uint64_t     pos;
    bool         has;
} DogEarSide;

static void SideBegin(const FileRecord* rec, DogEarSide* p)
{
    p->pos = 0;
    StripWalkBegin(rec, &p->w);
    p->has = StripWalkParcel(&p->w, &p->s, &p->count);
}

static void SideNext(DogEarSide* p)
{
    for (uint32_t i = 0; i < p->count; i++)
        p->pos += p->s[i].count;
    p->has = StripWalkParcel(&p->w, &p->s, &p->count);
}

/*
 * ‼ A PARCEL THE TWO RECORDS SHARE AT ONE PLACE IS NOT CHANGED (§2.9, H-5). A
 * change built `now` from `was` copying the Parcels it touched and sharing
 * every other (FileRecordSplice), so the two walk side by side a Parcel a step
 * — as FileRecordSplices walks them — and the key of every Parcel of either
 * that the other does not hold at the same block is changed: a Parcel built
 * anew, cut or joined, and every Parcel past a splice that moved the blocks
 * after it. A run of such keys is one span, closed at the next key both
 * share, or at the file's last; their DATA Strips gathered for DIGESTS.
 */
static void ParcelsChanged(DogEarChange* c, uint64_t fid, const FileRecord* was, const FileRecord* now,
                           TallyKey end)
{
    DogEarSide a, b;
    TallyKey   open   = { 0, 0 };
    bool       opened = false;
    SideBegin(was, &a);
    SideBegin(now, &b);
    while (a.has || b.has) {
        if (a.has && b.has && a.pos == b.pos && a.s == b.s) {
            if (opened)
                SpanNoted(c, false, open, (TallyKey){ fid, 1u + a.pos });
            opened = false;
            SideNext(&a);
            SideNext(&b);
            continue;
        }
        uint64_t pos = !b.has || (a.has && a.pos <= b.pos) ? a.pos : b.pos;
        if (!opened)
            open = (TallyKey){ fid, 1u + pos };
        opened = true;
        if (a.has && a.pos == pos) {
            GroundNoted(c, 0, a.s, a.count);
            SideNext(&a);
        }
        if (b.has && b.pos == pos) {
            GroundNoted(c, 1, b.s, b.count);
            SideNext(&b);
        }
    }
    if (opened)
        SpanNoted(c, false, open, end);
}

/* Every Strip of ground of `rec`, onto side `side`. */
static void RecordGround(DogEarChange* c, uint32_t side, const FileRecord* rec)
{
    StripWalk w;
    Strip     s;
    StripWalkBegin(rec, &w);
    while (!c->refused && StripWalkNextGround(&w, &s))
        GroundNoted(c, side, &s, 1);
}

void DogEarFileChanged(TagFSVolume* vol, uint64_t fid, const FileRecord* was, const FileRecord* now, uint64_t slice)
{
    DogEarChange c;
    TallyKey     end = fid == UINT64_MAX ? DOG_EAR_END : (TallyKey){ fid + 1u, 0 };
    memset(&c, 0, sizeof(c));
    c.slice = slice;
    if (__atomic_load_n(&vol->dog_ear.held.lost, __ATOMIC_ACQUIRE))
        return;
    c.went = was && !now;
    if (!was || !now) {
        SpanNoted(&c, false, (TallyKey){ fid, 0 }, end);
        if (was || now)
            RecordGround(&c, was ? 0u : 1u, was ? was : now);
    } else {
        SpanNoted(&c, false, (TallyKey){ fid, 0 }, (TallyKey){ fid, 1 });
        ParcelsChanged(&c, fid, was, now, end);
    }
    if (!c.refused)
        GroundDiffered(vol, &c);
    if (c.refused) {
        Lose(vol);
    } else {
        SpansPut(vol, TreeAt(TALLY_FILES), c.files, c.nfiles);
        SpansPut(vol, TreeAt(TALLY_DIGESTS), c.digests, c.ndigests);
    }
    if (c.went)
        __atomic_add_fetch(&vol->dog_ear.gone, 1u, __ATOMIC_RELEASE);
    kfree(c.files);
    kfree(c.digests);
    kfree(c.ground[0]);
    kfree(c.ground[1]);
}

// ----------------------------------------------------------------------------
// What a Tally takes
// ----------------------------------------------------------------------------

/* Every Tally that took DogEar let it go at its DONE (binder_done.c,
 * BinderTallyDone) or gave it back refused (binder.c, TallyEnded — every
 * end that is not DONE, BinderStopped's too): the set taken is empty here.
 * One that is not is a Tally that ended past both, said; its keys go back
 * under what is held, so no Tally lays less than changed. */
void DogEarTake(TagFSVolume* vol)
{
    if (vol->dog_ear.taken.lost || vol->dog_ear.taken.tree[0].n || vol->dog_ear.taken.tree[1].n || vol->dog_ear.taken.tree[2].n || vol->dog_ear.taken.tree[3].n) {
        kprintf("[TagFS] DEFECT: DogEar still holds what a Tally before took — a Tally ended neither at its DONE "
                "nor refused\n");
        DogEarTakenBack(vol);
    }
    TagFSLockTake(&vol->dog_ear.lock);
    vol->dog_ear.taken = vol->dog_ear.held;
    memset(&vol->dog_ear.held, 0, sizeof(vol->dog_ear.held));
    TagFSLockDrop(&vol->dog_ear.lock);
    vol->dog_ear.delta_taken = vol->dog_ear.delta_held;
    memset(&vol->dog_ear.delta_held, 0, sizeof(vol->dog_ear.delta_held));
}

/* What moved since the slice laid over what the Tally took: the ground the
 * files let go since, then the ground they took — the two as one change from
 * the slice before. The result held, false when the heap refused it. */
static bool DeltaBack(TagFSVolume* vol)
{
    bool ok = true;
    for (uint64_t i = 0; ok && i < vol->dog_ear.delta_held.gone.n; i++)
        ok = DeltaMoved(&vol->dog_ear.delta_taken, vol->dog_ear.delta_held.gone.run[i].start,
                        vol->dog_ear.delta_held.gone.run[i].start + vol->dog_ear.delta_held.gone.run[i].count, true);
    for (uint64_t i = 0; ok && i < vol->dog_ear.delta_held.born.n; i++)
        ok = DeltaMoved(&vol->dog_ear.delta_taken, vol->dog_ear.delta_held.born.run[i].start,
                        vol->dog_ear.delta_held.born.run[i].start + vol->dog_ear.delta_held.born.run[i].count, false);
    GroundLetGo(&vol->dog_ear.delta_held);
    vol->dog_ear.delta_held = vol->dog_ear.delta_taken;
    memset(&vol->dog_ear.delta_taken, 0, sizeof(vol->dog_ear.delta_taken));
    return ok;
}

void DogEarTakenBack(TagFSVolume* vol)
{
    if (vol->dog_ear.taken.lost) {
        SetLetGo(&vol->dog_ear.taken);
        GroundLetGo(&vol->dog_ear.delta_taken);
        (void)HeldLose(vol);
        BinderDemanded(vol);       /* the refused Tally had taken the demand too */
        return;
    }
    for (uint32_t i = 0; i < DOG_EAR_TREES; i++)
        SpansPut(vol, i, vol->dog_ear.taken.tree[i].span, vol->dog_ear.taken.tree[i].n);
    SetLetGo(&vol->dog_ear.taken);
    if (!DeltaBack(vol))
        Lose(vol);
}

void DogEarTakenLetGo(TagFSVolume* vol)
{
    SetLetGo(&vol->dog_ear.taken);
    GroundLetGo(&vol->dog_ear.delta_taken);
}

const BinderGround* DogEarTakenGround(TagFSVolume* vol, bool gone, uint64_t* n)
{
    const DogEarGround* g = gone ? &vol->dog_ear.delta_taken.gone : &vol->dog_ear.delta_taken.born;
    *n = g->n;
    return g->n ? g->run : NULL;
}

const BinderGround* DogEarHeldGround(TagFSVolume* vol, bool gone, uint64_t* n)
{
    const DogEarGround* g = gone ? &vol->dog_ear.delta_held.gone : &vol->dog_ear.delta_held.born;
    *n = g->n;
    return g->n ? g->run : NULL;
}

uint64_t DogEarGoneCount(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->dog_ear.gone, __ATOMIC_ACQUIRE);
}

bool DogEarTakenHolds(TagFSVolume* vol, uint64_t tree, TallyKey key)
{
    uint32_t at = TreeAt(tree);
    if (vol->dog_ear.taken.lost)
        return true;
    if (at == UINT32_MAX)
        return false;
    const DogEarTree* t = &vol->dog_ear.taken.tree[at];
    uint32_t          lo = 0, hi = t->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (KeyLess(key, t->span[mid].hi))
            hi = mid;
        else
            lo = mid + 1u;
    }
    return lo < t->n && !KeyLess(key, t->span[lo].lo);
}

const DogEarSpan* DogEarTakenSpans(TagFSVolume* vol, uint64_t tree, uint32_t* n)
{
    uint32_t at = TreeAt(tree);
    *n = at == UINT32_MAX ? 0 : vol->dog_ear.taken.tree[at].n;
    return *n ? vol->dog_ear.taken.tree[at].span : NULL;
}

uint32_t DogEarHeldSpans(TagFSVolume* vol, uint64_t tree, uint32_t from, DogEarSpan* out, uint32_t room)
{
    uint32_t t = TreeAt(tree), n = 0;
    if (t == UINT32_MAX)
        return 0;
    TagFSLockTake(&vol->dog_ear.lock);
    for (uint32_t i = from; !vol->dog_ear.held.lost && i < vol->dog_ear.held.tree[t].n && n < room; i++)
        out[n++] = vol->dog_ear.held.tree[t].span[i];
    TagFSLockDrop(&vol->dog_ear.lock);
    return n;
}

bool DogEarHeldLost(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->dog_ear.held.lost, __ATOMIC_ACQUIRE);
}

bool DogEarTakenLost(TagFSVolume* vol)
{
    return vol->dog_ear.taken.lost;
}

void DogEarLetGo(TagFSVolume* vol)
{
    DogEarSet gone;
    TagFSLockTake(&vol->dog_ear.lock);
    gone = vol->dog_ear.held;
    memset(&vol->dog_ear.held, 0, sizeof(vol->dog_ear.held));
    TagFSLockDrop(&vol->dog_ear.lock);
    SetLetGo(&gone);
    SetLetGo(&vol->dog_ear.taken);
    GroundLetGo(&vol->dog_ear.delta_held);
    GroundLetGo(&vol->dog_ear.delta_taken);
    __atomic_store_n(&vol->dog_ear.gone, 0u, __ATOMIC_RELEASE);
}

#ifdef CONFIG_VOLUME_TESTS
uint32_t DogEarStand(TagFSVolume* vol, uint64_t tree, bool taken, DogEarSpan* out, uint32_t room)
{
    uint32_t at = TreeAt(tree), n = 0;
    if (at == UINT32_MAX)
        return 0;
    if (!taken)
        TagFSLockTake(&vol->dog_ear.lock);
    const DogEarTree* t = taken ? &vol->dog_ear.taken.tree[at] : &vol->dog_ear.held.tree[at];
    n = t->n;
    if (n && room)
        memcpy(out, t->span, sizeof(DogEarSpan) * (n < room ? n : room));
    if (!taken)
        TagFSLockDrop(&vol->dog_ear.lock);
    return n;
}

bool DogEarStandLost(TagFSVolume* vol, bool taken)
{
    return taken ? vol->dog_ear.taken.lost : __atomic_load_n(&vol->dog_ear.held.lost, __ATOMIC_ACQUIRE);
}
#endif

/* The volume's dog_ear part as it begins: what was not nothing. */
void DogEarVolumeBegin(TagFSVolume* vol)
{
    vol->dog_ear.lock = (TagFSLock)TAGFS_LOCK_INIT("dogear", LOCK_RANK_LEAF);
}
