#include "dog_ear.h"
#include "../lock_rank/lock_rank.h"
#include "binder.h"

/*
 * DogEar (dog_ear.h): the spans of each tree in two sets — the ones held since
 * the last slice, and the ones the Tally under way took — each tree's an array
 * of disjoint spans in order of their keys, no two meeting.
 */

#define DOG_EAR_TREES  4u

typedef struct {
    DogEarSpan* span;
    uint32_t    n;
    uint32_t    cap;
} DogEarTree;

typedef struct {
    DogEarTree tree[DOG_EAR_TREES];
    bool       lost;
} DogEarSet;

static TagFSLock s_lock = TAGFS_LOCK_INIT("dogear", LOCK_RANK_LEAF);
static DogEarSet s_held;       /* under s_lock */
static DogEarSet s_taken;      /* the Settle's holder's alone */

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
static bool HeldLose(void)
{
    DogEarSet gone;
    TagFSLockTake(&s_lock);
    bool was    = s_held.lost;
    gone        = s_held;
    memset(&s_held, 0, sizeof(s_held));
    s_held.lost = true;
    TagFSLockDrop(&s_lock);
    gone.lost = false;
    SetLetGo(&gone);
    return was;
}

/* The heap refused DogEar room: said once a loss, and a Tally demanded. */
static void Lose(void)
{
    if (HeldLose())
        return;
    kprintf("[TagFS] no memory to keep the keys the book changed since its last Tally (DogEar): every key is held "
            "changed, and a Tally is demanded — the next is written whole\n");
    BinderDemanded();
}

/* `count` spans into tree `at` of the held set: room for them asked of the
 * heap with no lock, and asked again should a put on another core have taken
 * it meanwhile. */
static void SpansPut(uint32_t at, const DogEarSpan* in, uint32_t count)
{
    for (;;) {
        DogEarTree* t    = &s_held.tree[at];
        uint32_t    n    = __atomic_load_n(&t->n, __ATOMIC_ACQUIRE);
        uint32_t    room = __atomic_load_n(&t->cap, __ATOMIC_ACQUIRE);
        DogEarSpan* more = NULL;
        if (__atomic_load_n(&s_held.lost, __ATOMIC_ACQUIRE) || count == 0)
            return;
        if ((uint64_t)n + count > room) {
            uint64_t want = ((uint64_t)n + count) * 2u + 16u;
            more = want <= UINT32_MAX ? kmalloc(sizeof(DogEarSpan) * want) : NULL;
            if (!more) {
                Lose();
                return;
            }
            room = (uint32_t)want;
        }
        TagFSLockTake(&s_lock);
        if (!s_held.lost && (uint64_t)t->n + count > t->cap) {
            if (!more || (uint64_t)t->n + count > room) {
                TagFSLockDrop(&s_lock);
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
        for (uint32_t i = 0; !s_held.lost && i < count; i++)
            SpanJoin(t, in[i]);
        __atomic_store_n(&t->n, t->n, __ATOMIC_RELEASE);
        TagFSLockDrop(&s_lock);
        kfree(more);
        return;
    }
}

void DogEarPut(uint64_t tree, TallyKey lo, TallyKey hi)
{
    uint32_t   at = TreeAt(tree);
    DogEarSpan s  = { lo, hi };
    if (at != UINT32_MAX && KeyLess(lo, hi))
        SpansPut(at, &s, 1);
}

void DogEarMended(uint64_t tree, const TallyPlace* at)
{
    DogEarPut(tree, at->has_lo ? at->lo : (TallyKey){ 0, 0 }, at->has_hi ? at->hi : DOG_EAR_END);
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

/* The DATA Strips of `s` (`count`) onto side `side`'s ground. */
static void GroundNoted(DogEarChange* c, uint32_t side, const Strip* s, uint64_t count)
{
    for (uint64_t i = 0; i < count && !c->refused; i++) {
        if (s[i].kind != STRIP_DATA)
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

/* The windows of DIGEST_ITEM_MOST blocks [start, end) lies in. */
static void WindowsNoted(DogEarChange* c, uint64_t start, uint64_t end)
{
    uint64_t lo   = start / DIGEST_ITEM_MOST * DIGEST_ITEM_MOST;
    uint64_t last = (end - 1u) / DIGEST_ITEM_MOST * DIGEST_ITEM_MOST;
    SpanNoted(c, true, (TallyKey){ lo, 0 },
              last > UINT64_MAX - DIGEST_ITEM_MOST ? DOG_EAR_END : (TallyKey){ last + DIGEST_ITEM_MOST, 0 });
}

/* The ground one side names and the other does not, as windows: both sides
 * laid as ground first — in order, the runs that meet joined. */
static void GroundDiffered(DogEarChange* c)
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
            WindowsNoted(c, as, x);
        } else if (bs < as) {
            x = be < as ? be : as;
            WindowsNoted(c, bs, x);
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

/* Every DATA Strip of `rec`, onto side `side`. */
static void RecordGround(DogEarChange* c, uint32_t side, const FileRecord* rec)
{
    StripWalk w;
    Strip     s;
    StripWalkBegin(rec, &w);
    while (!c->refused && StripWalkNextGround(&w, &s))
        GroundNoted(c, side, &s, 1);
}

void DogEarFileChanged(uint64_t fid, const FileRecord* was, const FileRecord* now)
{
    DogEarChange c;
    TallyKey     end = fid == UINT64_MAX ? DOG_EAR_END : (TallyKey){ fid + 1u, 0 };
    memset(&c, 0, sizeof(c));
    if (__atomic_load_n(&s_held.lost, __ATOMIC_ACQUIRE))
        return;
    if (!was || !now) {
        SpanNoted(&c, false, (TallyKey){ fid, 0 }, end);
        if (was || now)
            RecordGround(&c, was ? 0u : 1u, was ? was : now);
    } else {
        SpanNoted(&c, false, (TallyKey){ fid, 0 }, (TallyKey){ fid, 1 });
        ParcelsChanged(&c, fid, was, now, end);
    }
    if (!c.refused)
        GroundDiffered(&c);
    if (c.refused) {
        Lose();
    } else {
        SpansPut(TreeAt(TALLY_FILES), c.files, c.nfiles);
        SpansPut(TreeAt(TALLY_DIGESTS), c.digests, c.ndigests);
    }
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
void DogEarTake(void)
{
    if (s_taken.lost || s_taken.tree[0].n || s_taken.tree[1].n || s_taken.tree[2].n || s_taken.tree[3].n) {
        kprintf("[TagFS] DEFECT: DogEar still holds what a Tally before took — a Tally ended neither at its DONE "
                "nor refused\n");
        DogEarTakenBack();
    }
    TagFSLockTake(&s_lock);
    s_taken = s_held;
    memset(&s_held, 0, sizeof(s_held));
    TagFSLockDrop(&s_lock);
}

void DogEarTakenBack(void)
{
    if (s_taken.lost) {
        SetLetGo(&s_taken);
        (void)HeldLose();
        BinderDemanded();       /* the refused Tally had taken the demand too */
        return;
    }
    for (uint32_t i = 0; i < DOG_EAR_TREES; i++)
        SpansPut(i, s_taken.tree[i].span, s_taken.tree[i].n);
    SetLetGo(&s_taken);
}

void DogEarTakenLetGo(void)
{
    SetLetGo(&s_taken);
}

bool DogEarTakenHolds(uint64_t tree, TallyKey key)
{
    uint32_t at = TreeAt(tree);
    if (s_taken.lost)
        return true;
    if (at == UINT32_MAX)
        return false;
    const DogEarTree* t = &s_taken.tree[at];
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

bool DogEarTakenLost(void)
{
    return s_taken.lost;
}

void DogEarLetGo(void)
{
    DogEarSet gone;
    TagFSLockTake(&s_lock);
    gone = s_held;
    memset(&s_held, 0, sizeof(s_held));
    TagFSLockDrop(&s_lock);
    SetLetGo(&gone);
    SetLetGo(&s_taken);
}

#ifdef CONFIG_VOLUME_TESTS
uint32_t DogEarStand(uint64_t tree, bool taken, DogEarSpan* out, uint32_t room)
{
    uint32_t at = TreeAt(tree), n = 0;
    if (at == UINT32_MAX)
        return 0;
    if (!taken)
        TagFSLockTake(&s_lock);
    const DogEarTree* t = taken ? &s_taken.tree[at] : &s_held.tree[at];
    n = t->n;
    if (n && room)
        memcpy(out, t->span, sizeof(DogEarSpan) * (n < room ? n : room));
    if (!taken)
        TagFSLockDrop(&s_lock);
    return n;
}

bool DogEarStandLost(bool taken)
{
    return taken ? s_taken.lost : __atomic_load_n(&s_held.lost, __ATOMIC_ACQUIRE);
}
#endif
