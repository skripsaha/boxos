#ifndef TAGFS_MOUNT_RIBBON_H
#define TAGFS_MOUNT_RIBBON_H

#include "../tagfs.h"
#include "volume_ribbon.h"
#include "spine_read.h"
#include "../file_map/file_record.h"

/*
 * The ribbon past a slot's Tally, as a mount reads it:
 *
 *   ribbon_unit.c        a unit of the ribbon read off the medium — its
 *                        sectors judged, its stream parsed whole or, where
 *                        sectors are lost, in pieces by `lead`
 *   ribbon_collate.c     pass 1 (RibbonCollate): the stitches sewn on by
 *                        rules (а)–(г), the Quires they run through, the
 *                        direct scan past the end and what each branch there
 *                        is (§3.9)
 *   ribbon_heads.c       the Quires of a slot not stood on, by its
 *                        StitchHeads alone (RibbonQuiresOf, §16.2 п.12)
 *   ribbon_verify.c      the data of the newest stitch held to its digests
 *                        (RibbonVerify, §4.5 п.6)
 *   ribbon_transcribe.c  pass 2 (RibbonTranscribe): every record replayed
 *                        through the rule of its kind (§3.7) into the memory
 *                        a mount fills, through the one door of the mirror
 *
 * Read the way volume-read reads the same ribbon
 * (tools/volumeread/stitch_ribbon.py Ribbon, bookmount.py _Replay): the census
 * scenario boots a landing of every rule and holds the kernel's lines to what
 * the host predicts. Every line a mount says of the ribbon is
 * `[TagFS] ribbon CODE: ...`, CODE the census's own for the finding.
 *
 * Everything here runs inside the mount, with the door shut, on the one core
 * attending it: the walk's memory is the heap's, never a frame's, and no lock
 * of TagFS is held while a sector is read.
 */

/* What a line of the ribbon is, by the census's code for it. */
typedef enum {
    RIBBON_CODE_RIBBON_TORN,
    RIBBON_CODE_STITCH_TORN,
    RIBBON_CODE_CLASP_STRANGER,
    RIBBON_CODE_DEAD_BRANCH,
    RIBBON_CODE_RECORD_SHAPE,
    RIBBON_CODE_UNDONE,
    RIBBON_CODE_NOT_LANDED,
    RIBBON_CODE_BASE_MISMATCH,
    RIBBON_CODE_EXTENT_OUTSIDE,
    RIBBON_CODE_RETIRED_WORN,
    RIBBON_CODES
} RibbonCode;

/* A Quire of the ribbon — its first block and its blocks; none when blocks is
 * 0 — and a sector of it: the Quire and the sector's offset there. */
typedef struct {
    uint64_t block;
    uint64_t blocks;
} RibbonQuire;

typedef struct {
    RibbonQuire quire;
    uint64_t    off;
} RibbonPlace;

/* A record of a stitch's stream: its kind, whether a reader may step over
 * it, and where it and its body lie in the stream. */
typedef struct {
    uint64_t kind;
    bool     ignorable;
    uint32_t at;
    uint32_t body;
    uint32_t len;
} RibbonRecord;

/* A unit of the ribbon begun at `start` (stitch.py Unit): its seq and `of`,
 * which of its sectors hold as its own (`valid`), which only say so in a head
 * whose sum does not hold (`present`), which hold as a sector of the book at
 * all (`holds`), and what reads of it — whole (`sound`), or in pieces. */
typedef struct {
    RibbonPlace   start;
    RibbonQuire   hop;        /* the Quire it runs on into past its own's end */
    uint64_t      seq;
    uint32_t      of;
    int           shape;      /* RIBBON_SHAPE_KEPT, or the rule it breaks: none of it is read */
    uint8_t*      valid;
    uint8_t*      present;
    uint8_t*      holds;
    uint16_t*     leads;
    uint8_t*      stream;     /* of * 488 bytes, zeros where a sector does not hold */
    uint32_t      size;
    bool          sound;      /* every sector valid, read from StitchHead to a Clasp that holds */
    bool          head_read;
    StitchHead    head;
    uint32_t      longer;     /* bytes past the known fields of its StitchHead and Clasp */
    bool          clasp_read;
    Clasp         clasp;      /* its fields when sound; the crc alone when read in pieces */
    uint32_t      clasp_at;
    uint32_t      useful;     /* the bytes of its records, StitchHead to the Clasp's end, when sound (§4.2) */
    RibbonRecord* records;
    uint32_t      nrecords;
    uint64_t      lost;       /* bytes of its records its lost sectors took with them */
    bool          has_end;
    RibbonPlace   end;
} RibbonUnit;

/* A run of ground the files of the Tally name, as the index a next_quire is
 * weighed against holds it: its first block, and the furthest end of it and
 * of every run sorted before it. */
typedef struct {
    uint64_t start;
    uint64_t reach;
} RibbonGroundRun;

/* The ribbon walked from a slot's `at` (stitch_ribbon.py Ribbon). */
typedef struct {
    const SpineSlot* slot;
    bool             stood;       /* the slot the mount stands on: lines said, the scan made */
    RibbonUnit**     stitch;      /* the stitches sewn on, in order */
    uint32_t         nstitch;
    uint32_t         capstitch;
    RibbonQuire*     quires;      /* the Quires the chain runs through, and the next named */
    uint32_t         nquires;
    uint32_t         capquires;
    uint64_t         seq;         /* the last stitch taken, and its Clasp — unknown when it did not read */
    uint32_t         clasp;
    bool             clasp_known;
    RibbonQuire      quire;       /* the Quire the chain is in, and the next it may run on into */
    RibbonQuire      next;
    bool             has_end;
    RibbonPlace      end;         /* where the next unit begins */
    bool             lied;
    RibbonUnit*      torn;        /* the stitch of this history torn right after the end */
    RibbonUnit*      ahead;       /* the unit at the end of the stitch last taken, read to weigh it */
    bool             ahead_read;  /* ... and read: NULL then is none there (ribbon_collate.c RibbonKept) */
    RibbonUnit**     past;        /* every unit past the end the direct scan found, by seq */
    uint32_t         npast;
    uint64_t         seen_fid;    /* past every number a record read gives, applied or not */
    uint64_t         seen_tag;
    uint64_t         seen_bookmark;
    uint32_t         said[RIBBON_CODES];
    RibbonGroundRun* ground;      /* the files' ground a next_quire may not name, built when one is first weighed */
    uint64_t         nground;
    bool             ground_built;
} RibbonWalk;

/* ribbon_unit.c ------------------------------------------------------------ */

/* The place `k` sectors on from `p`, into `hop` past its Quire's end: false
 * when it would run off a Quire with nothing named after it. */
bool    RibbonPlaceOn(const RibbonPlace* p, uint64_t k, const RibbonQuire* hop, RibbonPlace* out);

/* Whether `q` lies where a Quire of the ribbon may (§3.7's table): in the data
 * run, begun on an atom. A Quire a slot or a unit's own StitchHead names is
 * read only then — no word of the medium sends a reader past the volume's
 * ground (ribbon_collate.c). */
bool    RibbonQuireLies(const RibbonQuire* q);

static inline uint64_t RibbonPlaceVlba(const RibbonPlace* p)
{
    return p->quire.block * VOLUME_BLOCK_SECTORS + p->off;
}

static inline uint64_t RibbonPlaceLeft(const RibbonPlace* p)
{
    return p->quire.blocks * VOLUME_BLOCK_SECTORS - p->off;
}

static inline bool RibbonQuireSame(const RibbonQuire* a, const RibbonQuire* b)
{
    return a->block == b->block && a->blocks == b->blocks;
}

/* The unit of `of` sectors and `seq` begun at `start`, running on into `hop`,
 * read and parsed: OK and *out (the caller's to let go), ERR_NO_MEMORY, or
 * the medium's refusal — *out NULL then. A unit no stitch is shaped like is
 * read not at all. */
error_t RibbonUnitRead(const RibbonPlace* start, uint32_t of, uint64_t seq, const RibbonQuire* hop,
                       RibbonUnit** out);

#ifdef CONFIG_VOLUME_TESTS
/* How many units RibbonUnitRead has read since boot (the stand's measure of
 * what a walk of the ribbon costs). */
uint64_t RibbonUnitsReadStand(void);
#endif

/* The unit of seq past `after` that begins at `place`, by the first sector
 * from there on through `hop` holding one (stitch.py FindUnit): *out NULL
 * when that sector belongs to a unit begun elsewhere, or there is none. */
error_t RibbonUnitFind(const RibbonPlace* place, const RibbonQuire* hop, uint64_t after, RibbonUnit** out);

/* The first sector from `place` to the end of its Quire holding a stitch of
 * seq past `after` (rule (б′), stitch_ribbon.py Ribbon._Further): *found, its
 * offset from `place` in *j and its head in *head — and, when it is the first
 * sector of its unit, that unit read in *out, running on into the next_quire
 * its own StitchHead names where that lies where one may. OK, or the medium's
 * or memory's refusal. */
error_t RibbonUnitAhead(const RibbonPlace* place, uint64_t after, uint64_t* j, BookSectorHead* head, bool* found,
                        RibbonUnit** out);

/* The StitchHead of the stitch whose first sector lies at `place`, that one
 * sector alone read: *found when it holds as the first sector of a stitch of
 * seq past `after` and a StitchHead reads in it — `u` then its start, seq,
 * `of`, head, and the Quire it runs on into (its own next_quire where that
 * lies where one may, else `hop`); nothing of its stream is kept. OK, or the
 * medium's or memory's refusal. */
error_t RibbonUnitHead(const RibbonPlace* place, const RibbonQuire* hop, uint64_t after, RibbonUnit* u,
                       bool* found);

void    RibbonUnitLetGo(RibbonUnit* u);

/* The seq of the stitch the unit's StitchHead names before it — 0 for none —
 * and false when its StitchHead does not read, or names a prev past the
 * unit's own seq: no stitch at all (stitch.py's Names() is below zero there),
 * so a seq named is always below its namer's. */
bool    RibbonUnitNames(const RibbonUnit* u, uint64_t* seq);

/* Every sector of the unit present, sealed at its seq — however many sums
 * hold (§3.9: a cut or rot, not a cut alone). */
bool    RibbonUnitWhole(const RibbonUnit* u);

/* ribbon_collate.c --------------------------------------------------------- */

/* Pass 1 from the slot the mount stands on (§4.5 п.5): its stitches sewn on,
 * its Quires, every line said and the direct scan made. OK and *out
 * (RibbonWalkLetGo), ERR_NO_MEMORY, or the medium's refusal. */
error_t RibbonCollate(const SpineSlot* slot, RibbonWalk** out);
void    RibbonWalkLetGo(RibbonWalk* w);

/* The ribbon from an older slot's `at` sewn on as pass 1 sews it, said to no
 * one and claiming nothing — the walk Facsimile replays a range's keys over
 * (facsimile.c). *out as RibbonCollate gives it; OK, ERR_NO_MEMORY, or the
 * medium's refusal. */
error_t RibbonCollateOlder(const SpineSlot* slot, RibbonWalk** out);

/* ribbon_scan.c ------------------------------------------------------------ */

/* The direct scan (§2.2) past the end of the walk's chain: every unit there,
 * the torn tail, and what each branch is, said. OK, ERR_NO_MEMORY, or the
 * medium's refusal. */
error_t RibbonScan(RibbonWalk* w);

/* ribbon_collate.c, the walk ------------------------------------------------ */

/* A walk begun at a slot's `at` — the slot stood on (`stood`: its lines said)
 * or another — its Quire and the next_quire it names weighed; *out the walk
 * (the caller's to let go), with no end when the slot's ribbon does not lie
 * where one may. OK or ERR_NO_MEMORY. */
error_t RibbonWalkBegun(const SpineSlot* slot, bool stood, RibbonWalk** out);

/* A Quire into the walk's list, once; and `q`, named by `who`, taken as the
 * next_quire when it may be (§3.7's table) — said when it may not. False with
 * no memory. */
bool    RibbonQuireAdd(RibbonWalk* w, const RibbonQuire* q);
bool    RibbonNamed(RibbonWalk* w, const RibbonQuire* q, const char* who);

/* The Quires the ribbon of a slot not stood on runs through, by its
 * StitchHeads alone (ribbon_heads.c, §16.2 п.12): *out the walk, its
 * `quires` the answer. OK, ERR_NO_MEMORY, or the medium's refusal. */
error_t RibbonQuiresOf(const SpineSlot* slot, RibbonWalk** out);

/* A line of the ribbon, said by its code: `[TagFS] ribbon CODE: ...`, counted
 * into the walk's and the mount's numbers. */
void    RibbonSay(RibbonWalk* w, RibbonCode code, const char* fmt, ...);

/* The seqs of `units` — "4, 5" — into `out`, as the census writes them. */
void    RibbonSeqsSaid(RibbonUnit* const* units, uint32_t n, char* out, size_t room);

/* ribbon_verify.c / ribbon_transcribe.c ------------------------------------- */

/* The fids whose records of a stitch are passed over: an Undone's, or a
 * verdict of the mount's own (§2.8, §3.9). */
typedef struct {
    uint64_t seq;
    uint64_t fid;
} RibbonSkip;

typedef struct {
    RibbonSkip* at;
    uint32_t    n;
    uint32_t    cap;
} RibbonSkipSet;

/* A skip added as it comes; the set sorted once, each kept once, before it is
 * asked (RibbonSkipSorted) — and, once sorted, a skip put in its place, so the
 * set stays sorted for the next ask (RibbonSkipPut). */
error_t RibbonSkipAdd(RibbonSkipSet* set, uint64_t seq, uint64_t fid);
void    RibbonSkipSorted(RibbonSkipSet* set);
error_t RibbonSkipPut(RibbonSkipSet* set, uint64_t seq, uint64_t fid);
bool    RibbonSkipHas(const RibbonSkipSet* set, uint64_t seq, uint64_t fid);

/* §4.5 п.6: the data the newest stitch put down (the newest two on a medium
 * that lied) held to the digests its records carry — a record an Undone on
 * the ribbon already passes over is not read; every file whose block did not
 * land put into `undone` (sorted) for that stitch, said, and into the verdict
 * the session's first stitch lays down (errata.h). OK, ERR_NO_MEMORY, or the
 * medium's refusal. */
error_t RibbonVerify(RibbonWalk* w, RibbonSkipSet* undone);

/* Pass 2 (§4.5 п.5, §3.7): every stitch sewn on replayed into the memory every
 * format shares; the Undones gathered first, the data checked, the files a
 * replay leaves standing on others' ground locked. OK, or the refusal — no
 * memory, the medium's — with *what the mount's line. */
error_t RibbonTranscribe(RibbonWalk* w, const char** what);

/* Facsimile's replay: the stitches of `w` to seq `upto` replayed as pass 2
 * replays them, but only their records of keys [lo, hi) of `tree` — FILES by
 * fid, TAGS by id — and of what a stitch gathers only its files and its
 * coins laid into memory: its Fallow, its digests and its Use Context are the
 * Tally's the mount stands on, which took them in; the Undones of the
 * stitches to `upto` alone, for the Tally written of it absorbs one past it
 * (§2.8). *replayed the stitches; `passed` (room bytes) empty when none was
 * passed over for the keys, else the first that was — refused for a record
 * that may be of them (RibbonRecordWithin) or for its own StitchHead or
 * Clasp, or its Undone would not hold — said as the mount's own replay says
 * it where it puts the volume under glass: the range is then not the state
 * at `upto` (facsimile.c, rule 4). OK, or memory's refusal. */
error_t RibbonFacsimile(RibbonWalk* w, uint64_t tree, uint64_t lo, uint64_t hi, uint64_t upto, uint32_t* replayed,
                        char* passed, size_t room);

/* Pass 2's own (ribbon_transcribe.c, ribbon_records.c) ---------------------- */

/* A file as a stitch leaves it: its record — NULL when it is gone. */
typedef struct {
    uint64_t    fid;
    FileRecord* rec;
} RibbonStaged;

/* A tag a stitch coins — or retires, its flags RETIRED — and whether the registry can hold it
 * (BookTagHeld). */
typedef struct {
    uint64_t id;
    TallyTag tag;
    bool     held;
} RibbonCoin;

/* A digest a stitch's record carries for a block it puts in. */
typedef struct {
    uint64_t block;
    uint64_t digest;
} RibbonDigest;

/* A file whose flags reach past the 32 bits memory holds, and those bits. */
typedef struct {
    uint64_t fid;
    uint32_t high;
} RibbonHigh;

/* Everything one stitch changes, gathered before it is applied. */
typedef struct {
    const RibbonUnit* u;
    RibbonStaged*     files;      /* rising by fid */
    uint32_t          nfiles, capfiles;
    RibbonCoin*       coins;
    uint32_t          ncoins, capcoins;
    Strip*            fallow;     /* inside the volume */
    uint64_t          nfallow, capfallow;
    uint64_t          reach;      /* past the farthest block the ground it let go names, as a record names it */
    RibbonDigest*     digests;
    uint64_t          ndigests, capdigests;
    RibbonHigh*       high;       /* the high bits of the flags its records leave, 0 kept too */
    uint32_t          nhigh, caphigh;
    bool              use_set;
    const uint8_t*    use;
    uint64_t          use_len;
    bool              rot;        /* a record of it did not fit its file (the rule of rot): gathered again */
    char              refused[200];   /* why the stitch is not applied; "" while it is */
} RibbonStage;

/* What the whole replay keeps across its stitches. */
typedef struct {
    RibbonWalk*     w;
    RibbonSkipSet   undone;
    uint64_t*       refused;      /* stitches not applied for their Undone, by seq: rising as they come */
    uint32_t        nrefused, caprefused;
    /* The three below are sorted once the last stitch is replayed (RibbonSettled). */
    uint64_t*       touched;      /* fids a stitch applied changed */
    uint32_t        ntouched, captouched;
    uint64_t*       mismatched;   /* fids rolled back by the rule of rot */
    uint32_t        nmismatched, capmismatched;
    uint64_t*       ext;          /* fids a run with an extension was put in for */
    uint32_t        next, capext;
    RibbonHigh*     high;
    uint32_t        nhigh, caphigh;
    bool            use_set;
    const uint8_t*  use;
    uint64_t        use_len;
    uint64_t        only;         /* Facsimile's: the tree whose keys [only_lo, only_hi) alone are
                                   * replayed (RibbonFacsimile); 0 for every record */
    uint64_t        only_lo, only_hi;
    uint32_t        passed;       /* Facsimile's: stitches it passed over — refused, or an Undone
                                   * that does not hold — where the mount's replay puts the
                                   * volume under glass */
    char            passed_why[256];  /* Facsimile's: the first of them, in the mount's words */
} RibbonReplay;

/* Lists that grow: false with no memory. A list is added to as it comes and
 * sorted once, each value kept once (RibbonListSorted), before it is asked;
 * RibbonListHas asks a rising list. */
bool    RibbonGrow(void** at, uint64_t n, uint64_t* cap, size_t size);
bool    RibbonListAdd(uint64_t** list, uint32_t* n, uint32_t* cap, uint64_t v);
void    RibbonListSorted(uint64_t* list, uint32_t* n);
bool    RibbonListHas(const uint64_t* list, uint32_t n, uint64_t v);

/* The stitch's stage: why it is not applied (the first reason kept), the file
 * as it leaves it so far (the stage's, borrowed — NULL when gone — or the
 * mirror's, *held, which the caller lets go), a file put (one reference, the
 * stage's from here; NULL for gone), and DATA ground let go — `once` keeping
 * a run let go already from being let go twice. */
void        RibbonRefuse(RibbonStage* st, const char* fmt, ...);
FileRecord* RibbonFileNow(RibbonStage* st, uint64_t fid, bool* held);
error_t     RibbonStagePut(RibbonStage* st, uint64_t fid, FileRecord* rec);
error_t     RibbonStageFallow(RibbonStage* st, uint64_t start, uint64_t count, bool once);

/* A file's flags past the 32 bits memory holds: kept by the stage, laid with
 * it (0 lets them go), and the whole of them for a record — the stage's
 * first, when there is one. */
error_t     RibbonHighSet(RibbonStage* st, uint64_t fid, uint32_t high);
uint64_t    RibbonFlagsOf(const RibbonReplay* rp, const RibbonStage* st, const FileRecord* rec);

/* A record through the rule of its kind into the stage (ribbon_records.c),
 * and the stitch's Clasp.fresh held to its records: OK, or ERR_NO_MEMORY —
 * a refusal is the stage's. */
error_t     RibbonRecordApplied(RibbonReplay* rp, RibbonStage* st, const RibbonRecord* r);
/* Whether record `r` of `u` may be of a key Facsimile replays — a file's in
 * [lo, hi) of FILES, a tag's in [lo, hi) of TAGS — so its replay applies it:
 * its key among them, or not known — a body of a kind that names the tree's
 * keys that does not read, a kind this reader does not know and may not step
 * over — which, applied, refuses the stitch (ribbon_records.c). */
bool    RibbonRecordWithin(const RibbonUnit* u, const RibbonRecord* r, uint64_t tree, uint64_t lo, uint64_t hi);
void        RibbonFreshHeld(RibbonStage* st);

/* The fresh blocks a sound stitch's Borns and Shapeds name (Clasp.fresh,
 * §2.7) — a record that does not read names none: false when their sum
 * passes 2^64 - 1, which no Clasp can say. */
bool        RibbonFreshNamed(const RibbonUnit* u, uint64_t* named);

#endif /* TAGFS_MOUNT_RIBBON_H */
