#include "../tagfs.h"
#include "klib.h"

/*
 * ‼ WHO WEARS A TAG, KEPT FOR THE FILES THAT WEAR IT AND NO OTHER.
 *
 * Each tag used to be a bitmap as wide as the highest file id the volume had
 * ever issued, beside a second array per file id of the tags it wore. A tag
 * worn by one file cost an eighth of a byte for every file ever made, and the
 * index was sized by the volume's history, not by what it holds.
 *
 * Now a tag's members are split into windows of 65536 file ids, the way the
 * ids fall, and a window exists only while a member of the tag lies in it. A
 * window of up to 4096 members is a sorted array of their low 16 bits — two
 * bytes a member; past 4096 the array would outweigh a bitmap of the whole
 * window, so the window becomes that bitmap, 8 KiB, and goes back to an array
 * once it falls to 2048 — the gap keeps one member that comes and goes at the
 * edge from rebuilding the window every time. A removal under the volume's
 * state lock allocates nothing, so the array is made after it, with that lock
 * let go and taken again to put it in (TagBitmapShrink). The tags a file wears are read
 * from its record in the map of live files, not kept a second time here.
 *
 * The tags that have members are found by their id through a hash of chains,
 * not a table indexed by id: a table as long as the highest id any file ever
 * wore cost eight bytes for every id the volume had issued before it, worn or
 * not. The hash has between a quarter of one and one tag a bucket — it doubles
 * when an add would pass one, and halves back after the leaves that take it
 * under a quarter (TagBitmapShrink) — so it is sized by the tags worn now.
 *
 * Everything is changed under the index's lock, a leaf, and nothing is
 * allocated under it: an add that finds it needs memory — a longer table of
 * tags, a tag's first member, a longer list of windows, a window's storage —
 * lets the lock go, takes that one piece, and comes back to install it if it
 * is still what is wanted.
 */

#define TAG_WINDOW_SHIFT   16u
#define TAG_WINDOW_IDS     (1u << TAG_WINDOW_SHIFT)
#define TAG_WINDOW_WORDS   (TAG_WINDOW_IDS / 64u)
#define TAG_BITMAP_BYTES   (TAG_WINDOW_IDS / 8u)
#define TAG_ARRAY_MOST     (TAG_BITMAP_BYTES / sizeof(uint16_t))   /* 4096: as heavy as the bitmap */
#define TAG_ARRAY_BACK     (TAG_ARRAY_MOST / 2u)
#define TAG_ARRAY_FIRST    4u
#define TAG_BUCKETS_FIRST  16u     /* the hash of tags while few have members */

/* The work a query does under one hold of the lock, before letting it go for
 * anybody waiting and taking it again where it stopped: a step is a tag asked
 * for its size, its next member or whether it holds an id, a window more that
 * answer went through, or 64 words of a bitmap it scanned. A wildcard's tags
 * are asked one at a time, so a key with thousands of values is weighed by
 * its tags, not by the holds. */
#define TAG_QUERY_STEPS    512u

typedef struct {
    uint64_t hi;             /* the window's file ids share this: id >> 16 */
    uint16_t bitmap;         /* 1: `data` is TAG_WINDOW_WORDS words; 0: a sorted array */
    uint32_t count;          /* members in the window */
    uint32_t room;           /* an array's slots */
    void*    data;
} TagWindow;

typedef struct TagMembers {
    struct TagMembers* next; /* the next tag in its bucket */
    uint32_t   tag;
    uint32_t   members;
    uint32_t   windows;      /* in use, ascending by hi */
    uint32_t   room;
    TagWindow* window;
} TagMembers;

struct TagBitmapIndex {
    TagMembers** tags;       /* `buckets` chains, a tag's by its id; none for a tag no file wears */
    uint32_t     buckets;    /* a power of two */
    uint32_t     count;      /* tags with a TagMembers */
    uint64_t     bytes;      /* what the index holds of the kernel heap */
    bool         said_short;
    TagFSLock    lock;
#ifdef CONFIG_VOLUME_TESTS
    uint32_t     query_most;  /* the most steps one hold of a query took, since asked */
#endif
};

// ----------------------------------------------------------------------------
// A window
// ----------------------------------------------------------------------------

static uint32_t ArrayLowerBound(const uint16_t* a, uint32_t n, uint32_t lo)
{
    uint32_t l = 0, h = n;
    while (l < h) {
        uint32_t mid = (l + h) / 2;
        if (a[mid] < lo)
            l = mid + 1;
        else
            h = mid;
    }
    return l;
}

static bool WindowHas(const TagWindow* w, uint32_t lo)
{
    if (w->bitmap)
        return (((const uint64_t*)w->data)[lo / 64u] >> (lo % 64u)) & 1u;
    uint32_t at = ArrayLowerBound(w->data, w->count, lo);
    return at < w->count && ((const uint16_t*)w->data)[at] == lo;
}

/* The least member of the window at or past `lo`; the words of a bitmap it
 * scanned added to *words. */
static bool WindowNext(const TagWindow* w, uint32_t lo, uint32_t* out, uint32_t* words)
{
    if (!w->bitmap) {
        uint32_t at = ArrayLowerBound(w->data, w->count, lo);
        if (at == w->count)
            return false;
        *out = ((const uint16_t*)w->data)[at];
        return true;
    }
    const uint64_t* bits  = w->data;
    uint32_t        first = lo / 64u;
    uint32_t        word  = first;
    uint64_t        v     = bits[word] & (~0ull << (lo % 64u));
    for (;;) {
        if (v) {
            *words += word - first + 1u;
            *out = word * 64u + (uint32_t)__builtin_ctzll(v);
            return true;
        }
        if (++word == TAG_WINDOW_WORDS) {
            *words += word - first;
            return false;
        }
        v = bits[word];
    }
}

/* Where the window of `hi` is, or would go. */
static uint32_t WindowAt(const TagMembers* m, uint64_t hi)
{
    uint32_t l = 0, h = m->windows;
    while (l < h) {
        uint32_t mid = (l + h) / 2;
        if (m->window[mid].hi < hi)
            l = mid + 1;
        else
            h = mid;
    }
    return l;
}

static TagMembers* MembersOf(const TagBitmapIndex* idx, uint32_t tag)
{
    TagMembers* m = idx->tags[tag & (idx->buckets - 1u)];
    while (m && m->tag != tag)
        m = m->next;
    return m;
}

/* Every tag's members chained again into `table`, of `buckets` buckets, which
 * becomes the index's; the bytes are the caller's to count. */
static void IndexRehome(TagBitmapIndex* idx, TagMembers** table, uint32_t buckets)
{
    memset(table, 0, sizeof(TagMembers*) * buckets);
    for (uint32_t b = 0; b < idx->buckets; b++) {
        for (TagMembers* m = idx->tags[b], *next; m; m = next) {
            next    = m->next;
            m->next = table[m->tag & (buckets - 1u)];
            table[m->tag & (buckets - 1u)] = m;
        }
    }
    idx->tags    = table;
    idx->buckets = buckets;
}

static bool MembersHas(const TagMembers* m, uint64_t fid)
{
    if (!m)
        return false;
    uint32_t w = WindowAt(m, fid >> TAG_WINDOW_SHIFT);
    return w < m->windows && m->window[w].hi == (fid >> TAG_WINDOW_SHIFT) &&
           WindowHas(&m->window[w], (uint32_t)(fid & (TAG_WINDOW_IDS - 1u)));
}

/* The least member at or past x; what the asking cost, in a query's steps,
 * added to *cost. Every window past the first holds a member at all, so an
 * answer goes through two windows at most. */
static bool MembersNext(const TagMembers* m, uint64_t x, uint64_t* out, uint32_t* cost)
{
    (*cost)++;
    if (!m)
        return false;
    uint64_t hi    = x >> TAG_WINDOW_SHIFT;
    uint32_t words = 0;
    bool     found = false;
    for (uint32_t w = WindowAt(m, hi), first = w; w < m->windows && !found; w++) {
        const TagWindow* win   = &m->window[w];
        uint32_t         start = win->hi == hi ? (uint32_t)(x & (TAG_WINDOW_IDS - 1u)) : 0;
        uint32_t         lo;
        *cost += w > first;
        if (WindowNext(win, start, &lo, &words)) {
            *out  = ((uint64_t)win->hi << TAG_WINDOW_SHIFT) | lo;
            found = true;
        }
    }
    *cost += words / 64u;
    return found;
}

// ----------------------------------------------------------------------------
// What an add needs, and installing it
// ----------------------------------------------------------------------------

typedef enum {
    WANT_NOTHING = 0,
    WANT_TAGS,               /* a hash of `size` buckets of tags */
    WANT_MEMBERS,            /* the tag's TagMembers */
    WANT_WINDOWS,            /* a list of `size` windows */
    WANT_DATA                /* a window's storage: `size` array slots, or 0 for a bitmap */
} TagWant;

typedef struct {
    TagWant  want;
    uint32_t size;
    void*    piece;          /* taken for it, not yet installed */
    uint32_t tag;            /* the tag whose add wants it, for the line a shortage says */
} TagPiece;

static size_t PieceBytes(const TagPiece* p)
{
    switch (p->want) {
    case WANT_TAGS:    return sizeof(TagMembers*) * p->size;
    case WANT_MEMBERS: return sizeof(TagMembers);
    case WANT_WINDOWS: return sizeof(TagWindow) * p->size;
    case WANT_DATA:    return p->size ? sizeof(uint16_t) * p->size : TAG_BITMAP_BYTES;
    default:           return 0;
    }
}

static size_t WindowBytes(const TagWindow* w)
{
    return w->bitmap ? TAG_BITMAP_BYTES : sizeof(uint16_t) * w->room;
}

/* What adding `fid` to `tag` still lacks. */
static TagPiece IndexWants(const TagBitmapIndex* idx, uint32_t tag, uint64_t fid)
{
    TagPiece p = { WANT_NOTHING, 0, NULL, tag };
    const TagMembers* m = MembersOf(idx, tag);
    if (!m) {
        p.want = idx->count >= idx->buckets ? WANT_TAGS : WANT_MEMBERS;
        p.size = idx->count >= idx->buckets ? idx->buckets * 2u : 0;
        return p;
    }
    uint64_t hi = fid >> TAG_WINDOW_SHIFT;
    uint32_t w  = WindowAt(m, hi);
    if (w == m->windows || m->window[w].hi != hi) {
        p.want = m->windows == m->room ? WANT_WINDOWS : WANT_DATA;
        p.size = m->windows == m->room ? (m->room ? m->room * 2u : 4u) : TAG_ARRAY_FIRST;
        return p;
    }
    const TagWindow* win = &m->window[w];
    if (!win->bitmap && win->count == win->room &&
        !WindowHas(win, (uint32_t)(fid & (TAG_WINDOW_IDS - 1u)))) {
        p.want = WANT_DATA;
        p.size = win->room < TAG_ARRAY_MOST
                     ? (win->room * 2u < TAG_ARRAY_MOST ? win->room * 2u : TAG_ARRAY_MOST)
                     : 0;
    }
    return p;
}

/* The piece put where it is wanted, when it is still what adding `fid` to
 * `tag` wants: answers what it replaced, or the piece itself when it no longer
 * fits the index, for the caller to free once the lock is let go. */
static void* IndexInstall(TagBitmapIndex* idx, uint32_t tag, uint64_t fid, TagPiece* p)
{
    TagPiece now = IndexWants(idx, tag, fid);
    if (now.want != p->want || now.size != p->size)
        return p->piece;

    void* old = NULL;
    idx->bytes += PieceBytes(p);
    switch (p->want) {
    case WANT_TAGS:
        old = idx->tags;
        idx->bytes -= sizeof(TagMembers*) * idx->buckets;
        IndexRehome(idx, p->piece, p->size);
        break;
    case WANT_MEMBERS: {
        TagMembers* m = p->piece;
        memset(m, 0, sizeof(*m));
        m->tag  = tag;
        m->next = idx->tags[tag & (idx->buckets - 1u)];
        idx->tags[tag & (idx->buckets - 1u)] = m;
        idx->count++;
        break;
    }
    case WANT_WINDOWS: {
        TagMembers* m = MembersOf(idx, tag);
        if (m->windows)
            memcpy(p->piece, m->window, sizeof(TagWindow) * m->windows);
        old = m->window;
        idx->bytes -= sizeof(TagWindow) * m->room;
        m->window = p->piece;
        m->room   = p->size;
        break;
    }
    case WANT_DATA: {
        TagMembers* m  = MembersOf(idx, tag);
        uint64_t    hi = fid >> TAG_WINDOW_SHIFT;
        uint32_t    w  = WindowAt(m, hi);
        if (w == m->windows || m->window[w].hi != hi) {
            memmove(&m->window[w + 1], &m->window[w], sizeof(TagWindow) * (m->windows - w));
            m->window[w] = (TagWindow){ .hi = hi, .bitmap = 0, .count = 0,
                                        .room = p->size, .data = p->piece };
            m->windows++;
            break;
        }
        TagWindow* win = &m->window[w];
        old = win->data;
        idx->bytes -= WindowBytes(win);
        if (p->size) {
            memcpy(p->piece, win->data, sizeof(uint16_t) * win->count);
            win->room = p->size;
        } else {
            uint64_t*       bits = p->piece;
            const uint16_t* low  = win->data;
            memset(bits, 0, TAG_BITMAP_BYTES);
            for (uint32_t i = 0; i < win->count; i++)
                bits[low[i] / 64u] |= 1ull << (low[i] % 64u);
            win->bitmap = 1;
            win->room   = 0;
        }
        win->data = p->piece;
        break;
    }
    default:
        break;
    }
    p->piece = NULL;
    return old;
}

/* `fid` into `tag`'s members, which IndexWants found need nothing more. */
static void IndexAdd(TagBitmapIndex* idx, uint32_t tag, uint64_t fid)
{
    TagMembers* m   = MembersOf(idx, tag);
    TagWindow*  win = &m->window[WindowAt(m, fid >> TAG_WINDOW_SHIFT)];
    uint32_t    lo  = (uint32_t)(fid & (TAG_WINDOW_IDS - 1u));
    if (WindowHas(win, lo))
        return;
    if (win->bitmap) {
        ((uint64_t*)win->data)[lo / 64u] |= 1ull << (lo % 64u);
    } else {
        uint16_t* low = win->data;
        uint32_t  at  = ArrayLowerBound(low, win->count, lo);
        memmove(&low[at + 1], &low[at], sizeof(uint16_t) * (win->count - at));
        low[at] = (uint16_t)lo;
    }
    win->count++;
    m->members++;
}

/* ‼ A SHORTAGE IS SAID WITH THE INDEX'S NUMBERS, once until the heap gives
 * again — which file and tag were refused, what was asked, and what the index
 * already holds — and the caller hears the refusal. */
static void IndexSayShort(TagBitmapIndex* idx, uint32_t tag, uint64_t fid, const TagPiece* p)
{
    TagFSLockTake(&idx->lock);
    bool     said  = idx->said_short;
    uint64_t bytes = idx->bytes;
    idx->said_short = true;
    TagFSLockDrop(&idx->lock);
    if (!said)
        kprintf("[TagFS] the tag index could not put file %llu under tag %u: %zu bytes were "
                "refused, and it holds %llu bytes of kernel heap\n",
                fid, tag, PieceBytes(p), (unsigned long long)bytes);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

TagBitmapIndex* tag_bitmap_create(void)
{
    TagBitmapIndex* idx = kmalloc(sizeof(TagBitmapIndex));
    if (!idx)
        return NULL;
    memset(idx, 0, sizeof(*idx));
    idx->tags = kmalloc(sizeof(TagMembers*) * TAG_BUCKETS_FIRST);
    if (!idx->tags) {
        kfree(idx);
        return NULL;
    }
    memset(idx->tags, 0, sizeof(TagMembers*) * TAG_BUCKETS_FIRST);
    idx->buckets = TAG_BUCKETS_FIRST;
    idx->bytes   = sizeof(TagMembers*) * TAG_BUCKETS_FIRST;
    TagFSLockInit(&idx->lock, "index", LOCK_RANK_LEAF);
    return idx;
}

static void MembersFree(TagMembers* m)
{
    for (uint32_t w = 0; w < m->windows; w++)
        kfree(m->window[w].data);
    kfree(m->window);
    kfree(m);
}

void tag_bitmap_destroy(TagBitmapIndex* idx)
{
    if (!idx)
        return;
    for (uint32_t b = 0; b < idx->buckets; b++) {
        for (TagMembers* m = idx->tags[b], *next; m; m = next) {
            next = m->next;
            MembersFree(m);
        }
    }
    kfree(idx->tags);
    kfree(idx);
}

int tag_bitmap_set(TagBitmapIndex* idx, uint32_t tag_id, uint64_t file_id)
{
    if (!idx || file_id == 0)
        return -1;

    TagPiece piece = { WANT_NOTHING, 0, NULL, 0 };
    for (;;) {
        void* spare = NULL;
        TagFSLockTake(&idx->lock);
        if (piece.piece)
            spare = IndexInstall(idx, tag_id, file_id, &piece);
        TagPiece want = IndexWants(idx, tag_id, file_id);
        if (want.want == WANT_NOTHING) {
            IndexAdd(idx, tag_id, file_id);
            idx->said_short = false;
            TagFSLockDrop(&idx->lock);
            kfree(spare);
            return 0;
        }
        TagFSLockDrop(&idx->lock);
        kfree(spare);

        piece       = want;
        piece.piece = kmalloc(PieceBytes(&want));
        if (!piece.piece) {
            IndexSayShort(idx, tag_id, file_id, &want);
            return -1;
        }
    }
}

// ----------------------------------------------------------------------------
// An add staked before the volume's state lock, made under it
//
// A file's record changes under the volume's state lock (Ink), and the file
// goes under its tags in the same hold — but nothing is allocated under that
// lock. So every piece the adds lack is taken first, with no TagFS lock held
// (TagBitmapStakeFor), and under the lock the pieces are put in (Covers) and
// the file added (AddCovered). Every change of the index's shape is made under
// the state lock — by an Ink, or by the mount with the volume's door shut — so
// what Covers found enough stays enough until AddCovered. When it is not
// enough — the index changed since the stake — nothing is added, the pieces
// already put in stay (a table that grew, a list with room, both harmless),
// and the caller stakes again.
// ----------------------------------------------------------------------------

#define TAG_STAKE_PIECES 4u     /* the most one add can lack: tags, members, windows, data */

struct TagBitmapStake {
    uint32_t count;             /* pieces taken */
    uint32_t spent_old;         /* storage the installs replaced, freed with the stake */
    void*    old[1];            /* ... room for count of them, then the pieces */
};

static TagPiece* StakePieces(TagBitmapStake* s)
{
    return (TagPiece*)(void*)&s->old[s->count ? s->count : 1];
}

/* What adding `fid` to `tag` lacks, piece by piece, as if each were put in:
 * written into out[], the count answered. `count` and `buckets` are the tags
 * with members and the hash's size as the pieces staked before this one would
 * leave them. */
static uint32_t IndexWantsAll(const TagBitmapIndex* idx, uint32_t tag, uint64_t fid,
                              uint32_t* count, uint32_t* buckets, TagPiece out[TAG_STAKE_PIECES])
{
    uint32_t n = 0;
    const TagMembers* m = MembersOf(idx, tag);
    if (!m) {
        if (*count >= *buckets) {
            out[n++] = (TagPiece){ WANT_TAGS, *buckets * 2u, NULL, tag };
            *buckets *= 2u;
        }
        (*count)++;
        out[n++] = (TagPiece){ WANT_MEMBERS, 0, NULL, tag };
        out[n++] = (TagPiece){ WANT_WINDOWS, 4u, NULL, tag };
        out[n++] = (TagPiece){ WANT_DATA, TAG_ARRAY_FIRST, NULL, tag };
        return n;
    }
    TagPiece last = IndexWants(idx, tag, fid);
    if (last.want == WANT_WINDOWS) {
        out[n++] = last;
        last = (TagPiece){ WANT_DATA, TAG_ARRAY_FIRST, NULL, tag };
    }
    if (last.want != WANT_NOTHING)
        out[n++] = last;
    return n;
}

error_t TagBitmapStakeFor(TagBitmapIndex* idx, const uint32_t* tags, uint32_t count,
                          uint64_t fid, TagBitmapStake** out)
{
    *out = NULL;
    if (!idx || count == 0)
        return OK;

    TagPiece* wants = kmalloc(sizeof(TagPiece) * TAG_STAKE_PIECES * count);
    if (!wants)
        return ERR_NO_MEMORY;
    uint32_t n = 0;
    TagFSLockTake(&idx->lock);
    uint32_t members = idx->count, buckets = idx->buckets;
    for (uint32_t i = 0; i < count; i++)
        n += IndexWantsAll(idx, tags[i], fid, &members, &buckets, &wants[n]);
    TagFSLockDrop(&idx->lock);

    TagBitmapStake* s = kmalloc(sizeof(TagBitmapStake) + sizeof(void*) * (n ? n - 1 : 0) +
                                sizeof(TagPiece) * (n ? n : 1));
    if (!s) {
        kfree(wants);
        return ERR_NO_MEMORY;
    }
    s->count     = n;
    s->spent_old = 0;
    TagPiece* piece = StakePieces(s);
    error_t   rc    = OK;
    for (uint32_t i = 0; i < n; i++) {
        piece[i]       = wants[i];
        piece[i].piece = rc == OK ? kmalloc(PieceBytes(&wants[i])) : NULL;
        if (!piece[i].piece && rc == OK) {
            IndexSayShort(idx, wants[i].tag, fid, &wants[i]);
            rc = ERR_NO_MEMORY;
        }
    }
    kfree(wants);
    if (rc != OK) {
        TagBitmapStakeLetGo(s);
        return rc;
    }
    *out = s;
    return OK;
}

bool TagBitmapStakeCovers(TagBitmapIndex* idx, const uint32_t* tags, uint32_t count,
                          uint64_t fid, TagBitmapStake* s)
{
    bool covered = true;
    TagFSLockTake(&idx->lock);
    for (uint32_t i = 0; i < count && covered; i++) {
        for (;;) {
            TagPiece want = IndexWants(idx, tags[i], fid);
            if (want.want == WANT_NOTHING)
                break;
            TagPiece* have = NULL;
            for (uint32_t k = 0; s && k < s->count && !have; k++) {
                TagPiece* p = &StakePieces(s)[k];
                if (p->piece && p->want == want.want && p->size == want.size)
                    have = p;
            }
            if (!have) {
                covered = false;
                break;
            }
            void* old = IndexInstall(idx, tags[i], fid, have);
            if (old == have->piece) {
                covered = false;        /* the index is not what the want was read from */
                break;
            }
            if (old)
                s->old[s->spent_old++] = old;
        }
    }
    TagFSLockDrop(&idx->lock);
    return covered;
}

void TagBitmapAddCovered(TagBitmapIndex* idx, const uint32_t* tags, uint32_t count, uint64_t fid)
{
    TagFSLockTake(&idx->lock);
    for (uint32_t i = 0; i < count; i++)
        IndexAdd(idx, tags[i], fid);
    idx->said_short = false;
    TagFSLockDrop(&idx->lock);
}

void TagBitmapStakeLetGo(TagBitmapStake* s)
{
    if (!s)
        return;
    for (uint32_t i = 0; i < s->spent_old; i++)
        kfree(s->old[i]);
    for (uint32_t k = 0; k < s->count; k++)
        kfree(StakePieces(s)[k].piece);
    kfree(s);
}

/* `fid` out of `tag`'s members under the lock. What the removal emptied —
 * the window's storage, and the tag's own list when it has no member left —
 * goes to `freed[]` to be given back after the lock. */
static void IndexRemove(TagBitmapIndex* idx, uint32_t tag, uint64_t fid, void* freed[3])
{
    TagMembers* m = MembersOf(idx, tag);
    uint32_t    w = m ? WindowAt(m, fid >> TAG_WINDOW_SHIFT) : 0;
    if (!m || w == m->windows || m->window[w].hi != (fid >> TAG_WINDOW_SHIFT))
        return;
    TagWindow* win = &m->window[w];
    uint32_t   lo  = (uint32_t)(fid & (TAG_WINDOW_IDS - 1u));
    if (!WindowHas(win, lo))
        return;

    if (win->bitmap) {
        ((uint64_t*)win->data)[lo / 64u] &= ~(1ull << (lo % 64u));
    } else {
        uint16_t* low = win->data;
        uint32_t  at  = ArrayLowerBound(low, win->count, lo);
        memmove(&low[at], &low[at + 1], sizeof(uint16_t) * (win->count - at - 1));
    }
    win->count--;
    m->members--;

    if (win->count == 0) {
        idx->bytes -= WindowBytes(win);
        freed[0] = win->data;
        memmove(&m->window[w], &m->window[w + 1], sizeof(TagWindow) * (m->windows - w - 1));
        m->windows--;
    }
    if (m->members == 0) {
        TagMembers** at = &idx->tags[tag & (idx->buckets - 1u)];
        while (*at != m)
            at = &(*at)->next;
        *at = m->next;
        idx->count--;
        idx->bytes -= sizeof(TagWindow) * m->room + sizeof(TagMembers);
        freed[1] = m->window;
        freed[2] = m;
    }
}

/* The window of `fid` under `tag`, under the index's lock, when it is a
 * bitmap fallen to TAG_ARRAY_BACK members, where an array is the lighter
 * again; NULL otherwise. */
static TagWindow* IndexFallenBitmap(TagBitmapIndex* idx, uint32_t tag, uint64_t fid)
{
    TagMembers* m = MembersOf(idx, tag);
    uint32_t    w = m ? WindowAt(m, fid >> TAG_WINDOW_SHIFT) : 0;
    if (!m || w == m->windows || m->window[w].hi != (fid >> TAG_WINDOW_SHIFT))
        return NULL;
    TagWindow* win = &m->window[w];
    return win->bitmap && win->count <= TAG_ARRAY_BACK ? win : NULL;
}

/* That window made an array again, as long as its `count` members need, taken
 * with no lock held and put in under `state` — the index changes shape only
 * under the volume's state lock (TagBitmapStakeCovers) — and the index's. Left
 * a bitmap when there is no memory, or when it no longer fits. */
static void IndexShrink(TagBitmapIndex* idx, TagFSLock* state, uint32_t tag, uint64_t fid,
                        uint32_t count)
{
    uint32_t room = TAG_ARRAY_FIRST;
    while (room < count)
        room *= 2u;
    uint16_t* low = kmalloc(sizeof(uint16_t) * room);
    if (!low)
        return;
    void* old = low;
    TagFSLockTake(state);
    TagFSLockTake(&idx->lock);
    TagWindow* win = IndexFallenBitmap(idx, tag, fid);
    if (win && win->count <= room) {
        const uint64_t* bits = win->data;
        uint32_t        n    = 0;
        for (uint32_t k = 0; k < TAG_WINDOW_WORDS; k++)
            for (uint64_t v = bits[k]; v; v &= v - 1)
                low[n++] = (uint16_t)(k * 64u + (uint32_t)__builtin_ctzll(v));
        old         = win->data;
        win->data   = low;
        win->bitmap = 0;
        win->room   = room;
        idx->bytes  = idx->bytes - TAG_BITMAP_BYTES + sizeof(uint16_t) * room;
    }
    TagFSLockDrop(&idx->lock);
    TagFSLockDrop(state);
    kfree(old);
}

/* The size the hash of tags goes down to once the tags with members fill
 * under a quarter of it — the least that keeps it at most half full, never
 * under TAG_BUCKETS_FIRST — or 0 while it should stay as it is. */
static uint32_t IndexTagsShrinkTo(const TagBitmapIndex* idx)
{
    if (idx->buckets <= TAG_BUCKETS_FIRST || idx->count >= idx->buckets / 4u)
        return 0;
    uint32_t to = TAG_BUCKETS_FIRST;
    while (to < idx->count * 2u)
        to *= 2u;
    return to;
}

/* That smaller hash taken with no lock held and put in under `state` and the
 * index's lock — when it is still the size wanted. */
static void IndexShrinkTags(TagBitmapIndex* idx, TagFSLock* state)
{
    TagFSLockTake(&idx->lock);
    uint32_t to = IndexTagsShrinkTo(idx);
    TagFSLockDrop(&idx->lock);
    if (!to)
        return;
    TagMembers** table = kmalloc(sizeof(TagMembers*) * to);
    if (!table)
        return;
    void* old = table;
    TagFSLockTake(state);
    TagFSLockTake(&idx->lock);
    if (IndexTagsShrinkTo(idx) == to) {
        old        = idx->tags;
        idx->bytes = idx->bytes - sizeof(TagMembers*) * idx->buckets + sizeof(TagMembers*) * to;
        IndexRehome(idx, table, to);
    }
    TagFSLockDrop(&idx->lock);
    TagFSLockDrop(state);
    kfree(old);
}

void TagBitmapLeave(TagBitmapIndex* idx, uint32_t tag_id, uint64_t file_id)
{
    if (!idx)
        return;
    void* freed[3] = { NULL, NULL, NULL };
    TagFSLockTake(&idx->lock);
    IndexRemove(idx, tag_id, file_id, freed);
    TagFSLockDrop(&idx->lock);
    for (uint32_t i = 0; i < 3; i++)
        kfree(freed[i]);
}

void TagBitmapShrink(TagBitmapIndex* idx, TagFSLock* state, const uint32_t* tags,
                     uint32_t count, uint64_t file_id)
{
    if (!idx)
        return;
    for (uint32_t i = 0; i < count; i++) {
        TagFSLockTake(&idx->lock);
        TagWindow* win     = IndexFallenBitmap(idx, tags[i], file_id);
        uint32_t   members = win ? win->count : 0;
        TagFSLockDrop(&idx->lock);
        if (members)
            IndexShrink(idx, state, tags[i], file_id, members);
    }
    IndexShrinkTags(idx, state);
}

bool TagBitmapHas(TagBitmapIndex* idx, uint32_t tag_id, uint64_t file_id)
{
    if (!idx)
        return false;
    TagFSLockTake(&idx->lock);
    bool has = MembersHas(MembersOf(idx, tag_id), file_id);
    TagFSLockDrop(&idx->lock);
    return has;
}

uint64_t TagBitmapBytes(TagBitmapIndex* idx)
{
    if (!idx)
        return 0;
    TagFSLockTake(&idx->lock);
    uint64_t bytes = idx->bytes;
    TagFSLockDrop(&idx->lock);
    return bytes;
}

// ----------------------------------------------------------------------------
// A query, a page at a time
//
// Every term — a tag, or the tags of a key any one of which will do — is a
// sorted set of file ids, and the matches are the ids every term holds. They
// are found by leaping: each term is asked for its least member at or past
// the candidate, and a term whose answer lies further on moves the candidate
// there, until every term answers the candidate itself. No set is copied and
// nothing is allocated; the smallest term leads, so the leaps are as long as
// the index allows.
// ----------------------------------------------------------------------------

typedef struct {
    const uint32_t*  tag_ids;
    uint32_t         tag_count;
    const TagIdList* groups;
    uint32_t         group_count;
    uint32_t         lead;           /* the term asked first */
    /* Where the query stands, carried from one hold of the lock to the next. */
    bool             led;            /* the lead is chosen ... */
    uint32_t         sized;          /* ... while it is not, the term being sized, */
    uint64_t         size;           /* what its tags sized so far hold, */
    uint64_t         least;          /* and the fewest members of a term sized whole */
    uint64_t         x;              /* the candidate */
    uint32_t         k;              /* the term of the round asked next: 0 the lead */
    uint32_t         at;             /* of the term being sized or asked, its tag next */
    bool             any;            /* of its tags asked, one holds an id at or past x ... */
    uint64_t         y;              /* ... and this is the least such */
} TagQuery;

static uint32_t QueryTerms(const TagQuery* q)
{
    return q->tag_count + q->group_count;
}

static void QueryTerm(const TagQuery* q, uint32_t t, const uint32_t** ids, uint32_t* n)
{
    if (t < q->tag_count) {
        *ids = &q->tag_ids[t];
        *n   = 1;
    } else {
        *ids = q->groups[t - q->tag_count].ids;
        *n   = q->groups[t - q->tag_count].count;
    }
}

/* The lead: the term with the fewest members, sized a tag at a time. False
 * when the hold's steps ran out first. */
static bool QueryLead(const TagBitmapIndex* idx, TagQuery* q, uint32_t* used)
{
    for (; q->sized < QueryTerms(q); q->sized++, q->at = 0, q->size = 0) {
        const uint32_t* ids;
        uint32_t        n;
        QueryTerm(q, q->sized, &ids, &n);
        for (; q->at < n; q->at++) {
            if (*used >= TAG_QUERY_STEPS)
                return false;
            (*used)++;
            const TagMembers* m = MembersOf(idx, ids[q->at]);
            q->size += m ? m->members : 0;
        }
        if (q->sized == 0 || q->size < q->least) {
            q->least = q->size;
            q->lead  = q->sized;
        }
    }
    q->at  = 0;
    q->led = true;
    return true;
}

/* The least member of term t at or past the candidate, into q->any and q->y,
 * a tag at a time from where the last hold stopped. False when the hold's
 * steps ran out first. */
static bool TermNext(const TagBitmapIndex* idx, TagQuery* q, uint32_t t, uint32_t* used)
{
    const uint32_t* ids;
    uint32_t        n;
    QueryTerm(q, t, &ids, &n);
    for (; q->at < n; q->at++) {
        if (*used >= TAG_QUERY_STEPS)
            return false;
        uint64_t y = 0;
        if (MembersNext(MembersOf(idx, ids[q->at]), q->x, &y, used) && (!q->any || y < q->y)) {
            q->y   = y;
            q->any = true;
        }
    }
    return true;
}

typedef enum { QUERY_MATCH, QUERY_END, QUERY_PAUSE } QueryStep;

/* From the candidate, the next id every term holds: MATCH with the candidate
 * on it, END when there is none, PAUSE once the hold's steps are spent, to go
 * on from where it stood. A term whose answer lies further on moves the
 * candidate there, and the round begins again from the lead. */
static QueryStep QueryLeap(const TagBitmapIndex* idx, TagQuery* q, uint32_t* used)
{
    uint32_t terms = QueryTerms(q);
    while (q->k < terms) {
        uint32_t t = q->k == 0 ? q->lead : (q->k <= q->lead ? q->k - 1 : q->k);
        if (!TermNext(idx, q, t, used))
            return QUERY_PAUSE;
        bool     any = q->any;
        uint64_t y   = q->y;
        q->at  = 0;
        q->any = false;
        if (!any)
            return QUERY_END;
        if (y != q->x) {
            q->x = y;
            q->k = 0;
        } else {
            q->k++;
        }
    }
    q->k = 0;
    return QUERY_MATCH;
}

static bool QueryExcludes(const TagBitmapIndex* idx, const uint32_t* unless, uint32_t n,
                          uint64_t fid)
{
    for (uint32_t i = 0; i < n; i++)
        if (MembersHas(MembersOf(idx, unless[i]), fid))
            return true;
    return false;
}

#ifdef CONFIG_VOLUME_TESTS
uint32_t TagBitmapQueryMost(TagBitmapIndex* idx)
{
    TagFSLockTake(&idx->lock);
    uint32_t most   = idx->query_most;
    idx->query_most = 0;
    TagFSLockDrop(&idx->lock);
    return most;
}

/* A hold stops before a step once TAG_QUERY_STEPS are taken, so it takes at
 * most one less, then the dearest thing it may begin: a tag asked for its next
 * member — two windows and two bitmaps scanned whole — and, on a match, every
 * tag excluded. */
uint32_t TagBitmapQueryHoldBound(uint32_t unless_count)
{
    return TAG_QUERY_STEPS - 1u + 2u + 2u * TAG_WINDOW_WORDS / 64u + unless_count;
}
#endif

int TagBitmapQueryPage(TagBitmapIndex* idx,
                       const uint32_t* tag_ids, uint32_t tag_count,
                       const TagIdList* groups, uint32_t group_count,
                       const uint32_t* unless, uint32_t unless_count,
                       uint64_t from, uint64_t* out, uint32_t room,
                       bool want_total, TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (!idx || (room && !out))
        return -1;
    TagQuery q = { .tag_ids = tag_ids, .tag_count = tag_count, .groups = groups,
                   .group_count = group_count, .x = from ? from : 1 };
    if (QueryTerms(&q) == 0)
        return 0;

    bool done = false;
    while (!done) {
        uint32_t used = 0;
        TagFSLockTake(&idx->lock);
        while (!done && (q.led || QueryLead(idx, &q, &used))) {
            QueryStep step = QueryLeap(idx, &q, &used);
            if (step == QUERY_PAUSE)
                break;
            if (step == QUERY_END) {
                done = true;
                break;
            }
            uint64_t fid = q.x;
            done  = fid == UINT64_MAX;     /* no id lies past the last one */
            q.x++;
            used += unless_count;
            if (QueryExcludes(idx, unless, unless_count, fid))
                continue;
            if (page->delivered < room) {
                out[page->delivered++] = fid;
            } else if (!page->next) {
                page->next = fid;
                done       = done || !want_total;
            }
            if (want_total)
                page->total++;
        }
#ifdef CONFIG_VOLUME_TESTS
        if (used > idx->query_most)
            idx->query_most = used;
#endif
        TagFSLockDrop(&idx->lock);
    }
    return 0;
}
