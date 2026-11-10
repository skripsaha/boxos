#include "use_context.h"
#include "tagfs.h"
#include "klib.h"
#include "atomics.h"
#include "process.h"
#include "cabin.h"
#include "amp.h"
#include "touch.h"
#include "boxos_decks.h"

/* One tag of a list: where its canonical text lies in the list's text, and
 * how much of it is the key. */
typedef struct UseSpan {
    uint64_t at;
    uint32_t len;
    uint32_t key_len;
} UseSpan;

/*
 * A context, built once and never changed after — except `ids`, the mounted
 * volume's number for each tag, which only a binding writes: each slot goes
 * from TAGFS_INVALID_TAG_ID to a number once, by compare-and-swap, and back
 * only when the volume's book is let go of. One allocation holds the header
 * and the three arrays; `said` is shared with whoever remembers it.
 */
struct UseList {
    uint32_t  refs;
    uint32_t  count;
    uint32_t  unbound;   /* slots of `ids` still without a number */
    UseText  *said;      /* the canonical text, in the order the person said it */
    UseSpan  *spans;     /* count of them, in that order */
    uint32_t *sorted;    /* the indices of `spans` in TagSpellingCompare order */
    uint32_t *ids;
};

/*
 * What a verdict is counted against: the context's numbers at one moment,
 * those under 64 as a mask and the rest sorted, so a cabin's tag is found by
 * halves, and how many tags the context has. A tag the volume has no number
 * for is in that count and in neither set — nothing can wear a tag that does
 * not exist — and the tags that do exist are worn all the same. Immutable;
 * the newest one is kept for the next asker while the context's shape stands.
 */
typedef struct UseSnapshot {
    uint32_t refs;
    uint64_t shape;
    uint32_t total;
    uint64_t bits;
    uint32_t over_count;
    uint32_t over[];
} UseSnapshot;

/* A verdict word: the epoch it was counted at, over the Fit it found. */
#define USE_FIT_BITS 9u
#define USE_FIT_MASK ((1u << USE_FIT_BITS) - 1u)

/*
 * The context as it stands, under `lock`: a leaf taken for pointer stores and
 * reference counts only — no lookup, no allocation and no print is ever made
 * under it, so a caller holding any other lock may take it. `said` counts the
 * sayings — every set, clear and adoption — so the volume can tell a later
 * word from an earlier one, and a recall can tell whether one came while it
 * read.
 */
static struct {
    UseList     *list;
    uint64_t     said;
    UseSnapshot *snapshot;
    spinlock_t   lock;
} g_use;

/* The verdict epoch (starts at 1, so a fresh cabin's word of 0 is stale), the
 * context's shape (moves with what it says and what it is bound to), whether
 * the list may still wait for a number, and whether a recount is owed. */
static uint64_t g_use_epoch;
static uint64_t g_use_shape;
static uint32_t g_use_waiting;
static uint32_t g_use_owed;

#ifdef CONFIG_VOLUME_TESTS
/* Per core, so a count taken across one core's work is that work's alone. */
static uint64_t g_use_compares[MAX_CORES];
#define USE_COMPARED() __atomic_add_fetch(&g_use_compares[amp_get_core_index()], 1, \
                                          __ATOMIC_RELAXED)

uint64_t UseContextCompares(void)
{
    return __atomic_load_n(&g_use_compares[amp_get_core_index()], __ATOMIC_RELAXED);
}
#else
#define USE_COMPARED() ((void)0)
#endif

/* -------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */

UseText *UseTextRoom(size_t bytes)
{
    UseText *t = kmalloc(sizeof(UseText) + bytes + 1);
    if (!t) return NULL;
    t->refs        = 1;
    t->bytes       = bytes;
    t->text[bytes] = '\0';
    return t;
}

UseText *UseTextMake(const char *text, size_t bytes)
{
    UseText *t = UseTextRoom(bytes);
    if (t && bytes) memcpy(t->text, text, bytes);
    return t;
}

UseText *UseTextHold(UseText *t)
{
    if (t) __atomic_add_fetch(&t->refs, 1, __ATOMIC_RELAXED);
    return t;
}

void UseTextDrop(UseText *t)
{
    if (t && __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree(t);
}

bool UseTextSame(const UseText *a, const UseText *b)
{
    if (a == b) return true;
    if (!a || !b || a->bytes != b->bytes) return false;
    return memcmp(a->text, b->text, a->bytes) == 0;
}

/* -------------------------------------------------------------------------
 * Lists
 * ------------------------------------------------------------------------- */

static UseList *UseListMake(uint32_t count, UseText *said)
{
    size_t   each = sizeof(UseSpan) + sizeof(uint32_t) + sizeof(uint32_t);
    UseList *l    = kmalloc(sizeof(UseList) + (size_t)count * each);
    if (!l) return NULL;
    l->refs    = 1;
    l->count   = count;
    l->unbound = count;
    l->said    = said;
    l->spans   = (UseSpan *)(l + 1);
    l->sorted  = (uint32_t *)(l->spans + count);
    l->ids     = (uint32_t *)(l->sorted + count);
    for (uint32_t i = 0; i < count; i++)
        l->ids[i] = TAGFS_INVALID_TAG_ID;
    return l;
}

static UseList *UseListHold(UseList *l)
{
    if (l) __atomic_add_fetch(&l->refs, 1, __ATOMIC_RELAXED);
    return l;
}

void UseListDrop(UseList *l)
{
    if (!l || __atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    UseTextDrop(l->said);
    kfree(l);
}

uint32_t UseListCount(const UseList *l)
{
    return l ? l->count : 0;
}

UseText *UseListText(const UseList *l)
{
    return l ? l->said : NULL;
}

TagSpelling UseListTag(const UseList *l, uint32_t index)
{
    const UseSpan *s   = &l->spans[index];
    const char    *at  = l->said->text + s->at;
    TagSpelling    tag = { at, NULL, s->key_len, 0 };
    if (s->len > s->key_len) {
        tag.value     = at + s->key_len + 1;
        tag.value_len = s->len - s->key_len - 1;
    }
    return tag;
}

/* The spoken tags by spelling, and the order they were said in among equals —
 * so the first saying of a tag sorts first of its kind and is the one kept. */
static int UseSpokenOrder(const void *a, const void *b, void *ctx)
{
    const TagSpelling *spoken = ctx;
    uint32_t i = *(const uint32_t *)a;
    uint32_t j = *(const uint32_t *)b;
    int      by = TagSpellingCompare(&spoken[i], &spoken[j]);
    if (by != 0) return by;
    return (i > j) - (i < j);
}

#define USE_REPEAT UINT32_MAX

/* The spoken tags written into a list: the canonical text in the order they
 * were said, repeats left out, and the sorted index read off `order`. `place`
 * maps a spoken tag to its index in the list, or USE_REPEAT. */
static void UseListFill(UseList *l, const TagSpelling *tags, const uint32_t *order,
                        const uint32_t *place, uint32_t spoken)
{
    char  *text = l->said->text;
    size_t pos  = 0;
    for (uint32_t k = 0; k < spoken; k++) {
        if (place[k] == USE_REPEAT) continue;
        uint32_t j   = place[k];
        size_t   len = TagSpellingTextLength(&tags[k]);
        TagSpellingText(&tags[k], text + pos);
        l->spans[j].at      = pos;
        l->spans[j].len     = (uint32_t)len;
        l->spans[j].key_len = (uint32_t)tags[k].key_len;
        pos += len;
        if (j + 1 < l->count) text[pos++] = ',';
    }
    uint32_t n = 0;
    for (uint32_t s = 0; s < spoken; s++)
        if (place[order[s]] != USE_REPEAT)
            l->sorted[n++] = place[order[s]];
}

/*
 * Spelled tags made a list: repeats found by sorting — n log n compares, not
 * every tag against every other — and the first saying of each kept, in the
 * order said. The spellings point into text the caller holds while this
 * runs; the list copies what it keeps. *out is NULL, with OK, for no tag.
 */
static error_t UseListOfSpellings(const TagSpelling *tags, uint32_t spoken, UseList **out)
{
    *out = NULL;
    if (spoken == 0) return OK;

    uint32_t *order = kmalloc(sizeof(uint32_t) * spoken);
    uint32_t *place = kmalloc(sizeof(uint32_t) * spoken);
    error_t   rc    = ERR_NO_MEMORY;
    if (!order || !place) goto done;

    for (uint32_t k = 0; k < spoken; k++)
        order[k] = k;
    KSort(order, spoken, sizeof(uint32_t), UseSpokenOrder, (void *)tags);

    for (uint32_t s = 0; s < spoken; s++) {
        bool repeat = s > 0 && TagSpellingCompare(&tags[order[s]], &tags[order[s - 1]]) == 0;
        place[order[s]] = repeat ? USE_REPEAT : 0;
    }
    uint32_t kept  = 0;
    size_t   bytes = 0;
    for (uint32_t k = 0; k < spoken; k++) {
        if (place[k] == USE_REPEAT) continue;
        place[k] = kept++;
        bytes   += TagSpellingTextLength(&tags[k]);
    }
    bytes += kept - 1;   /* a comma between each two */

    UseText *text = UseTextRoom(bytes);
    UseList *l    = text ? UseListMake(kept, text) : NULL;
    if (!l) {
        UseTextDrop(text);
        goto done;
    }
    UseListFill(l, tags, order, place, spoken);
    *out = l;
    rc   = OK;
done:
    kfree(order);
    kfree(place);
    return rc;
}

/*
 * A comma-separated list read into a context. The whole list is spelled
 * before anything is sized by it, and a tag that does not spell refuses it
 * with the spelling's code. *out is NULL, with OK, for a list that names no
 * tag.
 */
static error_t UseListParse(const char *list, size_t len, UseList **out)
{
    *out = NULL;
    uint32_t spoken = 0;
    error_t  rc     = TagListCount(list, len, &spoken);
    if (rc != OK || spoken == 0) return rc;

    TagSpelling *tags = kmalloc(sizeof(TagSpelling) * spoken);
    if (!tags) return ERR_NO_MEMORY;
    size_t at = 0;
    for (uint32_t k = 0; k < spoken; k++)
        (void)TagListNext(list, len, &at, &tags[k]);   /* judged whole above */
    rc = UseListOfSpellings(tags, spoken, out);
    kfree(tags);
    return rc;
}

/* Where `tag` is in the list, found by halves over the sorted index. */
static bool UseListFind(const UseList *l, const TagSpelling *tag, uint32_t *out_index)
{
    uint32_t lo = 0, hi = l->count;
    while (lo < hi) {
        uint32_t    mid   = lo + (hi - lo) / 2;
        TagSpelling there = UseListTag(l, l->sorted[mid]);
        int         by    = TagSpellingCompare(tag, &there);
        USE_COMPARED();
        if (by == 0) {
            *out_index = l->sorted[mid];
            return true;
        }
        if (by < 0) hi = mid;
        else        lo = mid + 1;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * The context
 * ------------------------------------------------------------------------- */

UseList *UseContextHold(void)
{
    spin_lock(&g_use.lock);
    UseList *l = UseListHold(g_use.list);
    spin_unlock(&g_use.lock);
    return l;
}

/* Every verdict is stale from here on, and a recount is owed. */
static void UseEpochRaise(void)
{
    __atomic_add_fetch(&g_use_epoch, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_use_owed, 1u, __ATOMIC_RELEASE);
}

/* The context changed what it says or what it is bound to — said after the
 * change is made. The shape moves before the epoch, so a snapshot taken for
 * the new epoch is never one of the old shape. */
static void UseContextChanged(void)
{
    __atomic_add_fetch(&g_use_shape, 1, __ATOMIC_ACQ_REL);
    UseEpochRaise();
}

void UseContextCabinChanged(void)
{
    UseEpochRaise();
}

/* What a conditional saying was made over: the list the context held and
 * how many words had been said. */
typedef struct UseSeen {
    const UseList *list;
    uint64_t       said;
} UseSeen;

/*
 * `l` becomes the context — NULL for none. Unconditionally for a person's
 * word (`over` NULL); for a volume's memory or a change only while the
 * context is still what was seen and nobody has said anything since. One
 * store under the lock, the word's text and number taken under it for `word`;
 * the old list is let go of outside it. A word that says something else than
 * the context did is said on USE_TOUCH_TAG after it is made, outside every
 * lock.
 */
static bool UseContextInstall(UseList *l, const UseSeen *over, UseWord *word)
{
    UseListHold(l);
    spin_lock(&g_use.lock);
    bool     take   = !over || (g_use.list == over->list && g_use.said == over->said);
    UseList *gone   = l;
    uint64_t saying = 0;
    if (take) {
        gone       = g_use.list;
        g_use.list = l;
        saying     = ++g_use.said;
        __atomic_store_n(&g_use_waiting, (l && l->count) ? 1u : 0u, __ATOMIC_SEQ_CST);
        if (word) {
            word->said   = UseTextHold(l ? l->said : NULL);
            word->saying = saying;
        }
    }
    spin_unlock(&g_use.lock);
    bool differs = take && !UseTextSame(gone ? gone->said : NULL, l ? l->said : NULL);
    UseListDrop(gone);
    if (take) UseContextChanged();
    if (differs) TouchPublish(USE_TOUCH_TAG, &saying, sizeof(saying));
    return take;
}

/* A slot given its number, once. The last one bound stops BindTag looking. */
static bool UseSlotBind(UseList *l, uint32_t index, uint32_t id)
{
    uint32_t none = TAGFS_INVALID_TAG_ID;
    if (!__atomic_compare_exchange_n(&l->ids[index], &none, id, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return false;
    if (__atomic_sub_fetch(&l->unbound, 1, __ATOMIC_ACQ_REL) == 0) {
        spin_lock(&g_use.lock);
        if (g_use.list == l && __atomic_load_n(&l->unbound, __ATOMIC_ACQUIRE) == 0)
            __atomic_store_n(&g_use_waiting, 0u, __ATOMIC_SEQ_CST);
        spin_unlock(&g_use.lock);
    }
    return true;
}

/*
 * Every tag of `l` without a number looked up in the mounted volume — looked
 * up, never interned: saying a context writes no page of the volume's book.
 * The door is held across each look and its binding, so a number is never
 * bound after the book it came from was let go of (UseContextUnbind runs with
 * the door drained). A tag interned after its look binds itself through
 * UseContextBindTag: the list is the context before the first look.
 */
static void UseListBind(UseList *l)
{
    if (!l || !tagfs_enter()) return;
    uint32_t bound = 0;
    for (uint32_t i = 0; i < l->count; i++) {
        if (__atomic_load_n(&l->ids[i], __ATOMIC_ACQUIRE) != TAGFS_INVALID_TAG_ID) continue;
        TagSpelling tag = UseListTag(l, i);
        uint32_t    id  = TagFSLookupSpelling(&tag);
        if (id != TAGFS_INVALID_TAG_ID && UseSlotBind(l, i, id)) bound++;
    }
    tagfs_leave();
    if (bound) UseContextChanged();
}

error_t UseContextSet(const char *list, size_t len, UseWord *word)
{
    UseList *l  = NULL;
    error_t  rc = UseListParse(list, len, &l);
    if (rc != OK) return rc;
    if (!l) {
        UseContextClear(word);
        return OK;
    }
    UseContextInstall(l, NULL, word);
    UseListBind(l);
    UseListDrop(l);
    return OK;
}

void UseContextClear(UseWord *word)
{
    UseContextInstall(NULL, NULL, word);
}

/*
 * The list `now` would be after a change: its own tags but those of `less`,
 * then the tags of `more` it does not say yet and `less` does not take out,
 * in the order said. *differs false when that is `now` exactly — then *out is
 * NULL and nothing is built; *out NULL with *differs true when nothing is
 * left. The spellings point into the three lists, which the caller holds.
 */
static error_t UseListChanged(const UseList *now, const UseList *more, const UseList *less,
                              UseList **out, bool *differs)
{
    *out     = NULL;
    *differs = false;
    uint32_t room = UseListCount(now) + UseListCount(more);
    if (room == 0) return OK;

    TagSpelling *kept = kmalloc(sizeof(TagSpelling) * room);
    if (!kept) return ERR_NO_MEMORY;
    uint32_t n = 0, at;
    for (uint32_t i = 0; i < UseListCount(now); i++) {
        TagSpelling tag = UseListTag(now, i);
        if (less && UseListFind(less, &tag, &at)) {
            *differs = true;
            continue;
        }
        kept[n++] = tag;
    }
    for (uint32_t i = 0; i < UseListCount(more); i++) {
        TagSpelling tag = UseListTag(more, i);
        if ((now && UseListFind(now, &tag, &at)) || (less && UseListFind(less, &tag, &at)))
            continue;
        kept[n++] = tag;
        *differs  = true;
    }
    error_t rc = *differs ? UseListOfSpellings(kept, n, out) : OK;
    kfree(kept);
    return rc;
}

/*
 * The change is built from the context as it was seen, outside every lock,
 * and set only over that same context; a word said meanwhile is looked at
 * again and changed in turn. Each round that does not set lost to a word that
 * did, so the change is never starved by its own retries.
 */
error_t UseContextChange(const char *add, size_t add_len, const char *drop, size_t drop_len,
                         UseWord *word, bool *changed)
{
    UseList *more = NULL, *less = NULL;
    error_t  rc   = UseListParse(add, add_len, &more);
    if (rc == OK) rc = UseListParse(drop, drop_len, &less);
    if (changed) *changed = false;

    while (rc == OK) {
        spin_lock(&g_use.lock);
        UseList *now  = UseListHold(g_use.list);
        UseSeen  seen = { now, g_use.said };
        spin_unlock(&g_use.lock);

        UseList *next    = NULL;
        bool     differs = false;
        rc = UseListChanged(now, more, less, &next, &differs);
        if (rc == OK && !differs) {
            if (word) {
                word->said   = UseTextHold(now ? now->said : NULL);
                word->saying = seen.said;
            }
            UseListDrop(now);
            break;
        }
        bool took = rc == OK && UseContextInstall(next, &seen, word);
        UseListDrop(now);
        if (took) {
            UseListBind(next);
            if (changed) *changed = true;
        }
        UseListDrop(next);
        if (took) break;
    }
    UseListDrop(more);
    UseListDrop(less);
    return rc;
}

void UseContextBindTag(const TagSpelling *tag, uint32_t tag_id)
{
    if (!tag || tag_id == TAGFS_INVALID_TAG_ID) return;
    if (!__atomic_load_n(&g_use_waiting, __ATOMIC_SEQ_CST)) return;

    UseList *l = UseContextHold();
    uint32_t at;
    if (l && UseListFind(l, tag, &at) && UseSlotBind(l, at, tag_id))
        UseContextChanged();
    UseListDrop(l);
}

void UseContextUnbind(void)
{
    UseList *l = UseContextHold();
    if (!l) return;
    for (uint32_t i = 0; i < l->count; i++)
        __atomic_store_n(&l->ids[i], (uint32_t)TAGFS_INVALID_TAG_ID, __ATOMIC_RELAXED);
    __atomic_store_n(&l->unbound, l->count, __ATOMIC_RELEASE);
    spin_lock(&g_use.lock);
    if (g_use.list == l)
        __atomic_store_n(&g_use_waiting, 1u, __ATOMIC_SEQ_CST);
    spin_unlock(&g_use.lock);
    UseListDrop(l);
    UseContextChanged();
}

/* -------------------------------------------------------------------------
 * The scheduler's verdict
 * ------------------------------------------------------------------------- */

static void UseSnapshotDrop(UseSnapshot *s)
{
    if (s && __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree(s);
}

static int UseIdOrder(const void *a, const void *b, void *ctx)
{
    (void)ctx;
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* The numbers of `l` as they are bound now, labelled with `shape` — read
 * before them, so a binding made while they are read makes the label stale
 * rather than the numbers wrong. */
static UseSnapshot *UseSnapshotBuild(const UseList *l, uint64_t shape)
{
    uint32_t     room = l ? l->count : 0;
    UseSnapshot *s    = kmalloc(sizeof(UseSnapshot) + (size_t)room * sizeof(uint32_t));
    if (!s) return NULL;
    s->refs       = 1;
    s->shape      = shape;
    s->total      = room;
    s->bits       = 0;
    s->over_count = 0;
    for (uint32_t i = 0; i < room; i++) {
        uint32_t id = __atomic_load_n(&l->ids[i], __ATOMIC_ACQUIRE);
        if (id == TAGFS_INVALID_TAG_ID) continue;
        if (id < 64) s->bits |= (uint64_t)1 << id;
        else         s->over[s->over_count++] = id;
    }
    KSort(s->over, s->over_count, sizeof(uint32_t), UseIdOrder, NULL);
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < s->over_count; i++)
        if (distinct == 0 || s->over[distinct - 1] != s->over[i])
            s->over[distinct++] = s->over[i];
    s->over_count = distinct;
    return s;
}

/* The snapshot of the context's shape now: the kept one while the shape
 * stands, else one built outside the lock and kept for the next asker. NULL
 * only with no memory for one. */
static UseSnapshot *UseSnapshotHold(void)
{
    uint64_t shape = __atomic_load_n(&g_use_shape, __ATOMIC_ACQUIRE);

    spin_lock(&g_use.lock);
    UseSnapshot *s = g_use.snapshot;
    bool         fresh = s && s->shape == shape;
    if (fresh) __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
    UseList *l = fresh ? NULL : UseListHold(g_use.list);
    spin_unlock(&g_use.lock);
    if (fresh) return s;

    s = UseSnapshotBuild(l, shape);
    UseListDrop(l);
    if (!s) return NULL;

    UseSnapshot *old = NULL;
    spin_lock(&g_use.lock);
    if (!g_use.snapshot || g_use.snapshot->shape < shape) {
        old            = g_use.snapshot;
        g_use.snapshot = s;
        __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
    }
    spin_unlock(&g_use.lock);
    UseSnapshotDrop(old);
    return s;
}

static bool UseSnapshotHas(const UseSnapshot *s, uint32_t id)
{
    uint32_t lo = 0, hi = s->over_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        USE_COMPARED();
        if (s->over[mid] == id) return true;
        if (id < s->over[mid]) hi = mid;
        else                   lo = mid + 1;
    }
    return false;
}

/*
 * How much of the snapshot's context `cabin` wears: its Fit, the share of the
 * context's tags among the cabin's, out of USE_CONTEXT_FIT_FULL. The cabin's
 * tags are read without its lock: every change to them raises the epoch after
 * it is made, so a count taken across a change is labelled with an epoch
 * already stale and is counted again. A cabin's numbers past 63 are distinct,
 * so each one found in the snapshot is one more of the context's tags worn —
 * every worn number looked up by halves, no sort and no copy of the cabin's.
 */
static uint32_t UseSnapshotFit(const UseSnapshot *s, const cabin_t *cabin)
{
    if (s->total == 0) return 0;
    uint64_t worn  = __atomic_load_n(&cabin->tag_bits, __ATOMIC_ACQUIRE);
    uint64_t count = CountSetBits(worn & s->bits);
    if (s->over_count != 0) {
        uint32_t        n   = __atomic_load_n(&cabin->tag_overflow_count, __ATOMIC_ACQUIRE);
        const uint32_t *ids = __atomic_load_n(&cabin->tag_overflow_ids, __ATOMIC_ACQUIRE);
        for (uint32_t i = 0; ids && i < n; i++)
            if (UseSnapshotHas(s, __atomic_load_n(&ids[i], __ATOMIC_RELAXED))) count++;
    }
    return (uint32_t)(count * USE_CONTEXT_FIT_FULL / s->total);
}

uint32_t UseContextFit(const struct process_t *proc)
{
    if (!proc || !proc->cabin) return 0;
    uint64_t word = __atomic_load_n(&proc->cabin->use_verdict, __ATOMIC_ACQUIRE);
    if ((word >> USE_FIT_BITS) != __atomic_load_n(&g_use_epoch, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_use_owed, __ATOMIC_RELAXED))
        __atomic_store_n(&g_use_owed, 1u, __ATOMIC_RELEASE);
    return (uint32_t)(word & USE_FIT_MASK);
}

void UseContextVerdictRefresh(struct process_t *proc)
{
    if (!proc || !proc->cabin) return;
    cabin_t *cabin = proc->cabin;

    uint64_t epoch = __atomic_load_n(&g_use_epoch, __ATOMIC_ACQUIRE);
    uint64_t word  = __atomic_load_n(&cabin->use_verdict, __ATOMIC_ACQUIRE);
    if ((word >> USE_FIT_BITS) >= epoch) return;

    UseSnapshot *s = UseSnapshotHold();
    if (!s) {
        __atomic_store_n(&g_use_owed, 1u, __ATOMIC_RELEASE);
        return;
    }
    uint64_t verdict = (epoch << USE_FIT_BITS) | UseSnapshotFit(s, cabin);
    UseSnapshotDrop(s);

    /* Never over a newer word: a recount that began later may have landed
     * first. */
    while ((word >> USE_FIT_BITS) < epoch &&
           !__atomic_compare_exchange_n(&cabin->use_verdict, &word, verdict, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        ;
}

/*
 * The pass for cabins that never enter the kernel: every strand's cabin with
 * a stale word recounted, each strand held by a reference and counted with no
 * lock held. A strand born after the look is counted when it is first asked
 * about, which owes the next pass.
 */
void UseContextServiceIfPending(void)
{
    if (!__atomic_load_n(&g_use_owed, __ATOMIC_RELAXED)) return;
    if (!__atomic_exchange_n(&g_use_owed, 0u, __ATOMIC_ACQ_REL)) return;

    uint32_t room = process_get_count();
    if (room == 0) return;
    uint32_t *pids = kmalloc(sizeof(uint32_t) * room);
    if (!pids) {
        __atomic_store_n(&g_use_owed, 1u, __ATOMIC_RELEASE);
        return;
    }
    uint32_t n = process_snapshot_pids(pids, room);
    for (uint32_t i = 0; i < n; i++) {
        process_t *proc = process_find_ref(pids[i]);
        if (!proc) continue;
        UseContextVerdictRefresh(proc);
        process_ref_dec(proc);
    }
    kfree(pids);
}

/* -------------------------------------------------------------------------
 * The volume's memory
 * ------------------------------------------------------------------------- */

void UseContextRemember(bool *remembered)
{
    bool kept = false;

    spin_lock(&g_use.lock);
    UseList *l    = UseListHold(g_use.list);
    uint64_t said = g_use.said;
    spin_unlock(&g_use.lock);

    /* Through the door, like every caller from outside the volume: the ground
     * may be clearing under a medium that just left. */
    if (tagfs_enter()) {
        (void)tagfs_remember_use_context(l ? l->said : NULL, said, &kept);
        tagfs_leave();
    }
    UseListDrop(l);
    if (remembered) *remembered = kept;
}

/* The recall line: how many tags, how many bytes, the first few — each cut
 * where a line stops being read — and how many more. Never the whole list: it
 * has no ceiling, and the line is printed on the mount road under the print
 * lock. */
#define USE_RECALL_SHOWN      3u
#define USE_RECALL_TAG_BYTES  48u

static void UseRecallSay(const UseList *l)
{
    char     shown[USE_RECALL_SHOWN * (USE_RECALL_TAG_BYTES + 5u) + 16u];
    size_t   pos = 0;
    uint32_t n   = l->count < USE_RECALL_SHOWN ? l->count : USE_RECALL_SHOWN;
    shown[0] = '\0';
    for (uint32_t i = 0; i < n; i++) {
        const UseSpan *s   = &l->spans[i];
        uint32_t       cut = s->len > USE_RECALL_TAG_BYTES ? USE_RECALL_TAG_BYTES : s->len;
        pos += (size_t)ksnprintf(shown + pos, sizeof(shown) - pos, "%s%.*s%s",
                                 i ? ", " : "", (int)cut, l->said->text + s->at,
                                 cut < s->len ? "..." : "");
    }
    if (l->count > n)
        ksnprintf(shown + pos, sizeof(shown) - pos, " +%u", l->count - n);
    kprintf("[USE] the volume remembers a Use Context of %u tag(s), %llu bytes: %s\n",
            l->count, (unsigned long long)l->said->bytes, shown);
}

static void UseRecallRefused(const UseText *memory, error_t why)
{
    size_t cut = memory->bytes > USE_RECALL_TAG_BYTES ? USE_RECALL_TAG_BYTES : memory->bytes;
    kprintf("[USE] the volume remembers a Use Context this kernel cannot spell "
            "(error %d), %llu bytes: %.*s%s\n", (int)why,
            (unsigned long long)memory->bytes, (int)cut, memory->text,
            cut < memory->bytes ? "..." : "");
}

/*
 * The volume's memory taken up as the context: read here, outside every lock,
 * and set only while the machine still holds none and nobody has said
 * anything since `looked`. True when the recall is done with — taken up, or
 * nothing this kernel can take up; false when a word was said meanwhile, and
 * it stands.
 */
static bool UseContextAdopt(const UseText *memory, uint64_t looked)
{
    UseList *l  = NULL;
    error_t  rc = UseListParse(memory->text, memory->bytes, &l);
    if (rc != OK) {
        UseRecallRefused(memory, rc);
        return true;
    }
    if (!l) return true;

    UseSeen nothing = { NULL, looked };
    bool    adopted = UseContextInstall(l, &nothing, NULL);
    if (adopted) {
        UseListBind(l);
        UseRecallSay(l);
    }
    UseListDrop(l);
    return adopted;
}

void UseContextRecall(void)
{
    spin_lock(&g_use.lock);
    bool     holds  = g_use.list != NULL;
    uint64_t looked = g_use.said;
    spin_unlock(&g_use.lock);

    if (!holds) {
        /* One take of what the volume remembers. */
        UseText *memory = tagfs_recall_use_context(NULL);
        bool     done   = !memory || UseContextAdopt(memory, looked);
        UseTextDrop(memory);
        if (done) return;
    }

    /* The machine keeps its own: bound on this volume, and told to it. */
    UseList *own = UseContextHold();
    UseListBind(own);
    UseListDrop(own);
    bool kept = false;
    UseContextRemember(&kept);
    if (!kept)
        kprintf("[USE] the context stands, and this volume will not remember it\n");
}

/* -------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

void UseContextInit(void)
{
    memset(&g_use, 0, sizeof(g_use));
    spinlock_init(&g_use.lock);
    __atomic_store_n(&g_use_shape, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_use_waiting, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_use_owed, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_use_epoch, 1, __ATOMIC_RELEASE);
}

void UseContextShutdown(void)
{
    UseContextClear(NULL);
    spin_lock(&g_use.lock);
    UseSnapshot *s = g_use.snapshot;
    g_use.snapshot = NULL;
    spin_unlock(&g_use.lock);
    UseSnapshotDrop(s);
}
