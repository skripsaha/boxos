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
 * edge from rebuilding the window every time. The tags a file wears are read
 * from its record in the map of live files, not kept a second time here.
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

/* Next ids a query weighs under one hold of the lock, before letting it go
 * for anybody waiting and taking it again where it stopped. */
#define TAG_QUERY_STEPS    512u

typedef struct {
    uint16_t hi;             /* the window's file ids share this: id >> 16 */
    uint16_t bitmap;         /* 1: `data` is TAG_WINDOW_WORDS words; 0: a sorted array */
    uint32_t count;          /* members in the window */
    uint32_t room;           /* an array's slots */
    void*    data;
} TagWindow;

typedef struct {
    uint32_t   members;
    uint32_t   windows;      /* in use, ascending by hi */
    uint32_t   room;
    TagWindow* window;
} TagMembers;

struct TagBitmapIndex {
    TagMembers** tags;       /* by tag id; NULL for a tag no file wears */
    uint32_t     capacity;
    uint64_t     bytes;      /* what the index holds of the kernel heap */
    bool         said_short;
    TagFSLock    lock;
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

/* The least member of the window at or past `lo`. */
static bool WindowNext(const TagWindow* w, uint32_t lo, uint32_t* out)
{
    if (!w->bitmap) {
        uint32_t at = ArrayLowerBound(w->data, w->count, lo);
        if (at == w->count)
            return false;
        *out = ((const uint16_t*)w->data)[at];
        return true;
    }
    const uint64_t* bits = w->data;
    uint32_t        word = lo / 64u;
    uint64_t        v    = bits[word] & (~0ull << (lo % 64u));
    for (;;) {
        if (v) {
            *out = word * 64u + (uint32_t)__builtin_ctzll(v);
            return true;
        }
        if (++word == TAG_WINDOW_WORDS)
            return false;
        v = bits[word];
    }
}

/* Where the window of `hi` is, or would go. */
static uint32_t WindowAt(const TagMembers* m, uint32_t hi)
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

static TagMembers* MembersOf(const TagBitmapIndex* idx, uint16_t tag)
{
    return (uint32_t)tag < idx->capacity ? idx->tags[tag] : NULL;
}

static bool MembersHas(const TagMembers* m, uint32_t fid)
{
    if (!m)
        return false;
    uint32_t w = WindowAt(m, fid >> TAG_WINDOW_SHIFT);
    return w < m->windows && m->window[w].hi == (fid >> TAG_WINDOW_SHIFT) &&
           WindowHas(&m->window[w], fid & (TAG_WINDOW_IDS - 1u));
}

/* The least member at or past x. */
static bool MembersNext(const TagMembers* m, uint64_t x, uint64_t* out)
{
    if (!m || x > 0xFFFFFFFFull)
        return false;
    uint32_t hi = (uint32_t)(x >> TAG_WINDOW_SHIFT);
    for (uint32_t w = WindowAt(m, hi); w < m->windows; w++) {
        const TagWindow* win   = &m->window[w];
        uint32_t         start = win->hi == hi ? (uint32_t)(x & (TAG_WINDOW_IDS - 1u)) : 0;
        uint32_t         lo;
        if (WindowNext(win, start, &lo)) {
            *out = ((uint64_t)win->hi << TAG_WINDOW_SHIFT) | lo;
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// What an add needs, and installing it
// ----------------------------------------------------------------------------

typedef enum {
    WANT_NOTHING = 0,
    WANT_TAGS,               /* a table of `size` tags */
    WANT_MEMBERS,            /* the tag's TagMembers */
    WANT_WINDOWS,            /* a list of `size` windows */
    WANT_DATA                /* a window's storage: `size` array slots, or 0 for a bitmap */
} TagWant;

typedef struct {
    TagWant  want;
    uint32_t size;
    void*    piece;          /* taken for it, not yet installed */
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
static TagPiece IndexWants(const TagBitmapIndex* idx, uint16_t tag, uint32_t fid)
{
    TagPiece p = { WANT_NOTHING, 0, NULL };
    if ((uint32_t)tag >= idx->capacity) {
        uint32_t cap = idx->capacity ? idx->capacity : 64u;
        while (cap <= (uint32_t)tag)
            cap *= 2u;
        p.want = WANT_TAGS;
        p.size = cap;
        return p;
    }
    const TagMembers* m = idx->tags[tag];
    if (!m) {
        p.want = WANT_MEMBERS;
        return p;
    }
    uint32_t hi = fid >> TAG_WINDOW_SHIFT;
    uint32_t w  = WindowAt(m, hi);
    if (w == m->windows || m->window[w].hi != hi) {
        p.want = m->windows == m->room ? WANT_WINDOWS : WANT_DATA;
        p.size = m->windows == m->room ? (m->room ? m->room * 2u : 4u) : TAG_ARRAY_FIRST;
        return p;
    }
    const TagWindow* win = &m->window[w];
    if (!win->bitmap && win->count == win->room &&
        !WindowHas(win, fid & (TAG_WINDOW_IDS - 1u))) {
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
static void* IndexInstall(TagBitmapIndex* idx, uint16_t tag, uint32_t fid, TagPiece* p)
{
    TagPiece now = IndexWants(idx, tag, fid);
    if (now.want != p->want || now.size != p->size)
        return p->piece;

    void* old = NULL;
    idx->bytes += PieceBytes(p);
    switch (p->want) {
    case WANT_TAGS: {
        TagMembers** grown = p->piece;
        if (idx->capacity)
            memcpy(grown, idx->tags, sizeof(TagMembers*) * idx->capacity);
        memset(grown + idx->capacity, 0, sizeof(TagMembers*) * (p->size - idx->capacity));
        old = idx->tags;
        idx->bytes -= sizeof(TagMembers*) * idx->capacity;
        idx->tags     = grown;
        idx->capacity = p->size;
        break;
    }
    case WANT_MEMBERS:
        memset(p->piece, 0, sizeof(TagMembers));
        idx->tags[tag] = p->piece;
        break;
    case WANT_WINDOWS: {
        TagMembers* m = idx->tags[tag];
        if (m->windows)
            memcpy(p->piece, m->window, sizeof(TagWindow) * m->windows);
        old = m->window;
        idx->bytes -= sizeof(TagWindow) * m->room;
        m->window = p->piece;
        m->room   = p->size;
        break;
    }
    case WANT_DATA: {
        TagMembers* m  = idx->tags[tag];
        uint32_t    hi = fid >> TAG_WINDOW_SHIFT;
        uint32_t    w  = WindowAt(m, hi);
        if (w == m->windows || m->window[w].hi != hi) {
            memmove(&m->window[w + 1], &m->window[w], sizeof(TagWindow) * (m->windows - w));
            m->window[w] = (TagWindow){ .hi = (uint16_t)hi, .bitmap = 0, .count = 0,
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
static void IndexAdd(TagBitmapIndex* idx, uint16_t tag, uint32_t fid)
{
    TagMembers* m   = idx->tags[tag];
    TagWindow*  win = &m->window[WindowAt(m, fid >> TAG_WINDOW_SHIFT)];
    uint32_t    lo  = fid & (TAG_WINDOW_IDS - 1u);
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
static void IndexSayShort(TagBitmapIndex* idx, uint16_t tag, uint32_t fid, const TagPiece* p)
{
    TagFSLockTake(&idx->lock);
    bool     said  = idx->said_short;
    uint64_t bytes = idx->bytes;
    idx->said_short = true;
    TagFSLockDrop(&idx->lock);
    if (!said)
        kprintf("[TagFS] the tag index could not put file %u under tag %u: %zu bytes were "
                "refused, and it holds %llu bytes of kernel heap\n",
                fid, tag, PieceBytes(p), (unsigned long long)bytes);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

TagBitmapIndex* tag_bitmap_create(uint32_t initial_tag_cap)
{
    TagBitmapIndex* idx = kmalloc(sizeof(TagBitmapIndex));
    if (!idx)
        return NULL;
    memset(idx, 0, sizeof(*idx));
    if (initial_tag_cap) {
        idx->tags = kmalloc(sizeof(TagMembers*) * initial_tag_cap);
        if (!idx->tags) {
            kfree(idx);
            return NULL;
        }
        memset(idx->tags, 0, sizeof(TagMembers*) * initial_tag_cap);
        idx->capacity = initial_tag_cap;
        idx->bytes    = sizeof(TagMembers*) * initial_tag_cap;
    }
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
    for (uint32_t t = 0; t < idx->capacity; t++)
        if (idx->tags[t])
            MembersFree(idx->tags[t]);
    kfree(idx->tags);
    kfree(idx);
}

int tag_bitmap_set(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id)
{
    if (!idx || file_id == 0)
        return -1;

    TagPiece piece = { WANT_NOTHING, 0, NULL };
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
 * written into out[], the count answered. `cap` is the table's length as the
 * pieces staked before this one would leave it. */
static uint32_t IndexWantsAll(const TagBitmapIndex* idx, uint16_t tag, uint32_t fid,
                              uint32_t* cap, TagPiece out[TAG_STAKE_PIECES])
{
    uint32_t n = 0;
    const TagMembers* m = (uint32_t)tag < idx->capacity ? idx->tags[tag] : NULL;
    if ((uint32_t)tag >= *cap) {
        uint32_t grown = *cap ? *cap : 64u;
        while (grown <= (uint32_t)tag)
            grown *= 2u;
        out[n++] = (TagPiece){ WANT_TAGS, grown, NULL };
        *cap = grown;
    }
    if (!m) {
        out[n++] = (TagPiece){ WANT_MEMBERS, 0, NULL };
        out[n++] = (TagPiece){ WANT_WINDOWS, 4u, NULL };
        out[n++] = (TagPiece){ WANT_DATA, TAG_ARRAY_FIRST, NULL };
        return n;
    }
    TagPiece last = IndexWants(idx, tag, fid);
    if (last.want == WANT_WINDOWS) {
        out[n++] = last;
        last = (TagPiece){ WANT_DATA, TAG_ARRAY_FIRST, NULL };
    }
    if (last.want != WANT_NOTHING)
        out[n++] = last;
    return n;
}

error_t TagBitmapStakeFor(TagBitmapIndex* idx, const uint16_t* tags, uint32_t count,
                          uint32_t fid, TagBitmapStake** out)
{
    *out = NULL;
    if (!idx || count == 0)
        return OK;

    TagPiece* wants = kmalloc(sizeof(TagPiece) * TAG_STAKE_PIECES * count);
    if (!wants)
        return ERR_NO_MEMORY;
    uint32_t n = 0;
    TagFSLockTake(&idx->lock);
    uint32_t cap = idx->capacity;
    for (uint32_t i = 0; i < count; i++)
        n += IndexWantsAll(idx, tags[i], fid, &cap, &wants[n]);
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
            IndexSayShort(idx, tags[0], fid, &wants[i]);
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

bool TagBitmapStakeCovers(TagBitmapIndex* idx, const uint16_t* tags, uint32_t count,
                          uint32_t fid, TagBitmapStake* s)
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

void TagBitmapAddCovered(TagBitmapIndex* idx, const uint16_t* tags, uint32_t count, uint32_t fid)
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
 * goes to `freed[]` to be given back after the lock. Answers how many members
 * a bitmap window that fell to TAG_ARRAY_BACK still holds, to be made an array
 * again; 0 otherwise. */
static uint32_t IndexRemove(TagBitmapIndex* idx, uint16_t tag, uint32_t fid, void* freed[3])
{
    TagMembers* m = MembersOf(idx, tag);
    uint32_t    w = m ? WindowAt(m, fid >> TAG_WINDOW_SHIFT) : 0;
    if (!m || w == m->windows || m->window[w].hi != (fid >> TAG_WINDOW_SHIFT))
        return 0;
    TagWindow* win = &m->window[w];
    uint32_t   lo  = fid & (TAG_WINDOW_IDS - 1u);
    if (!WindowHas(win, lo))
        return 0;

    if (win->bitmap) {
        ((uint64_t*)win->data)[lo / 64u] &= ~(1ull << (lo % 64u));
    } else {
        uint16_t* low = win->data;
        uint32_t  at  = ArrayLowerBound(low, win->count, lo);
        memmove(&low[at], &low[at + 1], sizeof(uint16_t) * (win->count - at - 1));
    }
    win->count--;
    m->members--;
    uint32_t back = win->bitmap && win->count <= TAG_ARRAY_BACK ? win->count : 0;

    if (win->count == 0) {
        idx->bytes -= WindowBytes(win);
        freed[0] = win->data;
        memmove(&m->window[w], &m->window[w + 1], sizeof(TagWindow) * (m->windows - w - 1));
        m->windows--;
    }
    if (m->members == 0) {
        idx->bytes -= sizeof(TagWindow) * m->room + sizeof(TagMembers);
        freed[1] = m->window;
        freed[2] = m;
        idx->tags[tag] = NULL;
    }
    return back;
}

/* A bitmap window of `tag` that fell to TAG_ARRAY_BACK made an array again,
 * as long as its `count` members need, taken outside the lock; left a bitmap
 * when there is no memory, which costs room and nothing else. */
static void IndexShrink(TagBitmapIndex* idx, uint16_t tag, uint32_t fid, uint32_t count)
{
    uint32_t room = TAG_ARRAY_FIRST;
    while (room < count)
        room *= 2u;
    uint16_t* low = kmalloc(sizeof(uint16_t) * room);
    if (!low)
        return;
    void* old = low;
    TagFSLockTake(&idx->lock);
    TagMembers* m = MembersOf(idx, tag);
    uint32_t    w = m ? WindowAt(m, fid >> TAG_WINDOW_SHIFT) : 0;
    if (m && w < m->windows && m->window[w].hi == (fid >> TAG_WINDOW_SHIFT) &&
        m->window[w].bitmap && m->window[w].count <= room) {
        TagWindow*      win  = &m->window[w];
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
    kfree(old);
}

int tag_bitmap_clear(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id)
{
    if (!idx)
        return -1;
    void* freed[3] = { NULL, NULL, NULL };
    TagFSLockTake(&idx->lock);
    bool     known = MembersOf(idx, tag_id) != NULL;
    uint32_t back  = IndexRemove(idx, tag_id, file_id, freed);
    TagFSLockDrop(&idx->lock);
    for (uint32_t i = 0; i < 3; i++)
        kfree(freed[i]);
    if (back)
        IndexShrink(idx, tag_id, file_id, back);
    return known ? 0 : -1;
}

void TagBitmapLeave(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id)
{
    if (!idx)
        return;
    void* freed[3] = { NULL, NULL, NULL };
    TagFSLockTake(&idx->lock);
    (void)IndexRemove(idx, tag_id, file_id, freed);
    TagFSLockDrop(&idx->lock);
    for (uint32_t i = 0; i < 3; i++)
        kfree(freed[i]);
}

void tag_bitmap_remove_file(TagBitmapIndex* idx, uint32_t file_id,
                            const uint16_t* tag_ids, uint32_t tag_count)
{
    if (!idx)
        return;
    if (tag_ids) {
        for (uint32_t i = 0; i < tag_count; i++)
            tag_bitmap_clear(idx, tag_ids[i], file_id);
        return;
    }
    /* No record to say which tags the file wore: every tag is asked, one hold
     * of the lock each, so nobody waits on the whole walk. */
    TagFSLockTake(&idx->lock);
    uint32_t tags = idx->capacity;
    TagFSLockDrop(&idx->lock);
    for (uint32_t t = 0; t < tags && t < TAGFS_INVALID_TAG_ID; t++)
        if (TagBitmapHas(idx, (uint16_t)t, file_id))
            tag_bitmap_clear(idx, (uint16_t)t, file_id);
}

bool TagBitmapHas(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id)
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
    const uint16_t*  tag_ids;
    uint32_t         tag_count;
    const TagIdList* groups;
    uint32_t         group_count;
    uint32_t         lead;           /* the term asked first */
} TagQuery;

static uint32_t QueryTerms(const TagQuery* q)
{
    return q->tag_count + q->group_count;
}

static void QueryTerm(const TagQuery* q, uint32_t t, const uint16_t** ids, uint32_t* n)
{
    if (t < q->tag_count) {
        *ids = &q->tag_ids[t];
        *n   = 1;
    } else {
        *ids = q->groups[t - q->tag_count].ids;
        *n   = q->groups[t - q->tag_count].count;
    }
}

static uint64_t TermSize(const TagBitmapIndex* idx, const TagQuery* q, uint32_t t)
{
    const uint16_t* ids;
    uint32_t        n;
    uint64_t        size = 0;
    QueryTerm(q, t, &ids, &n);
    for (uint32_t i = 0; i < n; i++) {
        const TagMembers* m = MembersOf(idx, ids[i]);
        size += m ? m->members : 0;
    }
    return size;
}

static bool TermNext(const TagBitmapIndex* idx, const TagQuery* q, uint32_t t, uint64_t x,
                     uint64_t* out)
{
    const uint16_t* ids;
    uint32_t        n;
    bool            any = false;
    QueryTerm(q, t, &ids, &n);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t y;
        if (MembersNext(MembersOf(idx, ids[i]), x, &y) && (!any || y < *out)) {
            *out = y;
            any  = true;
        }
    }
    return any;
}

typedef enum { QUERY_MATCH, QUERY_END, QUERY_PAUSE } QueryStep;

/* From *x, the next id every term holds: MATCH with *x on it, END when there
 * is none, PAUSE with *x where to go on once the budget is spent. */
static QueryStep QueryLeap(const TagBitmapIndex* idx, const TagQuery* q, uint64_t* x,
                           uint32_t* budget)
{
    uint32_t terms = QueryTerms(q);
    for (;;) {
        bool moved = false;
        for (uint32_t k = 0; k < terms && !moved; k++) {
            uint32_t t = k == 0 ? q->lead : (k <= q->lead ? k - 1 : k);
            uint64_t y;
            if (*budget == 0)
                return QUERY_PAUSE;
            (*budget)--;
            if (!TermNext(idx, q, t, *x, &y))
                return QUERY_END;
            if (y != *x) {
                *x    = y;
                moved = true;
            }
        }
        if (!moved)
            return QUERY_MATCH;
    }
}

static bool QueryExcludes(const TagBitmapIndex* idx, const uint16_t* unless, uint32_t n,
                          uint32_t fid)
{
    for (uint32_t i = 0; i < n; i++)
        if (MembersHas(MembersOf(idx, unless[i]), fid))
            return true;
    return false;
}

int TagBitmapQueryPage(TagBitmapIndex* idx,
                       const uint16_t* tag_ids, uint32_t tag_count,
                       const TagIdList* groups, uint32_t group_count,
                       const uint16_t* unless, uint32_t unless_count,
                       uint32_t from, uint32_t* out, uint32_t room,
                       bool want_total, TagFSPage* page)
{
    memset(page, 0, sizeof(*page));
    if (!idx || (room && !out))
        return -1;
    TagQuery q = { tag_ids, tag_count, groups, group_count, 0 };
    if (QueryTerms(&q) == 0)
        return 0;

    uint64_t x    = from ? from : 1;
    bool     done = false;
    bool     led  = false;
    while (!done) {
        uint32_t budget = TAG_QUERY_STEPS;
        TagFSLockTake(&idx->lock);
        if (!led) {
            uint64_t least = TermSize(idx, &q, 0);
            for (uint32_t t = 1; t < QueryTerms(&q); t++) {
                uint64_t size = TermSize(idx, &q, t);
                if (size < least) {
                    least  = size;
                    q.lead = t;
                }
            }
            led = true;
        }
        for (;;) {
            QueryStep step = QueryLeap(idx, &q, &x, &budget);
            if (step == QUERY_PAUSE)
                break;
            if (step == QUERY_END) {
                done = true;
                break;
            }
            uint32_t fid = (uint32_t)x;
            x++;
            if (QueryExcludes(idx, unless, unless_count, fid))
                continue;
            if (page->delivered < room) {
                out[page->delivered++] = fid;
            } else if (!page->next) {
                page->next = fid;
                if (!want_total) {
                    done = true;
                    break;
                }
            }
            if (want_total)
                page->total++;
        }
        TagFSLockDrop(&idx->lock);
    }
    return 0;
}
