#ifndef VOLUME_RIBBON_H
#define VOLUME_RIBBON_H

/*
 * The ribbon's stitches and the records they carry — how each is written and
 * read.
 *
 * A Settle cycle of a volume lays its changes down as one stitch on
 * the ribbon: a unit of kind STITCH whose stream (volume_book.h) is a
 * StitchHead, the cycle's records about files and tags, and a Clasp, then PAD
 * to the end of its last atom. The Tally (volume_tally.h) sums the ribbon up
 * from time to time; between two Tallies a mount reads the stitches past the
 * Tally's and replays them record by record (§4.5 п.5).
 *
 * ‼ ONE CODEC FOR EVERY WRITER AND READER OF THEM, as volume_tally.h is for the
 * Tally: the kernel reads a ribbon with this (mount/ribbon_*.c) and its stand
 * builds stitches with it, and volume-read on the host says the same again by
 * its own code (tools/volumeread/stitch.py), whose landings — a stitch sewn by
 * the host, read by the kernel — hold the two to one spelling. A run is the
 * Tally's codec in the ribbon's form (BookRunGet): no birth_delta, its birth
 * the seq of the stitch that put it in.
 *
 * A reader is handed a record's body and its length: every *Get answers
 * TALLY_BODY_KNOWN when it read exactly the fields it knows, TALLY_BODY_LONGER
 * when bytes are left past them — written by a kernel that knows more, and
 * the stitch is not applied (§2.3, M-8), which is the caller's to say aloud —
 * and TALLY_BODY_BROKEN when a field is cut short or written as the book
 * never writes one. `used` is the bytes the known fields took. A list inside
 * a body (ids, runs, splices, digests) is checked whole by the *Get and left
 * where it stands, pointed at, to be walked again by the caller with the
 * same primitives — every one of them already found well formed.
 *
 * No library, no x86 instruction, no state: the rule of volume_book.h.
 */

#include "volume_tally.h"

/* ------------------------------------------------------------------------- */
/* Kinds, flags and bits                                                      */
/* ------------------------------------------------------------------------- */

/* The kinds of a stitch's records (§2.7, §2.8); a record's tag is kind << 1 |
 * ignorable. Kind 3 is Undone: the Catchword of the iron draft is a field of
 * StitchHead (next_quire), not a record. */
#define RIBBON_STITCH_HEAD   1u
#define RIBBON_CLASP         2u
#define RIBBON_UNDONE        3u
#define RIBBON_BORN          4u
#define RIBBON_SHAPED        5u
#define RIBBON_LABELED       6u
#define RIBBON_QUITCLAIM     7u
#define RIBBON_COINED        8u
#define RIBBON_USE_CONTEXT   9u
#define RIBBON_FROZEN       10u
#define RIBBON_THAWED       11u
#define RIBBON_KINDS_KNOWN  11u

/* StitchHead.flags (§2.7). */
#define STITCH_DATA_FLUSHED  1u   /* the cycle paid its first flush before the stitch */
#define STITCH_CLEAN         2u   /* the last cycle of a halt */
#define STITCH_MEDIUM_LIED   4u   /* the volume has caught its medium not keeping what it confirmed */

/* Labeled.what (§2.8). */
#define LABEL_NAME   1u
#define LABEL_FLAGS  2u
#define LABEL_TAGS   4u
#define LABEL_KEEP   8u   /* the file's deletion, and the time it is kept until (0: brought back) */
#define LABEL_SHOWN  16u  /* the programs a program's file shows its own tags to, the whole list anew (0 of them: none) */

/* The extensions a run may carry (§8), a TLV list after its head: CODEC's body
 * is the codec and the blocks it stores; SHARED names ground another owns. */
#define RUN_EXT_CODEC   1u
#define RUN_EXT_SHARED  2u
#define RUN_EXT_COPY    3u

/* Undone.why (§2.8). */
#define UNDONE_NOT_LANDED  1u
#define UNDONE_ROT         2u

/* ------------------------------------------------------------------------- */
/* The golden stitch                                                          */
/* ------------------------------------------------------------------------- */

/*
 * A unit defined by a rule, as volume_tally.h's golden node is: its eight
 * sectors — sealed for the volume 00 01 .. 0F — sum by CRC32C (BookCrc32cOn
 * over the 4096 bytes, inverted) to the number below. The kernel's ribbon
 * suite builds it with this codec, volume-read's self-test X3 with stitch.py's
 * own encoders. A rule and a number, not 4096 bytes spelled twice.
 *
 * Seq 7 at sector 0x4000, one block. StitchHead {DATA_FLUSHED, prev 2,
 * prev_clasp 0xC0FFEE11, tally 4, made_ns 1700000000123456789, next_quire 3000
 * for 256, from_quire 2560 for 256}, then:
 *   Undone     seq_back 1, why 1, fids 5, 6
 *   Coined     id 42, HAS_VALUE, "key" = "val"
 *   Born       fid 5, ACTIVE, 5000 bytes, created 1700000000000000000 and
 *              modified 7 ns later, "golden", tags 3, 5, 300, runs DATA 1024
 *              for 1 and a HOLE of 1, digest 0x1122334455667788, Slip "tail"
 *              at byte 100
 *   Shaped     fid 6, base 1, 9000 bytes, mtime 2, splices (1, take 1, put
 *              DATA 4096 for 1) and (0, take 0, put a HOLE of 1), digest
 *              0x99AABBCCDDEEFF00, no Slip
 *   Labeled    fid 7, base 2, NAME "renamed", FLAGS set 4 clear 2, TAGS put on
 *              9 and take off 3
 *   Quitclaim  fid 8, base 3
 *   UseContext "work,play"
 *   Frozen     id 1, scope mode 1 over ids 2, 3, "snap"
 *   Thawed     id 1
 *   kind 31, ignorable, body 07
 * and its Clasp: 10 records, 2 fresh blocks, and the stream's sum.
 */
#define RIBBON_GOLDEN_STITCH_CRC  0xD5743D56u

/* ------------------------------------------------------------------------- */
/* A record, and the lists inside one                                         */
/* ------------------------------------------------------------------------- */

/* A whole record — tag, length, body — of `kind`, ignorable or not, into
 * `out`: the bytes it took. */
static inline uint32_t RibbonRecordPut(uint8_t *out, uint64_t kind, int ignorable, const uint8_t *body,
                                       uint32_t len)
{
    uint32_t at = BookVarintPut(out, kind << 1 | (ignorable ? 1u : 0u));
    at += BookVarintPut(out + at, len);
    return at + TallyBytesPut(out + at, body, len);
}

/* A rising list of ids with its count in front, as every list of ids in the
 * book is written (§2.8): the count, then TallyIdsPut. */
static inline uint32_t RibbonIdsPut(uint8_t *out, const uint32_t *ids, uint32_t n)
{
    uint32_t at = BookVarintPut(out, n);
    return at + TallyIdsPut(out + at, ids, n);
}

/* A rising list of file ids, written as every list of ids is (RibbonIdsPut):
 * a file id is 64 bits wide (§2.2), and one rule with stitch.py's IdsPut. */
static inline uint32_t RibbonFidsPut(uint8_t *out, const uint64_t *fids, uint32_t n)
{
    uint32_t at = BookVarintPut(out, n);
    for (uint32_t i = 0; i < n; i++)
        at += BookVarintPut(out + at, i ? fids[i] - fids[i - 1] - 1u : fids[0]);
    return at;
}

/* Such a list read whole from *at: its count in *n, its ids pointed at in
 * *ids for *bytes bytes, each walked again by TallyIdNext. 0 when it does not
 * read, or passes 2^64 - 1. */
static inline int RibbonIdsTake(const uint8_t *b, uint32_t end, uint32_t *at, uint64_t *n, const uint8_t **ids,
                                uint32_t *bytes)
{
    uint64_t id = 0;
    if (!TallyTake(b, end, at, n) || *n > end - *at) return 0;
    *ids = b + *at;
    uint32_t from = *at;
    for (uint64_t k = 0; k < *n; k++)
        if (!TallyIdNext(b, end, at, &id, k == 0)) return 0;
    *bytes = *at - from;
    return 1;
}

/* A run in the ribbon's form (§2.8): no birth_delta, born at `seq`. */
static inline uint32_t RibbonRunPut(uint8_t *out, const TallyRun *r, uint64_t *cursor)
{
    return BookRunPut(out, r, cursor, 0, 0);
}

static inline int RibbonRunGet(const uint8_t *b, uint32_t end, uint32_t *at, uint64_t *cursor, uint64_t seq,
                               TallyRun *r)
{
    return BookRunGet(b, end, at, cursor, seq, 0, r);
}

/* `n` runs of the ribbon's form read whole from *at, the cursor carried in
 * and out (*cursor), each born at `seq`: their blocks in *blocks (held at
 * 2^64 - 1 rather than wrapped) and the digests a record carries for them in
 * *digests (RibbonRunDigests). 0 when one does not read. */
static inline int RibbonRunDigests(const TallyRun *r, uint64_t *n);

static inline int RibbonRunsTake(const uint8_t *b, uint32_t end, uint32_t *at, uint64_t n, uint64_t *cursor,
                                 uint64_t seq, uint64_t *blocks, uint64_t *digests)
{
    for (uint64_t i = 0; i < n; i++) {
        TallyRun r;
        if (!RibbonRunGet(b, end, at, cursor, seq, &r) || !RibbonRunDigests(&r, digests)) return 0;
        *blocks = r.count > ~(uint64_t)0 - *blocks ? ~(uint64_t)0 : *blocks + r.count;
    }
    return 1;
}

/* The body of a run's extension of `kind`, the first the list names (*body,
 * *len): 1 when one is named, 0 when none is, -1 when the list does not
 * walk to its end — the whole list is walked either way. */
static inline int RibbonRunExt(const TallyRun *r, uint64_t kind, const uint8_t **body, uint32_t *len)
{
    uint32_t at = 0, from = 0, n = 0;
    uint64_t tag = 0;
    int      got, named = 0;
    while (r->ext && (got = BookEntryNext(r->ext, r->ext_len, &at, &tag, &from, &n)) != 0) {
        if (got < 0) return -1;
        if (tag >> 1 == kind && !named) {
            *body = r->ext + from;
            *len  = n;
            named = 1;
        }
    }
    return named;
}

/*
 * The digests a record carries for a run it puts in (§2.8), added to *n: one
 * a block of a DATA run, none for a hole or a run SHARED (its blocks are
 * another's), the stored blocks of one CODEC (§8.3). 0 when its extensions do
 * not walk, or a CODEC's body does not read — the record does not read.
 */
static inline int RibbonRunDigests(const TallyRun *r, uint64_t *n)
{
    const uint8_t *body = (const uint8_t *)0;
    uint32_t       len = 0, at = 0;
    uint64_t       codec = 0, stored = 0;
    if (r->hole) return 1;
    int shared = RibbonRunExt(r, RUN_EXT_SHARED, &body, &len);
    if (shared < 0) return 0;
    if (shared) return 1;
    int coded = RibbonRunExt(r, RUN_EXT_CODEC, &body, &len);
    if (coded < 0) return 0;
    if (coded) {
        if (!TallyTake(body, len, &at, &codec) || !TallyTake(body, len, &at, &stored)) return 0;
        *n = stored > ~(uint64_t)0 - *n ? ~(uint64_t)0 : *n + stored;
        return 1;
    }
    *n = r->count > ~(uint64_t)0 - *n ? ~(uint64_t)0 : *n + r->count;
    return 1;
}

/*
 * Whether a reader of this book carries run `r` on (§8.2): RUN_CARRIED_PLAIN
 * for one with no extension; RUN_CARRIED_SHARED for a DATA run whose every
 * extension is SHARED with an empty body — one at least — ground another run
 * may name too; RUN_CARRIED_NOT for any other — a hole with an extension,
 * CODEC, COPY, a kind this reader does not know, a SHARED with a body, a list
 * that does not walk: a newer writer's, and the volume is only read. One rule
 * with volume-read (tools/volumeread/tally.py, RunCarried).
 */
#define RUN_CARRIED_NOT     0
#define RUN_CARRIED_PLAIN   1
#define RUN_CARRIED_SHARED  2

static inline int RibbonRunCarried(const TallyRun *r)
{
    uint32_t at = 0, from = 0, n = 0;
    uint64_t tag = 0;
    int      got, named = 0;
    if (!r->ext)
        return RUN_CARRIED_PLAIN;
    if (r->hole)
        return RUN_CARRIED_NOT;
    while ((got = BookEntryNext(r->ext, r->ext_len, &at, &tag, &from, &n)) > 0) {
        if (tag >> 1 != RUN_EXT_SHARED || n)
            return RUN_CARRIED_NOT;
        named = 1;
    }
    return got == 0 && named ? RUN_CARRIED_SHARED : RUN_CARRIED_NOT;
}

/* The list of extensions a writer gives a run SHARED, *len bytes: SHARED
 * alone, its body empty. */
static inline const uint8_t *RibbonSharedList(uint32_t *len)
{
    static const uint8_t list[2] = { RUN_EXT_SHARED << 1, 0 };
    *len = sizeof(list);
    return list;
}

/* The fresh blocks a run names (Clasp.fresh, §2.7): a DATA run's, unless it
 * is SHARED. Its extensions were walked when the record was read. */
static inline uint64_t RibbonRunFresh(const TallyRun *r)
{
    const uint8_t *body = (const uint8_t *)0;
    uint32_t       len  = 0;
    return r->hole || RibbonRunExt(r, RUN_EXT_SHARED, &body, &len) ? 0 : r->count;
}

/* `n` digests from *at, pointed at (TallyDigestAt reads the i-th). */
static inline int RibbonDigestsTake(const uint8_t *b, uint32_t end, uint32_t *at, uint64_t n,
                                    const uint8_t **digests)
{
    if (*at > end || n > (end - *at) / 8u) return 0;
    *digests = b + *at;
    *at += (uint32_t)(n * 8u);
    return 1;
}

/* A Slip (§2.8): slip_off · slip_len · slip — bytes laid on the file's last
 * block; slip_len 0 lays none. */
typedef struct {
    uint64_t       off;
    uint64_t       len;
    const uint8_t *bytes;
} RibbonSlip;

static inline uint32_t RibbonSlipPut(uint8_t *out, const RibbonSlip *s)
{
    uint32_t at = BookVarintPut(out, s->off);
    at += BookVarintPut(out + at, s->len);
    return at + TallyBytesPut(out + at, s->bytes, s->len);
}

static inline int RibbonSlipTake(const uint8_t *b, uint32_t end, uint32_t *at, RibbonSlip *s)
{
    return TallyTake(b, end, at, &s->off) && TallyTake(b, end, at, &s->len) &&
           TallyTakeBytes(b, end, at, s->len, &s->bytes);
}

/* ------------------------------------------------------------------------- */
/* StitchHead and Clasp (§2.7)                                               */
/* ------------------------------------------------------------------------- */

/*
 * StitchHead — the first record of every stitch: its flags; `prev`, its seq
 * less the seq of the stitch before it (0 for the first of a volume), and
 * `prev_clasp`, that stitch's Clasp — the identity a stitch is sewn on by
 * (H-2); `tally`, its seq less the Tally's it is replayed over; when it was
 * made; the next_quire (Catchword, M-5) once one is given, and in the first
 * stitch begun at a Quire's start the Quire the ribbon came from.
 */
typedef struct {
    uint64_t flags;
    uint64_t prev;
    uint32_t prev_clasp;
    uint64_t tally;
    uint64_t made_ns;
    uint64_t next_block;
    uint64_t next_blocks;
    uint64_t from_block;
    uint64_t from_blocks;
} StitchHead;

static inline uint32_t StitchHeadBodyPut(uint8_t *out, const StitchHead *h)
{
    uint32_t at = BookVarintPut(out, h->flags);
    at += BookVarintPut(out + at, h->prev);
    at += TallyLePut(out + at, h->prev_clasp, 4);
    at += BookVarintPut(out + at, h->tally);
    at += BookVarintPut(out + at, h->made_ns);
    at += BookVarintPut(out + at, h->next_block);
    at += BookVarintPut(out + at, h->next_blocks);
    at += BookVarintPut(out + at, h->from_block);
    return at + BookVarintPut(out + at, h->from_blocks);
}

static inline int StitchHeadGet(const uint8_t *b, uint32_t len, StitchHead *h, uint32_t *used)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, &h->flags) || !TallyTake(b, len, &at, &h->prev) || len - at < 4u)
        return TALLY_BODY_BROKEN;
    h->prev_clasp = (uint32_t)TallyLe(b + at, 4);
    at += 4u;
    if (!TallyTake(b, len, &at, &h->tally) || !TallyTake(b, len, &at, &h->made_ns) ||
        !TallyTake(b, len, &at, &h->next_block) || !TallyTake(b, len, &at, &h->next_blocks) ||
        !TallyTake(b, len, &at, &h->from_block) || !TallyTake(b, len, &at, &h->from_blocks))
        return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

/* Clasp — the last record before PAD: the records between StitchHead and it,
 * the fresh blocks they name (the bound of the mount's check, seen before a
 * block of data is read), and the CRC32C of the stream from StitchHead's first
 * byte to the byte before its own tag: the stitch's identity, named by the
 * next one's prev_clasp. */
typedef struct {
    uint64_t records;
    uint64_t fresh;
    uint32_t crc;
} Clasp;

static inline uint32_t ClaspBodyPut(uint8_t *out, const Clasp *c)
{
    uint32_t at = BookVarintPut(out, c->records);
    at += BookVarintPut(out + at, c->fresh);
    return at + TallyLePut(out + at, c->crc, 4);
}

static inline int ClaspGet(const uint8_t *b, uint32_t len, Clasp *c, uint32_t *used)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, &c->records) || !TallyTake(b, len, &at, &c->fresh) || len - at < 4u)
        return TALLY_BODY_BROKEN;
    c->crc = (uint32_t)TallyLe(b + at, 4);
    *used  = at + 4u;
    return TallyBodyEnd(at + 4u, len);
}

/*
 * The shape of a stitch a reader takes (§16.3 п.8, §1.5, §2.2): a whole number
 * of a block's eight sectors and one block at least — no writer's atom is
 * smaller, and a reader takes the next unit where the last one ends by its
 * writer's atom, not its own (§16.2 п.11), so one larger than a block is not
 * asked of it; begun on a block's boundary of its Quire; and no longer than
 * the rest of its Quire and the next Quire hold. `off` is the sector it begins
 * at in its Quire, `left` the sectors from there to the Quire's end, `hop`
 * the sectors of the Quire it may run on into. Nor is a stream longer than
 * RIBBON_STREAM_MOST read: a reader's offsets into it are 32 bits, and a
 * stitch of two Quires — the most a writer lays — comes nowhere near it.
 * RIBBON_SHAPE_KEPT when it is so shaped; otherwise which rule it breaks, for
 * the line that says so.
 */
#define RIBBON_SHAPE_KEPT    0
#define RIBBON_SHAPE_ATOMS   1
#define RIBBON_SHAPE_OFF     2
#define RIBBON_SHAPE_LONG    3
#define RIBBON_SHAPE_STREAM  4

#define RIBBON_STREAM_MOST   0xFFFFFFFFu

/* The sectors a stitch of a `len`-byte stream takes when it is written in
 * whole atoms of `atom` sectors (§1.5, §2.2): one atom at the least, its tail
 * PAD. */
static inline uint64_t RibbonStitchSectors(uint64_t len, uint32_t atom)
{
    uint64_t of = (len + BOOK_PAYLOAD_BYTES - 1u) / BOOK_PAYLOAD_BYTES;
    of = (of + atom - 1u) / atom * atom;
    return of ? of : atom;
}

/*
 * ‼ A STITCH ENDS WHERE AN ATOM OF THE RIBBON DOES (§1.5, §16.6 п.2). Written
 * in whole atoms of the medium from where it begins — `need` sectors of them
 * (RibbonStitchSectors) — a stitch goes on to the end of an atom of the
 * ribbon, `r` sectors — a flash page on a removable medium, the medium's own
 * atom on any other — counted from the volume's first sector, which a ribbon
 * of `r` begins on (book_mount.c): so the stitch after it begins on one, and
 * shares it with no settled stitch, whatever atom laid the stitch before it
 * or the Quire it lies in. `at` the volume's sector it begins at, `left` the
 * sectors of its Quire from there, `next` the first sector of the next_quire
 * named, 0 with none; its sectors. A Quire a writer of a smaller atom laid may
 * end inside an atom of the ribbon: a stitch that would end past it ends with
 * it, the next begun at the next_quire's start; with none named, where the
 * atom a stitch keeps past it (§3.6) falls past that end, it ends on the
 * medium's atom alone, and the stitch after it shares its page (ribbon.c,
 * RibbonRoom, says so). Run on into the next_quire, its tail ends on an atom
 * of the ribbon in turn; with none named it runs nowhere (RibbonRoom refuses).
 */
static inline uint64_t RibbonStitchEnds(uint64_t need, uint64_t at, uint64_t left, uint64_t next, uint32_t r)
{
    uint64_t of;
    if (need > left)
        return next ? left + ((next + need - left + r - 1u) / r * r - next) : need;
    of = (at + need + r - 1u) / r * r - at;
    if (of <= left && (next || of + r <= left))
        return of;
    return next && of > left ? left : need;
}

/*
 * ‼ A WRITER GOES ON WHERE A READER TAKES THE NEXT STITCH UP, AND WRITES NO
 * ATOM OF THE MEDIUM A SETTLED SECTOR LIES IN (§16.2 п.11, §1.5, §16.6 п.2).
 * A reader takes the next unit where the last one ends, whatever atom laid
 * it, or at the start of the next_quire the last one names (rule (б)) — never
 * past a gap inside a Quire. Past an end at sector `off` of the Quire of
 * `qn` blocks at block `qb`, the next_quire `nb`+`nn` (`nn` 0 with none
 * named), in atoms of the medium `m` and of the ribbon `r` (sectors), the
 * writer goes on:
 *   RIBBON_ON_END     at the end — its Quire laid on the medium's atom, the
 *                     end on one — sharing no page where the end lies on an
 *                     atom of the ribbon;
 *   RIBBON_ON_NEXT    else at the start of a next_quire laid on an atom of
 *                     the ribbon, the rest of the Quire left as an early
 *                     close leaves it;
 *   | RIBBON_ON_SHARED else at the end, or at the start of a next_quire laid
 *                     on the medium's atom alone: the stitch laid there first
 *                     shares an atom of the ribbon — a flash page — with the
 *                     last one settled, or with what lies before that Quire;
 *   0                 else nowhere: no writer goes on, the volume under glass
 *                     — never in an atom smaller than the medium's.
 * A Quire filled whole goes on in its next_quire alone.
 */
#define RIBBON_ON_END    1u
#define RIBBON_ON_NEXT   2u
#define RIBBON_ON_SHARED 4u

static inline uint32_t RibbonGoesOn(uint64_t qb, uint64_t qn, uint64_t off, uint64_t nb, uint64_t nn, uint32_t m,
                                    uint32_t r)
{
    uint64_t mb = m / VOLUME_BLOCK_SECTORS ? m / VOLUME_BLOCK_SECTORS : 1u;
    uint64_t rb = r / VOLUME_BLOCK_SECTORS ? r / VOLUME_BLOCK_SECTORS : 1u;
    uint64_t at = qb * VOLUME_BLOCK_SECTORS + off;
    int      end = off < qn * VOLUME_BLOCK_SECTORS && !(qb % mb) && !(qn % mb) && !(at % m);
    int      on_m = nn && !(nb % mb) && !(nn % mb);
    if (end && !(at % r))
        return RIBBON_ON_END;
    if (on_m && !(nb % rb) && !(nn % rb))
        return RIBBON_ON_NEXT;
    if (end)
        return RIBBON_ON_END | RIBBON_ON_SHARED;
    return on_m ? RIBBON_ON_NEXT | RIBBON_ON_SHARED : 0u;
}

/*
 * A stitch's stream of `len` bytes laid on its `of` sectors at `out`, sealed
 * where each lies: the first `left` at `vlba` on — the rest of its Quire — and
 * the others from `hop_vlba`, the first sector of the next_quire it runs on
 * into (§2.7). `starts` are its records' offsets in the stream, StitchHead's
 * and Clasp's among them, rising: each sector's lead. TallySectorLay's one rule.
 */
static inline void RibbonStitchSeal(uint8_t *out, const uint8_t *stream, uint64_t len, const uint32_t *starts,
                                    uint32_t nstarts, uint64_t seq, uint64_t vlba, uint64_t left,
                                    uint64_t hop_vlba, uint32_t of, const uint8_t uuid[16])
{
    uint32_t s = 0;
    for (uint32_t k = 0; k < of; k++)
        TallySectorLay(out + (uint64_t)k * BOOK_SECTOR_BYTES, stream, len, starts, nstarts, &s, BOOK_UNIT_STITCH,
                       seq, k, of, k < left ? vlba + k : hop_vlba + (k - left), uuid);
}

static inline int RibbonShape(uint64_t off, uint64_t left, uint64_t hop, uint64_t of)
{
    if (of < VOLUME_BLOCK_SECTORS || of % VOLUME_BLOCK_SECTORS) return RIBBON_SHAPE_ATOMS;
    if (off % VOLUME_BLOCK_SECTORS) return RIBBON_SHAPE_OFF;
    if (of > left && of - left > hop) return RIBBON_SHAPE_LONG;
    if (of > RIBBON_STREAM_MOST / BOOK_PAYLOAD_BYTES) return RIBBON_SHAPE_STREAM;
    return RIBBON_SHAPE_KEPT;
}

/* ------------------------------------------------------------------------- */
/* The records about files and tags (§2.8)                                   */
/* ------------------------------------------------------------------------- */

/*
 * Born — a file born, its whole state: fid · flags · size · created_ns ·
 * mtime_z · name · tags (count, rising) · nruns · runs (from cursor 0) ·
 * digests · slip · [made_by] · [nshown · shown]. A run covers exactly the blocks its size needs,
 * holes written as HOLE runs; the digests are those of every block its DATA
 * runs put in (RibbonRunDigests), and the slip lays the tail of a file shorter
 * than its last block (§2.8, H-7). made_by is there when, and only when, the
 * flags carry TALLY_FILE_MADE_BY: the file of the program that made the file,
 * never 0 (TallyFileHead says the same of a header); the programs it shows
 * its own tags to follow, there when, and only when, the flags carry
 * TALLY_FILE_SHOWN — the count never 0, the ids rising (TallyFidsPut). The
 * writer gives `tags`, `runs` and `shown` as their bytes already laid
 * (RibbonIdsPut's body after the count, RibbonRunPut each, TallyFidsPut); the
 * reader finds them pointed at where they stand.
 */
typedef struct {
    uint64_t       fid;
    uint64_t       flags;
    uint64_t       size;
    uint64_t       created_ns;
    uint64_t       modified_ns;
    const uint8_t *name;
    uint64_t       name_len;
    uint64_t       ntags;
    const uint8_t *tags;
    uint32_t       tags_bytes;
    uint64_t       nruns;
    const uint8_t *runs;
    uint32_t       runs_bytes;
    uint64_t       blocks;       /* read: the blocks its runs cover */
    uint64_t       ndigests;
    const uint8_t *digests;
    RibbonSlip     slip;
    uint64_t       made_by;      /* 0 unless TALLY_FILE_MADE_BY */
    uint64_t       nshown;       /* 0 unless TALLY_FILE_SHOWN */
    const uint8_t *shown;
    uint32_t       shown_bytes;
} RibbonBorn;

static inline uint32_t RibbonBornBodyPut(uint8_t *out, const RibbonBorn *r)
{
    uint32_t at = BookVarintPut(out, r->fid);
    at += BookVarintPut(out + at, r->flags);
    at += BookVarintPut(out + at, r->size);
    at += BookVarintPut(out + at, r->created_ns);
    at += BookVarintPut(out + at, BookZigzag((int64_t)(r->modified_ns - r->created_ns)));
    at += BookVarintPut(out + at, r->name_len);
    at += TallyBytesPut(out + at, r->name, r->name_len);
    at += BookVarintPut(out + at, r->ntags);
    at += TallyBytesPut(out + at, r->tags, r->tags_bytes);
    at += BookVarintPut(out + at, r->nruns);
    at += TallyBytesPut(out + at, r->runs, r->runs_bytes);
    at += TallyBytesPut(out + at, r->digests, r->ndigests * 8u);
    at += RibbonSlipPut(out + at, &r->slip);
    if (r->flags & TALLY_FILE_MADE_BY)
        at += BookVarintPut(out + at, r->made_by);
    if (r->flags & TALLY_FILE_SHOWN) {
        at += BookVarintPut(out + at, r->nshown);
        at += TallyBytesPut(out + at, r->shown, r->shown_bytes);
    }
    return at;
}

static inline int RibbonBornGet(const uint8_t *b, uint32_t len, uint64_t seq, RibbonBorn *r, uint32_t *used)
{
    uint32_t at = 0, from;
    uint64_t z, cursor = 0;
    r->blocks = r->ndigests = r->made_by = r->nshown = 0;
    r->shown       = (const uint8_t *)0;
    r->shown_bytes = 0;
    if (!TallyTake(b, len, &at, &r->fid) || !TallyTake(b, len, &at, &r->flags) ||
        !TallyTake(b, len, &at, &r->size) || !TallyTake(b, len, &at, &r->created_ns) ||
        !TallyTake(b, len, &at, &z) || !TallyTake(b, len, &at, &r->name_len) ||
        !TallyTakeBytes(b, len, &at, r->name_len, &r->name) ||
        !RibbonIdsTake(b, len, &at, &r->ntags, &r->tags, &r->tags_bytes) || !TallyTake(b, len, &at, &r->nruns))
        return TALLY_BODY_BROKEN;
    if (r->flags & TALLY_FILE_DELETED)
        return TALLY_BODY_BROKEN;           /* a file is born whole, never deleted: Labeled KEEP deletes it */
    r->modified_ns = r->created_ns + (uint64_t)BookUnzigzag(z);
    from    = at;
    r->runs = b + from;
    if (!RibbonRunsTake(b, len, &at, r->nruns, &cursor, seq, &r->blocks, &r->ndigests))
        return TALLY_BODY_BROKEN;
    r->runs_bytes = at - from;
    if (!RibbonDigestsTake(b, len, &at, r->ndigests, &r->digests) || !RibbonSlipTake(b, len, &at, &r->slip))
        return TALLY_BODY_BROKEN;
    if ((r->flags & TALLY_FILE_MADE_BY) && (!TallyTake(b, len, &at, &r->made_by) || !r->made_by))
        return TALLY_BODY_BROKEN;
    if ((r->flags & TALLY_FILE_SHOWN) &&
        (!TallyFidsTake(b, len, &at, &r->nshown, &r->shown, &r->shown_bytes) || !r->nshown))
        return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

/*
 * Shaped — a file's size and runs changed by splices: fid · base · size ·
 * mtime_ns · nsplice · { at · remove · ninsert · runs } × nsplice · digests ·
 * slip. `at` is the distance from the end of the splice before it in the file
 * as it stands after it (its end = its absolute at + the blocks it put in), 0
 * from the file's start for the first; the runs' cursor runs on from splice to
 * splice. RibbonSpliceNext walks them again.
 */
typedef struct {
    uint64_t       fid;
    uint64_t       base;
    uint64_t       size;
    uint64_t       mtime_ns;
    uint64_t       nsplice;
    const uint8_t *splices;
    uint32_t       splices_bytes;
    uint64_t       ndigests;
    const uint8_t *digests;
    RibbonSlip     slip;
} RibbonShaped;

/* One splice as it is read: where it begins (relative, as written), the blocks
 * it takes out, the runs it puts in — pointed at, their cursor in `cursor` —
 * and the blocks they cover. */
typedef struct {
    uint64_t       at;
    uint64_t       remove;
    uint64_t       ninsert;
    const uint8_t *runs;
    uint32_t       runs_bytes;
    uint64_t       cursor;
    uint64_t       blocks;
} RibbonSplice;

/* The next splice of a list read whole from *at (the cursor carried in
 * *cursor, the digests counted into *digests): 0 when it does not read. */
static inline int RibbonSpliceNext(const uint8_t *b, uint32_t end, uint32_t *at, uint64_t *cursor, uint64_t seq,
                                   uint64_t *digests, RibbonSplice *s)
{
    if (!TallyTake(b, end, at, &s->at) || !TallyTake(b, end, at, &s->remove) ||
        !TallyTake(b, end, at, &s->ninsert))
        return 0;
    uint32_t from = *at;
    s->runs   = b + from;
    s->cursor = *cursor;
    s->blocks = 0;
    if (!RibbonRunsTake(b, end, at, s->ninsert, cursor, seq, &s->blocks, digests)) return 0;
    s->runs_bytes = *at - from;
    return 1;
}

static inline uint32_t RibbonSplicePut(uint8_t *out, uint64_t at_from_last, uint64_t remove, uint64_t ninsert,
                                       const uint8_t *runs, uint32_t runs_bytes)
{
    uint32_t at = BookVarintPut(out, at_from_last);
    at += BookVarintPut(out + at, remove);
    at += BookVarintPut(out + at, ninsert);
    return at + TallyBytesPut(out + at, runs, runs_bytes);
}

static inline uint32_t RibbonShapedBodyPut(uint8_t *out, const RibbonShaped *r)
{
    uint32_t at = BookVarintPut(out, r->fid);
    at += BookVarintPut(out + at, r->base);
    at += BookVarintPut(out + at, r->size);
    at += BookVarintPut(out + at, r->mtime_ns);
    at += BookVarintPut(out + at, r->nsplice);
    at += TallyBytesPut(out + at, r->splices, r->splices_bytes);
    at += TallyBytesPut(out + at, r->digests, r->ndigests * 8u);
    return at + RibbonSlipPut(out + at, &r->slip);
}

static inline int RibbonShapedGet(const uint8_t *b, uint32_t len, uint64_t seq, RibbonShaped *r, uint32_t *used)
{
    uint32_t     at = 0, from;
    uint64_t     cursor = 0;
    RibbonSplice s;
    r->ndigests = 0;
    if (!TallyTake(b, len, &at, &r->fid) || !TallyTake(b, len, &at, &r->base) ||
        !TallyTake(b, len, &at, &r->size) || !TallyTake(b, len, &at, &r->mtime_ns) ||
        !TallyTake(b, len, &at, &r->nsplice) || r->nsplice > len - at)
        return TALLY_BODY_BROKEN;
    r->splices = b + (from = at);
    for (uint64_t i = 0; i < r->nsplice; i++)
        if (!RibbonSpliceNext(b, len, &at, &cursor, seq, &r->ndigests, &s)) return TALLY_BODY_BROKEN;
    r->splices_bytes = at - from;
    if (!RibbonDigestsTake(b, len, &at, r->ndigests, &r->digests) || !RibbonSlipTake(b, len, &at, &r->slip))
        return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

/*
 * Labeled — a file's name, flags, tags, deletion or the programs it shows its
 * tags to: fid · base · what · [name] · [set · clear] · [add (count, rising) ·
 * drop (count, rising)] · [keep_ns] · [nshown · shown].
 * A bit of `what` this reader does not know carries no field: it is a
 * writer's, and a reader that finds bytes left past the fields it knows takes
 * the record for a newer writer's (TALLY_BODY_LONGER) and does not apply the
 * stitch.
 *
 * KEEP is the one verb of deletion, and TALLY_FILE_DELETED is never a bit of
 * `set` or `clear` — a Labeled that names it there does not read, nor does
 * one that names TALLY_FILE_MADE_BY, which no change of a file moves: keep_ns
 * above 0 deletes the file, kept until that time (a file deleted already has
 * its term moved), and keep_ns 0 brings it back. The bit in the file's flags
 * is what the verb leaves, put there by the one reader and the other alike.
 *
 * SHOWN is the one verb of the list a program's file shows its tags to, and
 * TALLY_FILE_SHOWN is never a bit of `set` or `clear` either: the list comes
 * whole, rising (TallyFidsPut), 0 first for every program — a list of none
 * shows nothing and takes the bit off, any other puts it on.
 */
typedef struct {
    uint64_t       fid;
    uint64_t       base;
    uint64_t       what;
    const uint8_t *name;
    uint64_t       name_len;
    uint64_t       set;
    uint64_t       clear;
    uint64_t       nadd;
    const uint8_t *add;
    uint32_t       add_bytes;
    uint64_t       ndrop;
    const uint8_t *drop;
    uint32_t       drop_bytes;
    uint64_t       keep_ns;      /* with LABEL_KEEP */
    uint64_t       nshown;       /* with LABEL_SHOWN */
    const uint8_t *shown;
    uint32_t       shown_bytes;
} RibbonLabeled;

static inline uint32_t RibbonLabeledBodyPut(uint8_t *out, const RibbonLabeled *r)
{
    uint32_t at = BookVarintPut(out, r->fid);
    at += BookVarintPut(out + at, r->base);
    at += BookVarintPut(out + at, r->what);
    if (r->what & LABEL_NAME) {
        at += BookVarintPut(out + at, r->name_len);
        at += TallyBytesPut(out + at, r->name, r->name_len);
    }
    if (r->what & LABEL_FLAGS) {
        at += BookVarintPut(out + at, r->set);
        at += BookVarintPut(out + at, r->clear);
    }
    if (r->what & LABEL_TAGS) {
        at += BookVarintPut(out + at, r->nadd);
        at += TallyBytesPut(out + at, r->add, r->add_bytes);
        at += BookVarintPut(out + at, r->ndrop);
        at += TallyBytesPut(out + at, r->drop, r->drop_bytes);
    }
    if (r->what & LABEL_KEEP)
        at += BookVarintPut(out + at, r->keep_ns);
    if (r->what & LABEL_SHOWN) {
        at += BookVarintPut(out + at, r->nshown);
        at += TallyBytesPut(out + at, r->shown, r->shown_bytes);
    }
    return at;
}

static inline int RibbonLabeledGet(const uint8_t *b, uint32_t len, RibbonLabeled *r, uint32_t *used)
{
    uint32_t at = 0;
    r->name = r->add = r->drop = r->shown = (const uint8_t *)0;
    r->name_len = r->set = r->clear = r->nadd = r->ndrop = r->keep_ns = r->nshown = 0;
    r->add_bytes = r->drop_bytes = r->shown_bytes = 0;
    if (!TallyTake(b, len, &at, &r->fid) || !TallyTake(b, len, &at, &r->base) || !TallyTake(b, len, &at, &r->what))
        return TALLY_BODY_BROKEN;
    if ((r->what & LABEL_NAME) &&
        (!TallyTake(b, len, &at, &r->name_len) || !TallyTakeBytes(b, len, &at, r->name_len, &r->name)))
        return TALLY_BODY_BROKEN;
    if ((r->what & LABEL_FLAGS) && (!TallyTake(b, len, &at, &r->set) || !TallyTake(b, len, &at, &r->clear)))
        return TALLY_BODY_BROKEN;
    if ((r->what & LABEL_TAGS) && (!RibbonIdsTake(b, len, &at, &r->nadd, &r->add, &r->add_bytes) ||
                                   !RibbonIdsTake(b, len, &at, &r->ndrop, &r->drop, &r->drop_bytes)))
        return TALLY_BODY_BROKEN;
    if (((r->what & LABEL_FLAGS) &&
         ((r->set | r->clear) & (TALLY_FILE_DELETED | TALLY_FILE_MADE_BY | TALLY_FILE_SHOWN))) ||
        ((r->what & LABEL_KEEP) && !TallyTake(b, len, &at, &r->keep_ns)) ||
        ((r->what & LABEL_SHOWN) && !TallyFidsTake(b, len, &at, &r->nshown, &r->shown, &r->shown_bytes)))
        return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

/* Quitclaim — a file gone: fid · base. */
static inline uint32_t RibbonQuitclaimBodyPut(uint8_t *out, uint64_t fid, uint64_t base)
{
    uint32_t at = BookVarintPut(out, fid);
    return at + BookVarintPut(out + at, base);
}

static inline int RibbonQuitclaimGet(const uint8_t *b, uint32_t len, uint64_t *fid, uint64_t *base,
                                     uint32_t *used)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, fid) || !TallyTake(b, len, &at, base)) return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

/* Coined — a tag given its id on the volume: id · the tag as TAGS carries it
 * (volume_tally.h TallyTag: flags, then whose it is when OWNED and the key and
 * the value, unless RETIRED). */
static inline uint32_t RibbonCoinedBodyPut(uint8_t *out, uint64_t id, const TallyTag *t)
{
    uint32_t at = BookVarintPut(out, id);
    return at + TallyTagPut(out + at, t);
}

static inline int RibbonCoinedGet(const uint8_t *b, uint32_t len, uint64_t *id, TallyTag *t, uint32_t *used)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, id) || !TallyTagTake(b, len, &at, t)) return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

/* UseContext — the volume's Use Context whole: len · text (§7); TallyUsePut
 * and TallyUseGet are its codec, the same the Tally's USE element keeps. */

/* Undone — a mount's verdict that outlives the next mount (H-1): seq_back ·
 * why · fids (count, rising) — every record of these fids in the stitch
 * seq_back before this one is passed over. */
static inline uint32_t RibbonUndoneBodyPut(uint8_t *out, uint64_t seq_back, uint64_t why, const uint64_t *fids,
                                           uint32_t n)
{
    uint32_t at = BookVarintPut(out, seq_back);
    at += BookVarintPut(out + at, why);
    return at + RibbonFidsPut(out + at, fids, n);
}

typedef struct {
    uint64_t       seq_back;
    uint64_t       why;
    uint64_t       nfid;
    const uint8_t *fids;
    uint32_t       fids_bytes;
} RibbonUndone;

static inline int RibbonUndoneGet(const uint8_t *b, uint32_t len, RibbonUndone *r, uint32_t *used)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, &r->seq_back) || !TallyTake(b, len, &at, &r->why) ||
        !RibbonIdsTake(b, len, &at, &r->nfid, &r->fids, &r->fids_bytes))
        return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

/* Frozen — a bookmark set (§5): id · scope_mode · [scope (count, rising) when
 * scope_mode is 1] · name. Thawed — one let go: id. This kernel holds no
 * bookmark: it reads them to know their ids and to say so. */
static inline int RibbonFrozenGet(const uint8_t *b, uint32_t len, uint64_t *id, uint32_t *used)
{
    uint32_t       at = 0, bytes = 0;
    uint64_t       mode = 0, n = 0, name_len = 0;
    const uint8_t *p    = (const uint8_t *)0;
    if (!TallyTake(b, len, &at, id) || !TallyTake(b, len, &at, &mode) ||
        (mode == 1 && !RibbonIdsTake(b, len, &at, &n, &p, &bytes)) || !TallyTake(b, len, &at, &name_len) ||
        !TallyTakeBytes(b, len, &at, name_len, &p))
        return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

static inline int RibbonThawedGet(const uint8_t *b, uint32_t len, uint64_t *id, uint32_t *used)
{
    uint32_t at = 0;
    if (!TallyTake(b, len, &at, id)) return TALLY_BODY_BROKEN;
    *used = at;
    return TallyBodyEnd(at, len);
}

#endif /* VOLUME_RIBBON_H */
