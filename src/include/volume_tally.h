#ifndef VOLUME_TALLY_H
#define VOLUME_TALLY_H

/*
 * The Tally's nodes and the Spine's slots — how each is written and read.
 *
 * A format-v2 volume keeps what it knows of its files in a book (volume_book.h
 * says how every sector of it is lettered). Two parts of the book are written
 * here: the Tally, a set of trees of nodes that sums up everything the ribbon
 * of stitches has said (spec_v2 §2.9), and the slots of the Spine ring, each
 * naming one Tally and where the ribbon goes on from it (§1.4). A node and a
 * slot are each a stream of records across the sectors of one unit — a SHEET
 * of 8 * sheets sectors, a SLOT of 8 — and nothing in them has a width but
 * u64's: every number is a varint.
 *
 * ‼ ONE CODEC FOR EVERY WRITER AND READER OF THEM. mkfs writes the first Tally
 * of every format-v2 volume with this (tools/create_tagfs_bind.c), and
 * volume-read on the host says the same again by its own code
 * (tools/volumeread/tally.py). The kernel of this tree reads no book yet — it
 * turns a format-v2 volume away (deed.c, DeedFormatRefused) — but its book
 * suite (book_tests.c) holds this codec to the sums below on every boot that
 * mounts a volume, and no second codec of these records is to be written
 * beside it. So the golden node and the golden slot are sums all three must
 * reach, and self-test X3 holds tally.py to every number here. One value, one
 * spelling (§2.4): two writers of the same Tally write the same bytes, which
 * is what lets the host bind a volume's Tally again where it lies and compare
 * it byte for byte (binder.Circle).
 *
 * A writer is handed exact room by its caller: every *Put answers the bytes
 * it wrote. A reader is handed a body and its length: every *Get of a body
 * answers TALLY_BODY_KNOWN when it read exactly the fields it knows,
 * TALLY_BODY_LONGER when bytes are left past them — the body was written by a
 * kernel that knows more, and the volume may only be read (§2.3, M-8), which
 * is the caller's to say aloud — and TALLY_BODY_BROKEN when a field is cut
 * short, a number is written in a way the book never writes one, or a rule of
 * the format refuses it.
 *
 * No library, no x86 instruction, no state: the same rule as volume_book.h,
 * for the kernel (-mno-sse) and the host's mkfs alike. Integer types come
 * from the includer, as volume_deed.h says.
 */

#include "volume_book.h"

/* ------------------------------------------------------------------------- */
/* The trees, the records and their bits                                     */
/* ------------------------------------------------------------------------- */

/* The trees of a Tally (§2.9), each the `tree` of its nodes and SpineRoot. */
#define TALLY_FILES      1u   /* (fid, part): a file's header, part 0, and its Parcels, part 1 + b */
#define TALLY_TAGS       2u   /* tag id: its flags and text                                          */
#define TALLY_USE        3u   /* 0: the volume's Use Context                                         */
#define TALLY_BOOKMARKS  4u
#define TALLY_LIEN       5u   /* (holder id, start)                                                  */
#define TALLY_DIGESTS    6u   /* first block: the digests of blocks in a row                         */
#define TALLY_MAP        7u   /* derived                                                             */
#define TALLY_SHARED     8u   /* derived                                                             */

#define TALLY_ROOT_DERIVED  1u   /* SpineRoot.flags bit 0: a reader may count it again from the rest */

/* The records of a slot's stream (§1.4), and the one flag SpineFlags has. */
#define SPINE_HEAD    1u
#define SPINE_ROOT    2u
#define SPINE_RIBBON  3u
#define SPINE_COUNTS  4u
#define SPINE_FLAGS   5u

#define SPINE_MEDIUM_LIED  1u

/* The first record of every node's stream (§2.9). */
#define NODE_HEAD  1u

/* A run's head (§2.8): count << 2, then these. */
#define RUN_HOLE  1u
#define RUN_EXT   2u

/* The bits of a FILES header and of a tag that this codec itself reads by:
 * a header that is not ACTIVE does not read (§16.3 п.7), and a RETIRED tag
 * carries no text (§6.4). The rest of their bits are carried as they are. */
#define TALLY_FILE_ACTIVE    1u
#define TALLY_FILE_UNDONE    16u     /* rolled back by a mount's verdict (§2.8) — the system's word, never a tag */
#define TALLY_FILE_DELETED   128u    /* deleted, kept until a time its header names (TallyFileHead.keep_ns) — never a tag */
#define TALLY_FILE_ROLE_MASK    96u  /* bits 5-6: what the system keeps the file as — never a tag (tagfs_constants.h) */
#define TALLY_FILE_ROLE_SYSTEM  32u
#define TALLY_FILE_ROLE_BOOT    64u
#define TALLY_FILE_ROLE_KERNEL  96u  /* the loader's first choice; then boot, then system */
#define TALLY_TAG_HAS_VALUE  1u
#define TALLY_TAG_RETIRED    2u

/* A Sheet's room for a node's stream: its eight sectors' payloads. */
#define TALLY_SHEET_PAYLOAD  3904u
_Static_assert(TALLY_SHEET_PAYLOAD == VOLUME_BLOCK_SECTORS * BOOK_PAYLOAD_BYTES,
               "a Sheet's room is eight payloads");

/* What a body read answers. */
#define TALLY_BODY_BROKEN  0
#define TALLY_BODY_KNOWN   1
#define TALLY_BODY_LONGER  2

/* ------------------------------------------------------------------------- */
/* The two rules every writer of a Tally keeps alike (§16.1 п.7, §16.2 п.24)  */
/* ------------------------------------------------------------------------- */

/*
 * A file's runs are cut into Parcels of at most PARCEL_STRIPS_MOST, as evenly
 * as they go (file_map/tract.c CutPieces, CutLength) — by the kernel, mkfs and
 * volume-read's binder alike, so the same file is the same Parcels whoever
 * wrote it. Why 128: a DATA run in the Tally's form takes at most 5 + 10 + 10
 * bytes (head, zigzag start, birth_delta), and a run SHARED 3 more (its list
 * of extensions, §8.2), so ~139 fit a Sheet's room whatever the numbers grow
 * to (§2.9): a Parcel of memory is one Parcel of the medium, and no writer
 * ever has to cut one at the Sheet.
 */
#define PARCEL_STRIPS_MOST  128u

/*
 * A DIGESTS element carries the digests of blocks in a row inside one window
 * of DIGEST_ITEM_MOST blocks (block / DIGEST_ITEM_MOST), so a change to one
 * block rewrites one element wherever it lies. 485 is the most for which a
 * leaf's NodeHead of one element and the widest key (15 bytes) and the element
 * (length 2, key 1 — the first element's key is its distance from the
 * NodeHead's, 0 — count 2, 8 bytes each) share one Sheet: 15 + 5 + 8 * 485 =
 * 3900 <= 3904, and 486 is 3908 (§16.3 п.1; the book suite proves it).
 */
#define DIGEST_ITEM_MOST  485u

/* ------------------------------------------------------------------------- */
/* The golden node and the golden slot                                       */
/* ------------------------------------------------------------------------- */

/*
 * Each a unit defined by a rule, whose eight sectors — sealed for the volume
 * 00 01 .. 0F — sum by CRC32C (BookCrc32cOn over the 4096 bytes, inverted) to
 * the number below. The kernel's book suite builds them with this codec;
 * volume-read's self-test with tally.py's own encoders. A rule and a number,
 * not 4096 bytes spelled twice.
 *
 * The golden node: a FILES leaf of seq 1 at block 0x2000, one Sheet, five
 * items, each key a pair (fid, part):
 *   (1, 0)  header: gen 1, flags ACTIVE, size 5000, created_ns
 *           1700000000000000000, modified 7 ns later, name "golden", tags 3, 5, 300
 *   (1, 1)  Parcel: DATA 1024 for 2 blocks, born at 1
 *   (2, 0)  header: gen 9, flags ACTIVE | HIDDEN (5), size 0, created_ns 1,
 *           modified_ns 0, name "", no tags
 *   (3, 0)  header: gen 1, flags ACTIVE, size 9000, created_ns 2, modified_ns 2,
 *           name "three", tag 0
 *   (3, 1)  Parcel: a HOLE of 1 block; DATA 4096 for 1, born at 1; DATA 10
 *           for 1, born at 0, with the extension bytes 02 01 07
 *
 * The golden slot: slot 0 (block 256) at seq 2 — SpineHead {tally 5, stitch 9,
 * clasp 0xC0FFEE11, made_ns 1700000000123456789}; SpineRoot FILES {flags 0,
 * level 1, items 70, BookPtr seq 5, 1 Sheet, blocks 2048 and 2304}; SpineRoot
 * TAGS {0, level 0, items 40, seq 5, 1 Sheet, 2049 and 2305}; SpineRoot DIGESTS
 * {0, level 1, items 15000, seq 3, 2 Sheets, 1500 and 1756}; SpineRibbon {at
 * 2600, Quire 2560 for 256, next 2816 for 256}; SpineCounts {next_fid 71,
 * next_tag 52, next_bookmark 1}; SpineFlags MEDIUM_LIED.
 */
#define TALLY_GOLDEN_NODE_CRC  0x2DB73240u
#define TALLY_GOLDEN_SLOT_CRC  0x364D3494u

/* ------------------------------------------------------------------------- */
/* Numbers within a body                                                     */
/* ------------------------------------------------------------------------- */

/* The bytes BookVarintPut writes `v` in. */
static inline uint32_t TallyVarintBytes(uint64_t v)
{
    uint32_t n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        n++;
    }
    return n;
}

/* One number of a body of `end` bytes, from *at on: 1 with *at past it, or 0
 * refused (cut short, or written as the book never writes one). */
static inline int TallyTake(const uint8_t *p, uint32_t end, uint32_t *at, uint64_t *v)
{
    if (*at >= end) return 0;
    uint32_t n = BookVarintGet(p + *at, end - *at, v);
    *at += n;
    return n != 0;
}

/* `n` bytes of a body, from *at on, pointed at where they stand. */
static inline int TallyTakeBytes(const uint8_t *p, uint32_t end, uint32_t *at, uint64_t n,
                                 const uint8_t **bytes)
{
    if (*at > end || n > end - *at) return 0;
    *bytes = p + *at;
    *at += (uint32_t)n;
    return 1;
}

/* A little-endian number of `n` bytes, the fixed-width fields of the book. */
static inline uint64_t TallyLe(const uint8_t *p, uint32_t n)
{
    uint64_t v = 0;
    for (uint32_t i = n; i-- > 0; ) v = v << 8 | p[i];
    return v;
}

static inline uint32_t TallyLePut(uint8_t *out, uint64_t v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)(v >> (8u * i));
    return n;
}

static inline uint32_t TallyBytesPut(uint8_t *out, const uint8_t *from, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) out[i] = from[i];
    return (uint32_t)n;
}

/* What a body read to `at` of its `len` answers: all of it known, or more. */
static inline int TallyBodyEnd(uint32_t at, uint32_t len)
{
    return at == len ? TALLY_BODY_KNOWN : TALLY_BODY_LONGER;
}

/* A record's tag — `kind`, not ignorable — and its body's length, into `out`;
 * the bytes they took. Its body follows them. */
static inline uint32_t TallyRecordOpen(uint8_t *out, uint64_t kind, uint64_t body_len)
{
    uint32_t at = BookVarintPut(out, kind << 1);
    return at + BookVarintPut(out + at, body_len);
}

/* ------------------------------------------------------------------------- */
/* Keys and items (§2.9)                                                     */
/* ------------------------------------------------------------------------- */

/* A key of a Tally: (k1, k2) for FILES and LIEN, k1 alone — k2 0 and never
 * written — for every other tree. */
typedef struct {
    uint64_t k1;
    uint64_t k2;
} TallyKey;

static inline int TallyPairKeyed(uint64_t tree)
{
    return tree == TALLY_FILES || tree == TALLY_LIEN;
}

/*
 * A key as an item writes it, from the key before it (§2.9): for a simple key
 * its distance; for a pair d1 = k1 - k1_prev, and when d1 is 0 the distance of
 * k2, else k2 whole. The first item of a node is written from the NodeHead's
 * first_key — itself — so as 0 (§16.3 п.2). `key` is never below `prev`.
 */
static inline uint32_t TallyKeyPut(uint8_t *out, uint64_t tree, TallyKey key, TallyKey prev)
{
    if (!TallyPairKeyed(tree)) return BookVarintPut(out, key.k1 - prev.k1);
    uint32_t at = BookVarintPut(out, key.k1 - prev.k1);
    return at + BookVarintPut(out + at, key.k1 == prev.k1 ? key.k2 - prev.k2 : key.k2);
}

static inline uint32_t TallyKeyBytes(uint64_t tree, TallyKey key, TallyKey prev)
{
    if (!TallyPairKeyed(tree)) return TallyVarintBytes(key.k1 - prev.k1);
    return TallyVarintBytes(key.k1 - prev.k1) +
           TallyVarintBytes(key.k1 == prev.k1 ? key.k2 - prev.k2 : key.k2);
}

/* TallyKeyPut read back: 0 refused, or for a key past 2^64 - 1. Whether keys
 * rise is the node's rule, the caller's to hold (each after the one before
 * it; the first not below first_key). */
static inline int TallyKeyGet(const uint8_t *p, uint32_t end, uint32_t *at, uint64_t tree, TallyKey prev,
                              TallyKey *key)
{
    uint64_t d1, d2;
    if (!TallyTake(p, end, at, &d1) || d1 > ~prev.k1) return 0;
    key->k1 = prev.k1 + d1;
    key->k2 = 0;
    if (!TallyPairKeyed(tree)) return 1;
    if (!TallyTake(p, end, at, &d2)) return 0;
    if (d1 != 0) {
        key->k2 = d2;
        return 1;
    }
    if (d2 > ~prev.k2) return 0;
    key->k2 = prev.k2 + d2;
    return 1;
}

/* The bytes an item of a `body_len`-byte body takes: its length, its key from
 * `prev`, its body — what a writer packs a node by. */
static inline uint32_t TallyItemBytes(uint64_t tree, TallyKey key, TallyKey prev, uint32_t body_len)
{
    uint32_t inner = TallyKeyBytes(tree, key, prev) + body_len;
    return TallyVarintBytes(inner) + inner;
}

/* An item: `len varint · key · body` (len — the key's bytes and the body's). */
static inline uint32_t TallyItemPut(uint8_t *out, uint64_t tree, TallyKey key, TallyKey prev,
                                    const uint8_t *body, uint32_t body_len)
{
    uint32_t at = BookVarintPut(out, TallyKeyBytes(tree, key, prev) + (uint64_t)body_len);
    at += TallyKeyPut(out + at, tree, key, prev);
    return at + TallyBytesPut(out + at, body, body_len);
}

/*
 * The item at *at of a node's stream of `room` bytes: its key from `prev`,
 * and its body at *body for *len bytes; *at moves past it. 0 — and *at left
 * where it was — when its length runs past the stream or its key does not
 * read. The length is held before anything inside it is looked at.
 */
static inline int TallyItemGet(const uint8_t *s, uint32_t room, uint32_t *at, uint64_t tree, TallyKey prev,
                               TallyKey *key, uint32_t *body, uint32_t *len)
{
    uint32_t p = *at;
    uint64_t size;
    if (!TallyTake(s, room, &p, &size) || size > room - p) return 0;
    uint32_t end = p + (uint32_t)size;
    if (!TallyKeyGet(s, end, &p, tree, prev, key)) return 0;
    *body = p;
    *len  = end - p;
    *at   = end;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* NodeHead (§2.9): tree · level · items · first_key                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t tree;
    uint64_t level;      /* 0 for a leaf */
    uint64_t items;
    TallyKey first;      /* the key of its first item, whole */
} NodeHead;

static inline uint32_t NodeHeadBodyBytes(const NodeHead *h)
{
    uint32_t n = TallyVarintBytes(h->tree) + TallyVarintBytes(h->level) + TallyVarintBytes(h->items) +
                 TallyVarintBytes(h->first.k1);
    return TallyPairKeyed(h->tree) ? n + TallyVarintBytes(h->first.k2) : n;
}

/* The bytes the NodeHead record takes — its tag, length and body. */
static inline uint32_t NodeHeadBytes(const NodeHead *h)
{
    uint32_t body = NodeHeadBodyBytes(h);
    return TallyVarintBytes((uint64_t)NODE_HEAD << 1) + TallyVarintBytes(body) + body;
}

static inline uint32_t NodeHeadPut(uint8_t *out, const NodeHead *h)
{
    uint32_t at = TallyRecordOpen(out, NODE_HEAD, NodeHeadBodyBytes(h));
    at += BookVarintPut(out + at, h->tree);
    at += BookVarintPut(out + at, h->level);
    at += BookVarintPut(out + at, h->items);
    at += BookVarintPut(out + at, h->first.k1);
    if (TallyPairKeyed(h->tree)) at += BookVarintPut(out + at, h->first.k2);
    return at;
}

/* The NodeHead that opens a node's stream of `room` bytes, from *at: a
 * TALLY_BODY_* answer, and *at past it when it read. A first record of any
 * other kind is BROKEN: the stream is no node. */
static inline int NodeHeadGet(const uint8_t *s, uint32_t room, uint32_t *at, NodeHead *h)
{
    uint64_t tag;
    uint32_t p = *at, body, len, b = 0;
    if (BookEntryNext(s, room, &p, &tag, &body, &len) != 1 || tag >> 1 != NODE_HEAD) return TALLY_BODY_BROKEN;
    const uint8_t *q = s + body;
    h->first.k2 = 0;
    if (!TallyTake(q, len, &b, &h->tree) || !TallyTake(q, len, &b, &h->level) ||
        !TallyTake(q, len, &b, &h->items) || !TallyTake(q, len, &b, &h->first.k1) ||
        (TallyPairKeyed(h->tree) && !TallyTake(q, len, &b, &h->first.k2)))
        return TALLY_BODY_BROKEN;
    *at = p;
    return TallyBodyEnd(b, len);
}

/* ------------------------------------------------------------------------- */
/* A node's stream judged whole (§2.9, §16.3 п.2)                            */
/* ------------------------------------------------------------------------- */

/* Keys [lo, hi) of a tree — no lo from its start, no hi to its end: the range
 * an inner node's separators give a child. */
typedef struct {
    TallyKey lo, hi;
    int      has_lo, has_hi;
} TallyRange;

static inline int TallyKeyBefore(TallyKey a, TallyKey b)
{
    return a.k1 < b.k1 || (a.k1 == b.k1 && a.k2 < b.k2);
}

static inline int TallyRangeHolds(const TallyRange *r, TallyKey key)
{
    return !(r->has_lo && TallyKeyBefore(key, r->lo)) && !(r->has_hi && !TallyKeyBefore(key, r->hi));
}

/* What TallyNodeJudge finds, the first rule broken in this order. */
#define TALLY_NODE_HOLDS      0
#define TALLY_NODE_HEADLESS   1   /* it opens with no NodeHead                         */
#define TALLY_NODE_COUNTED    2   /* it counts more items than it has bytes            */
#define TALLY_NODE_RUNS_PAST  3   /* item `nth` runs past the node                     */
#define TALLY_NODE_FALLS      4   /* the keys do not rise at item `nth`                */
#define TALLY_NODE_TRAILED    5   /* a byte other than PAD follows its last item       */
#define TALLY_NODE_OUTSIDE    6   /* item `nth`'s key, `key`, lies outside the range   */

typedef struct {
    NodeHead head;
    uint32_t items_at;   /* where its first item begins */
    uint64_t nth;
    TallyKey key;
} TallyNodeJudged;

/*
 * ‼ ONE RULE FOR WHETHER A COPY OF A NODE HOLDS (§16.7 п.2). A copy's stream
 * of `room` bytes holds as a node of the range `r` its parent gives it when it
 * opens with a NodeHead counting no more items than it has bytes; every item
 * is whole in the stream, each key above the one before it and none below the
 * first key; nothing but PAD follows the last; and every key lies inside the
 * range. The kernel's mount reads a node by it (mount/tally_walk.c,
 * TallyNodeParse) and so does the UEFI loader's Lectern (tagboot_lectern.c):
 * a loader that held a node another kernel refuses would stand on another
 * slot than the kernel's mount, and hold the kernel's blocks to the digests of
 * another Tally. volume-read keeps the same rule by its own code (tally.py
 * NodeRead, NodeParse). The rules of the stream are held over every item
 * before any key is held to the range, the order the reasons are said in.
 */
static inline int TallyNodeJudge(const uint8_t *s, uint32_t room, const TallyRange *r, TallyNodeJudged *j)
{
    uint32_t at = 0, body, len;
    TallyKey prev, key;

    if (NodeHeadGet(s, room, &at, &j->head) == TALLY_BODY_BROKEN) return TALLY_NODE_HEADLESS;
    j->items_at = at;
    if (j->head.items > room) return TALLY_NODE_COUNTED;
    prev = j->head.first;
    for (j->nth = 0; j->nth < j->head.items; j->nth++) {
        if (!TallyItemGet(s, room, &at, j->head.tree, prev, &key, &body, &len)) return TALLY_NODE_RUNS_PAST;
        if ((j->nth && !TallyKeyBefore(prev, key)) || TallyKeyBefore(key, j->head.first)) return TALLY_NODE_FALLS;
        prev = key;
    }
    if (at < room && s[at] != 0) return TALLY_NODE_TRAILED;
    at   = j->items_at;
    prev = j->head.first;
    for (j->nth = 0; j->nth < j->head.items; j->nth++) {
        TallyItemGet(s, room, &at, j->head.tree, prev, &key, &body, &len);
        prev = key;
        if (!TallyRangeHolds(r, key)) {
            j->key = key;
            return TALLY_NODE_OUTSIDE;
        }
    }
    return TALLY_NODE_HOLDS;
}

/* ------------------------------------------------------------------------- */
/* FILES: a file's header (part 0) and its Parcels (part 1 + b)              */
/* ------------------------------------------------------------------------- */

/* A rising list of ids after its count, as the book writes every one (§2.8
 * Born's tags, Labeled, Undone; a header's tags): the first whole, each next
 * as its distance from the last less one. The count is the caller's. */
static inline uint32_t TallyIdsPut(uint8_t *out, const uint32_t *ids, uint32_t n)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i < n; i++)
        at += BookVarintPut(out + at, i ? (uint64_t)ids[i] - ids[i - 1] - 1u : ids[0]);
    return at;
}

/* The next id of such a list, from *at, into *id — the id before it, unless
 * `first`. 0 when it does not read or would pass 2^64 - 1. */
static inline int TallyIdNext(const uint8_t *p, uint32_t end, uint32_t *at, uint64_t *id, int first)
{
    uint64_t v;
    if (!TallyTake(p, end, at, &v)) return 0;
    if (first) {
        *id = v;
        return 1;
    }
    if (*id == ~(uint64_t)0 || v > ~(uint64_t)0 - 1u - *id) return 0;
    *id += 1u + v;
    return 1;
}

/*
 * A file's header: gen · flags · size · created_ns · mtime_z · name_len ·
 * name · ntags · tags · [keep_ns]. mtime_z is zigzag(modified_ns -
 * created_ns): a clock may run backwards. The name and the tags are pointed
 * at where they stand; a writer points them at what it writes from — `tags`
 * the ids after their count, as TallyIdsPut writes them, `tags_bytes` long.
 *
 * keep_ns is there when, and only when, the flags carry TALLY_FILE_DELETED:
 * the time, in nanoseconds of the clock created_ns is of, until which the
 * file is kept deleted and can be brought back — never 0. A reader that does
 * not know the bit finds bytes left past the tags and takes the header for a
 * newer writer's (TALLY_BODY_LONGER), which stands the volume under glass.
 */
typedef struct {
    uint64_t       gen;
    uint64_t       flags;
    uint64_t       size;
    uint64_t       created_ns;
    uint64_t       modified_ns;
    const uint8_t *name;
    uint64_t       name_len;
    uint64_t       ntags;
    const uint8_t *tags;
    uint32_t       tags_bytes;
    uint64_t       keep_ns;      /* 0 unless TALLY_FILE_DELETED */
} TallyFileHead;

static inline uint32_t TallyFileHeadPut(uint8_t *out, const TallyFileHead *h)
{
    uint32_t at = BookVarintPut(out, h->gen);
    at += BookVarintPut(out + at, h->flags);
    at += BookVarintPut(out + at, h->size);
    at += BookVarintPut(out + at, h->created_ns);
    at += BookVarintPut(out + at, BookZigzag((int64_t)(h->modified_ns - h->created_ns)));
    at += BookVarintPut(out + at, h->name_len);
    at += TallyBytesPut(out + at, h->name, h->name_len);
    at += BookVarintPut(out + at, h->ntags);
    at += TallyBytesPut(out + at, h->tags, h->tags_bytes);
    return (h->flags & TALLY_FILE_DELETED) ? at + BookVarintPut(out + at, h->keep_ns) : at;
}

/* A header read back: BROKEN as well for one that is not ACTIVE — such an
 * element does not read, and its file is locked (§16.3 п.7). */
static inline int TallyFileHeadGet(const uint8_t *b, uint32_t len, TallyFileHead *h)
{
    uint32_t at = 0;
    uint64_t z, id = 0;
    if (!TallyTake(b, len, &at, &h->gen) || !TallyTake(b, len, &at, &h->flags) ||
        !TallyTake(b, len, &at, &h->size) || !TallyTake(b, len, &at, &h->created_ns) ||
        !TallyTake(b, len, &at, &z) || !TallyTake(b, len, &at, &h->name_len) ||
        !TallyTakeBytes(b, len, &at, h->name_len, &h->name) || !TallyTake(b, len, &at, &h->ntags) ||
        h->ntags > len - at)
        return TALLY_BODY_BROKEN;
    h->modified_ns = h->created_ns + (uint64_t)BookUnzigzag(z);
    h->tags = b + at;
    uint32_t from = at;
    for (uint64_t k = 0; k < h->ntags; k++)
        if (!TallyIdNext(b, len, &at, &id, k == 0)) return TALLY_BODY_BROKEN;
    h->tags_bytes = at - from;
    h->keep_ns    = 0;
    if ((h->flags & TALLY_FILE_DELETED) && (!TallyTake(b, len, &at, &h->keep_ns) || !h->keep_ns))
        return TALLY_BODY_BROKEN;
    if (!(h->flags & TALLY_FILE_ACTIVE)) return TALLY_BODY_BROKEN;
    return TallyBodyEnd(at, len);
}

/* One run of a file (§2.8): a hole of `count` blocks, or `count` blocks of
 * the ground from `start`, born at `birth` (the seq of what put it in), and
 * the TLV list of its extensions as written — `ext` NULL when it has none. */
typedef struct {
    uint64_t       start;
    uint64_t       count;
    uint64_t       birth;
    uint32_t       hole;
    uint32_t       ext_len;
    const uint8_t *ext;
} TallyRun;

/*
 * A run in one of its two forms (§2.8): head = count << 2 | HOLE | EXT; a DATA
 * run's start as zigzag(start - *cursor) — *cursor the end of the DATA run
 * before it in the same Parcel or record, 0 at its first — then, in the
 * Tally's form (`tally` set), birth_delta = seq - birth, counted from the
 * node's seq and not the file's gen: a later Tally names an unchanged node as
 * it lies (§2.9); then the extensions. The ribbon's form carries no birth: a
 * run is born at the seq of the stitch that put it in (volume_ribbon.h).
 */
static inline uint32_t BookRunPut(uint8_t *out, const TallyRun *r, uint64_t *cursor, uint64_t seq, int tally)
{
    uint32_t at = BookVarintPut(out, r->count << 2 | (r->hole ? RUN_HOLE : 0u) | (r->ext ? RUN_EXT : 0u));
    if (!r->hole) {
        at += BookVarintPut(out + at, BookZigzag((int64_t)(r->start - *cursor)));
        *cursor = r->start + r->count;
        if (tally)
            at += BookVarintPut(out + at, seq - r->birth);
    }
    if (r->ext) {
        at += BookVarintPut(out + at, r->ext_len);
        at += TallyBytesPut(out + at, r->ext, r->ext_len);
    }
    return at;
}

/* BookRunPut read back, from *at of a body of `end` bytes: 0 for a run of no
 * blocks, a start before block 0 or past 2^64 - 1, or a birth_delta past the
 * node's seq. A hole is born with its node or stitch, and so is every run of
 * the ribbon's form. */
static inline int BookRunGet(const uint8_t *b, uint32_t end, uint32_t *at, uint64_t *cursor, uint64_t seq,
                             int tally, TallyRun *r)
{
    uint64_t head, z, delta = 0, n;
    if (!TallyTake(b, end, at, &head) || (r->count = head >> 2) == 0) return 0;
    r->hole    = (uint32_t)(head & RUN_HOLE);
    r->start   = 0;
    r->birth   = seq;
    r->ext     = (const uint8_t *)0;
    r->ext_len = 0;
    if (!r->hole) {
        if (!TallyTake(b, end, at, &z)) return 0;
        uint64_t far = z & 1u ? (z >> 1) + 1u : z >> 1;    /* |start - cursor| */
        if (z & 1u ? far > *cursor : far > ~*cursor) return 0;
        r->start = z & 1u ? *cursor - far : *cursor + far;
        if (r->count > ~r->start) return 0;
        if (tally && (!TallyTake(b, end, at, &delta) || delta > seq)) return 0;
        *cursor  = r->start + r->count;
        r->birth = seq - delta;
    }
    if (head & RUN_EXT) {
        if (!TallyTake(b, end, at, &n) || !TallyTakeBytes(b, end, at, n, &r->ext)) return 0;
        r->ext_len = (uint32_t)n;
    }
    return 1;
}

/* A run in the Tally's form, in a node of `seq`. */
static inline uint32_t TallyRunPut(uint8_t *out, const TallyRun *r, uint64_t *cursor, uint64_t seq)
{
    return BookRunPut(out, r, cursor, seq, 1);
}

static inline int TallyRunGet(const uint8_t *b, uint32_t end, uint32_t *at, uint64_t *cursor, uint64_t seq,
                              TallyRun *r)
{
    return BookRunGet(b, end, at, cursor, seq, 1, r);
}

/* A Parcel's body: nruns, then its runs, the cursor from 0 — each Parcel
 * stands alone (§2.9). */
static inline uint32_t TallyParcelPut(uint8_t *out, const TallyRun *runs, uint32_t n, uint64_t seq)
{
    uint64_t cursor = 0;
    uint32_t at     = BookVarintPut(out, n);
    for (uint32_t i = 0; i < n; i++) at += TallyRunPut(out + at, &runs[i], &cursor, seq);
    return at;
}

/* ------------------------------------------------------------------------- */
/* TAGS, USE, DIGESTS, and an inner node's items                             */
/* ------------------------------------------------------------------------- */

/* A tag's text as TAGS and Coined carry it: flags, then — unless RETIRED,
 * which names no text — key_len · key · [value_len · value when HAS_VALUE]. */
typedef struct {
    uint64_t       flags;
    const uint8_t *key;
    uint64_t       key_len;
    const uint8_t *value;
    uint64_t       value_len;
} TallyTag;

static inline uint32_t TallyTagPut(uint8_t *out, const TallyTag *t)
{
    uint32_t at = BookVarintPut(out, t->flags);
    if (t->flags & TALLY_TAG_RETIRED) return at;
    at += BookVarintPut(out + at, t->key_len);
    at += TallyBytesPut(out + at, t->key, t->key_len);
    if (!(t->flags & TALLY_TAG_HAS_VALUE)) return at;
    at += BookVarintPut(out + at, t->value_len);
    return at + TallyBytesPut(out + at, t->value, t->value_len);
}

static inline int TallyTagGet(const uint8_t *b, uint32_t len, TallyTag *t)
{
    uint32_t at = 0;
    t->key = t->value = (const uint8_t *)0;
    t->key_len = t->value_len = 0;
    if (!TallyTake(b, len, &at, &t->flags)) return TALLY_BODY_BROKEN;
    if (!(t->flags & TALLY_TAG_RETIRED)) {
        if (!TallyTake(b, len, &at, &t->key_len) || !TallyTakeBytes(b, len, &at, t->key_len, &t->key))
            return TALLY_BODY_BROKEN;
        if ((t->flags & TALLY_TAG_HAS_VALUE) &&
            (!TallyTake(b, len, &at, &t->value_len) || !TallyTakeBytes(b, len, &at, t->value_len, &t->value)))
            return TALLY_BODY_BROKEN;
    }
    return TallyBodyEnd(at, len);
}

/* The Use Context (§7): len · text. */
static inline uint32_t TallyUsePut(uint8_t *out, const uint8_t *text, uint64_t len)
{
    uint32_t at = BookVarintPut(out, len);
    return at + TallyBytesPut(out + at, text, len);
}

static inline int TallyUseGet(const uint8_t *b, uint32_t len, const uint8_t **text, uint64_t *text_len)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, text_len) || !TallyTakeBytes(b, len, &at, *text_len, text))
        return TALLY_BODY_BROKEN;
    return TallyBodyEnd(at, len);
}

/* DIGESTS: n · n × u64 LE, the digests of blocks in a row from the key. */
static inline uint32_t TallyDigestsPut(uint8_t *out, const uint64_t *digests, uint32_t n)
{
    uint32_t at = BookVarintPut(out, n);
    for (uint32_t i = 0; i < n; i++) at += TallyLePut(out + at, digests[i], 8);
    return at;
}

/* The digests pointed at where they stand: TallyDigestAt reads the i-th. */
static inline int TallyDigestsGet(const uint8_t *b, uint32_t len, uint64_t *n, const uint8_t **digests)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, n) || *n > (len - at) / 8u) return TALLY_BODY_BROKEN;
    *digests = b + at;
    return TallyBodyEnd(at + (uint32_t)(*n * 8u), len);
}

static inline uint64_t TallyDigestAt(const uint8_t *digests, uint64_t i)
{
    return TallyLe(digests + i * 8u, 8);
}

/*
 * The element of a DIGESTS leaf's stream of `room` bytes that names `block`,
 * its digest in *digest: 1 found, 0 named by none, -1 the NodeHead or an
 * element on the way does not read. The elements are walked in key order and
 * the walk ends at the first past the block — so an element past it that does
 * not read is no answer about the block. That the stream is a DIGESTS leaf,
 * and the leaf of the block's range, is the reader's to hold first, as it
 * reads the node (§16.3 п.2): the kernel's reads of a block (tally_read.c,
 * TallyDigestFind), and the UEFI loader's of its kernel's blocks to come
 * (§16.7 п.6, Attest).
 */
static inline int TallyDigestIn(const uint8_t *s, uint32_t room, uint64_t block, uint64_t *digest)
{
    NodeHead       h;
    TallyKey       prev, key;
    uint32_t       at = 0, body, len;
    uint64_t       n;
    const uint8_t *d;

    if (NodeHeadGet(s, room, &at, &h) == TALLY_BODY_BROKEN) return -1;
    prev = h.first;
    for (uint64_t i = 0; i < h.items; i++) {
        if (!TallyItemGet(s, room, &at, TALLY_DIGESTS, prev, &key, &body, &len)) return -1;
        prev = key;
        if (key.k1 > block) return 0;
        if (TallyDigestsGet(s + body, len, &n, &d) == TALLY_BODY_BROKEN) return -1;
        if (block - key.k1 < n) {
            *digest = TallyDigestAt(d, block - key.k1);
            return 1;
        }
    }
    return 0;
}

/* An inner node's item (level >= 1): count · BookPtr of the child — count
 * the leaf elements under it (FILES: its headers, i.e. files), so a rotten
 * range is said with how many it lost (§2.9). The pointer as BookPtrPut
 * wrote it. */
static inline uint32_t TallyInnerPut(uint8_t *out, uint64_t count, const uint8_t *ptr, uint32_t ptr_len)
{
    uint32_t at = BookVarintPut(out, count);
    return at + TallyBytesPut(out + at, ptr, ptr_len);
}

static inline int TallyInnerGet(const uint8_t *b, uint32_t len, uint64_t *count, BookPtr *ptr)
{
    uint32_t at = 0, n;
    if (!TallyTake(b, len, &at, count) || !(n = BookPtrGet(b + at, len - at, ptr))) return TALLY_BODY_BROKEN;
    return TallyBodyEnd(at + n, len);
}

/* ------------------------------------------------------------------------- */
/* The slot's records (§1.4) — each *Put writes a whole record, tag and all;  */
/* each *Get reads a body BookEntryNext found                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t tally_seq;      /* the seq of the Tally this slot names                        */
    uint64_t stitch_seq;     /* the last stitch the Tally took in; 0 for none               */
    uint32_t stitch_clasp;   /* that stitch's Clasp.crc: what the next one's prev_clasp names */
    uint64_t made_ns;
} SpineHead;

static inline uint32_t SpineHeadPut(uint8_t *out, const SpineHead *h)
{
    uint32_t at = TallyRecordOpen(out, SPINE_HEAD, TallyVarintBytes(h->tally_seq) +
                                  TallyVarintBytes(h->stitch_seq) + 4u + TallyVarintBytes(h->made_ns));
    at += BookVarintPut(out + at, h->tally_seq);
    at += BookVarintPut(out + at, h->stitch_seq);
    at += TallyLePut(out + at, h->stitch_clasp, 4);
    return at + BookVarintPut(out + at, h->made_ns);
}

static inline int SpineHeadGet(const uint8_t *b, uint32_t len, SpineHead *h)
{
    uint32_t       at = 0;
    const uint8_t *clasp;
    if (!TallyTake(b, len, &at, &h->tally_seq) || !TallyTake(b, len, &at, &h->stitch_seq) ||
        !TallyTakeBytes(b, len, &at, 4, &clasp) || !TallyTake(b, len, &at, &h->made_ns))
        return TALLY_BODY_BROKEN;
    h->stitch_clasp = (uint32_t)TallyLe(clasp, 4);
    return TallyBodyEnd(at, len);
}

/* A tree's root: tree · flags · level · items · BookPtr of its root node.
 * items is the count the root carries (FILES: its files, §16.3 п.3). */
typedef struct {
    uint64_t tree;
    uint64_t flags;
    uint64_t level;
    uint64_t items;
    BookPtr  ptr;
} SpineRoot;

static inline uint32_t SpineRootPut(uint8_t *out, uint64_t tree, uint64_t flags, uint64_t level,
                                    uint64_t items, const uint8_t *ptr, uint32_t ptr_len)
{
    uint32_t at = TallyRecordOpen(out, SPINE_ROOT, TallyVarintBytes(tree) + TallyVarintBytes(flags) +
                                  TallyVarintBytes(level) + TallyVarintBytes(items) + ptr_len);
    at += BookVarintPut(out + at, tree);
    at += BookVarintPut(out + at, flags);
    at += BookVarintPut(out + at, level);
    at += BookVarintPut(out + at, items);
    return at + TallyBytesPut(out + at, ptr, ptr_len);
}

static inline int SpineRootGet(const uint8_t *b, uint32_t len, SpineRoot *r)
{
    uint32_t at = 0, n;
    if (!TallyTake(b, len, &at, &r->tree) || !TallyTake(b, len, &at, &r->flags) ||
        !TallyTake(b, len, &at, &r->level) || !TallyTake(b, len, &at, &r->items) ||
        !(n = BookPtrGet(b + at, len - at, &r->ptr)))
        return TALLY_BODY_BROKEN;
    return TallyBodyEnd(at + n, len);
}

/* Where the ribbon goes on from this slot's Tally: the block of the first
 * atom past the stitch it took in, the Quire that block lies in, and that
 * stitch's next_quire (0 · 0: none yet). */
typedef struct {
    uint64_t at;
    uint64_t quire_block;
    uint64_t quire_blocks;
    uint64_t next_block;
    uint64_t next_blocks;
} SpineRibbon;

static inline uint32_t SpineRibbonPut(uint8_t *out, const SpineRibbon *r)
{
    uint32_t at = TallyRecordOpen(out, SPINE_RIBBON, TallyVarintBytes(r->at) + TallyVarintBytes(r->quire_block) +
                                  TallyVarintBytes(r->quire_blocks) + TallyVarintBytes(r->next_block) +
                                  TallyVarintBytes(r->next_blocks));
    at += BookVarintPut(out + at, r->at);
    at += BookVarintPut(out + at, r->quire_block);
    at += BookVarintPut(out + at, r->quire_blocks);
    at += BookVarintPut(out + at, r->next_block);
    return at + BookVarintPut(out + at, r->next_blocks);
}

static inline int SpineRibbonGet(const uint8_t *b, uint32_t len, SpineRibbon *r)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, &r->at) || !TallyTake(b, len, &at, &r->quire_block) ||
        !TallyTake(b, len, &at, &r->quire_blocks) || !TallyTake(b, len, &at, &r->next_block) ||
        !TallyTake(b, len, &at, &r->next_blocks))
        return TALLY_BODY_BROKEN;
    return TallyBodyEnd(at, len);
}

/* The bounds of what the medium has given out: no fid, tag id or bookmark id
 * below them is given again. */
typedef struct {
    uint64_t next_fid;
    uint64_t next_tag;
    uint64_t next_bookmark;
} SpineCounts;

static inline uint32_t SpineCountsPut(uint8_t *out, const SpineCounts *c)
{
    uint32_t at = TallyRecordOpen(out, SPINE_COUNTS, TallyVarintBytes(c->next_fid) +
                                  TallyVarintBytes(c->next_tag) + TallyVarintBytes(c->next_bookmark));
    at += BookVarintPut(out + at, c->next_fid);
    at += BookVarintPut(out + at, c->next_tag);
    return at + BookVarintPut(out + at, c->next_bookmark);
}

static inline int SpineCountsGet(const uint8_t *b, uint32_t len, SpineCounts *c)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, &c->next_fid) || !TallyTake(b, len, &at, &c->next_tag) ||
        !TallyTake(b, len, &at, &c->next_bookmark))
        return TALLY_BODY_BROKEN;
    return TallyBodyEnd(at, len);
}

/* SpineFlags — written only when a flag is set: no record reads as 0. */
static inline uint32_t SpineFlagsPut(uint8_t *out, uint64_t flags)
{
    uint32_t at = TallyRecordOpen(out, SPINE_FLAGS, TallyVarintBytes(flags));
    return at + BookVarintPut(out + at, flags);
}

static inline int SpineFlagsGet(const uint8_t *b, uint32_t len, uint64_t *flags)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, flags)) return TALLY_BODY_BROKEN;
    return TallyBodyEnd(at, len);
}

/* ------------------------------------------------------------------------- */
/* A unit laid on its sectors                                                */
/* ------------------------------------------------------------------------- */

/*
 * A unit's stream of `len` bytes laid on its `of` sectors at `out` (len at
 * most of * 488): each sector's payload is its 488 bytes of the stream, zeros
 * — PAD — past the stream's end; its head the unit's `kind` and `seq`, its
 * place `at` of `of`, and its lead: where in it the first record that begins
 * there begins, 0 when none does (§2.1, §16.3 п.4). `starts` are the stream
 * offsets the unit's records begin at, rising — a NodeHead and each item, or
 * each record of a slot; never PAD. Each sector is sealed at its own address,
 * vlba + at, so a sector put down in the wrong place does not hold there.
 */
/* Sector `k` of `of` of a unit laid at `sector` and sealed at `vlba` — the one
 * rule of TallyUnitSeal below, for a writer whose unit does not lie in one run
 * of sectors (a stitch that runs on into its next_quire, volume_ribbon.h).
 * *s is the first of `starts` not yet passed, carried from one sector to the
 * next in order. */
static inline void TallySectorLay(uint8_t *sector, const uint8_t *stream, uint64_t len, const uint32_t *starts,
                                  uint32_t nstarts, uint32_t *s, uint16_t kind, uint64_t seq, uint32_t k,
                                  uint32_t of, uint64_t vlba, const uint8_t uuid[16])
{
    uint64_t       lo = (uint64_t)k * BOOK_PAYLOAD_BYTES;
    BookSectorHead h  = { .crc32c = 0, .lead = 0, .kind = kind, .at = k, .of = of, .seq = seq };
    for (uint32_t i = 0; i < BOOK_PAYLOAD_BYTES; i++)
        sector[BOOK_HEAD_BYTES + i] = lo + i < len ? stream[lo + i] : 0u;
    while (*s < nstarts && starts[*s] < lo) (*s)++;
    if (*s < nstarts && starts[*s] < lo + BOOK_PAYLOAD_BYTES)
        h.lead = (uint16_t)(BOOK_HEAD_BYTES + (starts[*s] - lo));
    BookSectorSeal(sector, uuid, vlba, &h);
}

static inline void TallyUnitSeal(uint8_t *out, const uint8_t *stream, uint64_t len, const uint32_t *starts,
                                 uint32_t nstarts, uint16_t kind, uint64_t seq, uint64_t vlba, uint32_t of,
                                 const uint8_t uuid[16])
{
    uint32_t s = 0;
    for (uint32_t k = 0; k < of; k++)
        TallySectorLay(out + (uint64_t)k * BOOK_SECTOR_BYTES, stream, len, starts, nstarts, &s, kind, seq, k, of,
                       vlba + k, uuid);
}

#endif /* VOLUME_TALLY_H */
