#ifndef VOLUME_SPINE_H
#define VOLUME_SPINE_H

/*
 * The Spine's slots judged, and the slot a book is read from chosen — the
 * decisions, not the reads: every sector of a slot and every node
 * a root names is read by the caller off its own medium, and handed here as
 * bytes.
 *
 * A slot holds when its eight sectors are one SLOT unit (volume_book.h,
 * BookUnitCheck) and its stream has the shape every writer gives it:
 * SpineHead first, a root of each tree at most once, SpineRibbon and
 * SpineCounts once each, SpineFlags any number of times — the last one
 * standing. The shape is judged here record by record (SpineSlotStep), each
 * step answering with a reason, never a sentence: the kernel says the reason
 * in its own words (mount/spine_read.c, SpineSaid), the UEFI loader is to
 * pass a slot that does not hold over, and volume-read keeps the same rule by
 * its own code (tools/volumeread/tally.py SlotParse).
 *
 * Of the slots that hold, the newest is read first — the largest seq, a tie by
 * the lower index (SpineSlotNewer) — and the book stands on the first whose
 * root of every authoritative tree (SpineTreeAuthoritative) reads in some copy
 * (§2.5); one that does not is said and the next read. Whether a root reads is
 * the caller's: it is a read of the medium.
 *
 * ‼ ONE RULE FOR EVERY READER THAT CHOOSES A SLOT (§16.7 п.2). The kernel
 * mounts by it, and the UEFI loader is to pick the kernel it jumps to by it
 * (its Lectern, §16.7 п.6); a loader that stood on another slot than the
 * kernel's mount would hold the kernel's blocks to digests of another Tally.
 * So the shape, the order and the trees that must read are spelled once,
 * here, and both take them from here — the loader compiles this text already
 * (tagboot_lectern.h, which tagboot_lectern*.c are built on).
 *
 * No library, no state, no memory but the caller's: the rule of volume_book.h.
 * Integer types come from the includer, as volume_deed.h says.
 */

#include "volume_tally.h"

/* ------------------------------------------------------------------------- */
/* A slot's shape, a record at a time                                        */
/* ------------------------------------------------------------------------- */

/* What one step of a slot's walk answers (SpineSlotStep). */
typedef enum {
    SPINE_SAYS_RECORD,       /* a record read as this reader knows it, or stepped over as its tag lets it be */
    SPINE_SAYS_UNKNOWN,      /* a record of a kind this reader does not know, its tag not letting it be
                              * stepped over: the volume may only be read (§2.3, M-8) */
    SPINE_SAYS_LONGER,       /* a known record read, its body `longer` bytes past what this reader knows of
                              * it: the same */
    SPINE_SAYS_WHOLE,        /* the stream ended and every record a slot must carry was met: it holds */
    SPINE_SAYS_NOT_HEAD,     /* torn: its first record is of `kind`, not SpineHead */
    SPINE_SAYS_SECOND,       /* torn: a second SpineHead, SpineRibbon or SpineCounts, of `kind` */
    SPINE_SAYS_BROKEN,       /* torn: its record of `kind` does not read */
    SPINE_SAYS_TREE_TWICE,   /* torn: a second root of the tree root.tree */
    SPINE_SAYS_RUNS_PAST,    /* torn: the record at byte `at` runs past the slot */
    SPINE_SAYS_NO_RIBBON,    /* torn: the stream ended with no SpineRibbon */
    SPINE_SAYS_NO_COUNTS,    /* torn: the stream ended with no SpineCounts */
} SpineSays;

/* Whether an answer says the slot is torn: the walk ends there, and the slot
 * is not read. */
static inline int SpineSaysTorn(SpineSays says)
{
    return says >= SPINE_SAYS_NOT_HEAD;
}

/* A walk along a slot's stream: zeroed before its first step, read after its
 * last. What the slot's records say lands in it as each is read. */
typedef struct {
    uint32_t    at;          /* where the next record begins in the stream */
    uint32_t    nth;         /* records walked */
    uint32_t    seen;        /* a bit for each known kind met, 1 << kind */
    uint32_t    longer;      /* SPINE_SAYS_LONGER: the bytes past what this reader knows */
    uint64_t    kind;        /* the kind of the record the last step was about */
    SpineRoot   root;        /* the root the last step read, when `kind` is SPINE_ROOT */
    SpineHead   head;
    SpineRibbon ribbon;
    SpineCounts counts;
    uint64_t    flags;       /* SpineFlags, the last one standing; 0 for none */
} SpineWalk;

/* A known record's body into the walk: a TALLY_BODY_* answer. */
static inline int SpineRecordGet(const uint8_t *b, uint32_t len, SpineWalk *w)
{
    switch (w->kind) {
    case SPINE_HEAD:   return SpineHeadGet(b, len, &w->head);
    case SPINE_ROOT:   return SpineRootGet(b, len, &w->root);
    case SPINE_RIBBON: return SpineRibbonGet(b, len, &w->ribbon);
    case SPINE_COUNTS: return SpineCountsGet(b, len, &w->counts);
    default:           return SpineFlagsGet(b, len, &w->flags);
    }
}

/* The bytes of a known record's body this reader knows, as it was just read
 * into the walk — what is past them was written by a kernel that knows more. */
static inline uint32_t SpineKnownBytes(const SpineWalk *w, const uint8_t *b, uint32_t len)
{
    const SpineRoot *r = &w->root;
    BookPtr          ptr;
    uint32_t         n;

    switch (w->kind) {
    case SPINE_HEAD:
        return TallyVarintBytes(w->head.tally_seq) + TallyVarintBytes(w->head.stitch_seq) + 4u +
               TallyVarintBytes(w->head.made_ns);
    case SPINE_ROOT:
        n = TallyVarintBytes(r->tree) + TallyVarintBytes(r->flags) + TallyVarintBytes(r->level) +
            TallyVarintBytes(r->items);
        return n + BookPtrGet(b + n, len - n, &ptr);
    case SPINE_RIBBON:
        return TallyVarintBytes(w->ribbon.at) + TallyVarintBytes(w->ribbon.quire_block) +
               TallyVarintBytes(w->ribbon.quire_blocks) + TallyVarintBytes(w->ribbon.next_block) +
               TallyVarintBytes(w->ribbon.next_blocks);
    case SPINE_COUNTS:
        return TallyVarintBytes(w->counts.next_fid) + TallyVarintBytes(w->counts.next_tag) +
               TallyVarintBytes(w->counts.next_bookmark);
    default:
        return TallyVarintBytes(w->flags);
    }
}

/* Whether a record before byte `end` of the stream — every one of them walked
 * already, and read — is a root of `tree`. The stream is walked again rather
 * than the roots kept: a reader with no heap judges a slot of any number of
 * roots in the room of one. */
static inline int SpineTreeNamedBefore(const uint8_t *stream, uint32_t end, uint64_t tree)
{
    uint32_t at = 0, body, len, b;
    uint64_t tag, named;

    while (BookEntryNext(stream, end, &at, &tag, &body, &len) == 1) {
        b = 0;
        if (tag >> 1 == SPINE_ROOT && TallyTake(stream + body, len, &b, &named) && named == tree) return 1;
    }
    return 0;
}

/*
 * One step of the walk along a slot's stream of `room` bytes: the record at
 * w->at judged and, when it is known, read into *w, and w->at moved past it.
 * RECORD, UNKNOWN and LONGER go on to the next step — a root read (w->kind
 * SPINE_ROOT) is in w->root for the caller to keep; WHOLE and every torn
 * answer (SpineSaysTorn) end the walk. The rules are held in the order the
 * kernel has always held them, so a slot that breaks two is torn for the
 * first: the first record SpineHead; a kind this reader does not know stepped
 * over only when its tag says it may be; a second record of a kind there is
 * one of; the body read; a root of a tree named before; a body longer than
 * this reader knows.
 */
static inline SpineSays SpineSlotStep(const uint8_t *stream, uint32_t room, SpineWalk *w)
{
    uint32_t from = w->at, body, len;
    uint64_t tag;
    int      step = BookEntryNext(stream, room, &w->at, &tag, &body, &len);

    if (step < 0) return SPINE_SAYS_RUNS_PAST;
    if (step == 0) {
        if (!(w->seen & (1u << SPINE_RIBBON))) return SPINE_SAYS_NO_RIBBON;
        if (!(w->seen & (1u << SPINE_COUNTS))) return SPINE_SAYS_NO_COUNTS;
        return SPINE_SAYS_WHOLE;
    }
    w->kind = tag >> 1;
    if (w->nth++ == 0 && w->kind != SPINE_HEAD) return SPINE_SAYS_NOT_HEAD;
    if (w->kind < SPINE_HEAD || w->kind > SPINE_FLAGS) return tag & 1u ? SPINE_SAYS_RECORD : SPINE_SAYS_UNKNOWN;
    if (w->kind != SPINE_FLAGS && w->kind != SPINE_ROOT && (w->seen & (1u << w->kind))) return SPINE_SAYS_SECOND;
    w->seen |= 1u << w->kind;
    int read = SpineRecordGet(stream + body, len, w);
    if (read == TALLY_BODY_BROKEN) return SPINE_SAYS_BROKEN;
    if (w->kind == SPINE_ROOT && SpineTreeNamedBefore(stream, from, w->root.tree)) return SPINE_SAYS_TREE_TWICE;
    if (read == TALLY_BODY_LONGER) {
        w->longer = len - SpineKnownBytes(w, stream + body, len);
        return SPINE_SAYS_LONGER;
    }
    return SPINE_SAYS_RECORD;
}

/* ------------------------------------------------------------------------- */
/* The slot stood on                                                         */
/* ------------------------------------------------------------------------- */

/* Whether a slot that holds, of `seq` at ring index `index`, is read before
 * one of `than_seq` at `than_index`: the newer first — seq is the book's one
 * counter (§2.2), so the newest slot is the one of the largest seq — and of two
 * of one seq, the one of the lower index. */
static inline int SpineSlotNewer(uint64_t seq, uint32_t index, uint64_t than_seq, uint32_t than_index)
{
    return seq > than_seq || (seq == than_seq && index < than_index);
}

/* Whether a slot's root names a tree the book is read from as it is written,
 * whose root must read for the slot to be stood on — not one a reader counts
 * again from the rest: MAP, SHARED, a tree this reader does not know, or a
 * root that says it is DERIVED. */
static inline int SpineTreeAuthoritative(const SpineRoot *r)
{
    return r->tree >= TALLY_FILES && r->tree <= TALLY_DIGESTS && !(r->flags & TALLY_ROOT_DERIVED);
}

#endif /* VOLUME_SPINE_H */
