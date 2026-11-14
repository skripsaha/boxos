#ifndef TAGBOOT_LECTERN_BOOK_H
#define TAGBOOT_LECTERN_BOOK_H

/*
 * What crosses between the files of the Lectern (tagboot_lectern.h), and
 * nothing else:
 *
 *   tagboot_lectern.c         memory and lines; a unit and a node read off
 *                             the medium; the slot stood on; the DIGESTS tree
 *                             walked down to a block's digest; Attest
 *   tagboot_lectern_tree.c    a tree's leaves walked in key order: the files
 *                             of FILES kept in a role
 *   tagboot_lectern_ribbon.c  the stitches past the slot's `at`, sewn on by
 *                             rules (а)/(б)/(б′)/(в)
 *   tagboot_lectern_replay.c  their records replayed for the files followed,
 *                             and the digests they carry
 *   tagboot_succession.c      the kernels tried in turn, and the choice
 *
 * The loader's integer types are uefi.h's (it builds -nostdinc, with no
 * <stdint.h>); the host stand's are the host's.
 */

#if __STDC_HOSTED__
#include <stdint.h>
#else
#include "uefi.h"
#endif

#include "tagboot_lectern.h"

/* ------------------------------------------------------------------------- */
/* Lines                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    char     text[LECTERN_LINE_BYTES];
    uint32_t at;
} LecternLine;

void LineStart(LecternLine *l);
void LinePut(LecternLine *l, const char *s);
void LineNum(LecternLine *l, uint64_t v);

/* The role of a file, from its flags: TALLY_FILE_ROLE_KERNEL, _BOOT, _SYSTEM,
 * or 0 for a file the system keeps in none — a file's tags are its user's,
 * and the loader reads none of them. A kernel is looked for in the order of
 * the roles, the larger first (§16.7 п.3). */
static inline uint64_t LecternRoleOf(uint64_t flags)
{
    return flags & TALLY_FILE_ROLE_MASK;
}

/* ------------------------------------------------------------------------- */
/* What the book is read into                                                */
/* ------------------------------------------------------------------------- */

/* A node read whole: its unit in `room` (`bytes` the desk gave), its stream
 * laid over the room's front (`stream` bytes), its head, where its items
 * begin, and the seq it was sealed at. */
typedef struct {
    uint8_t *room;
    uint64_t bytes;
    uint32_t stream;
    NodeHead head;
    uint32_t items_at;
    uint64_t seq;
} LecternNode;

/* A rung of a path down a tree: the node held at its level, or the range at
 * its level no copy of whose node holds. */
typedef struct {
    LecternNode node;      /* node.room NULL: no node held */
    TallyRange  range;
    int         rotten;
} LecternRung;

typedef enum { NODE_READ, NODE_ROTTEN, NODE_NO_ROOM } LecternNodeSays;

/* A run of a file: a hole of `count` blocks, or `count` blocks from `start`;
 * `carried` RibbonRunCarried's answer. */
typedef struct {
    uint64_t start;
    uint64_t count;
    uint32_t hole;
    int      carried;
} LecternRun;

/* A file the Lectern follows: its gen, size and flags, its runs, and the
 * Slips laid on its last block — `tail` NULL for none, else a block of their
 * bytes and a block's bits more saying which are laid. `gone` a file followed
 * that a Quitclaim took. */
typedef struct {
    uint64_t    fid;
    uint64_t    gen;
    uint64_t    size;
    uint64_t    flags;
    LecternRun *runs;
    uint64_t    nruns, capruns;
    uint8_t    *tail;
    int         gone;
} LecternFile;

#define LECTERN_TAIL_BYTES (VOLUME_BLOCK_BYTES + VOLUME_BLOCK_BYTES / 8u)

/* A Quire of the ribbon — none when blocks is 0 — and a sector of it. */
typedef struct {
    uint64_t block;
    uint64_t blocks;
} LecternQuire;

typedef struct {
    LecternQuire quire;
    uint64_t     off;
} LecternPlace;

/* A record of a stitch's stream: its kind, whether it may be stepped over,
 * where it and its body lie. */
typedef struct {
    uint64_t kind;
    int      ignorable;
    uint32_t at;
    uint32_t body;
    uint32_t len;
} LecternRecord;

/* A unit of the ribbon begun at `start` (the kernel's RibbonUnit, stitch.py
 * Unit): its seq and `of`, which of its sectors hold as its own, and what
 * reads of it — whole (`sound`), or in pieces by `lead`. */
typedef struct {
    LecternPlace   start;
    LecternQuire   hop;         /* the Quire it runs on into past its own's end */
    uint64_t       seq;
    uint32_t       of;
    int            shape;       /* RIBBON_SHAPE_KEPT, or none of it is read */
    uint8_t       *valid;
    uint16_t      *leads;
    uint8_t       *stream;      /* of * 488 bytes, zeros where a sector does not hold */
    uint32_t       size;
    int            sound;
    int            head_read;
    StitchHead     head;
    uint32_t       longer;      /* bytes past the known fields of its StitchHead and Clasp */
    int            clasp_read;
    Clasp          clasp;
    LecternRecord *records;
    uint32_t       nrecords, caprecords;
    int            has_end;
    LecternPlace   end;
} LecternStitch;

/* A file's records of a stitch passed over: an Undone's, or the rule of rot. */
typedef struct {
    uint64_t seq;
    uint64_t fid;
} LecternSkip;

/* The digest a record of the ribbon carries for a block it put in. */
typedef struct {
    uint64_t block;
    uint64_t digest;
} LecternDigest;

typedef struct {
    const LecternDesk *desk;
    uint64_t           lane;          /* the digests' seed: lane 0 of the volume's uuid */
    int                starved;       /* the desk gave no room for something asked */
    /* The slot stood on, its stream held while the book is read. */
    int                stood;
    LecternLine        why;           /* why none is */
    uint8_t           *slot_room;
    uint32_t           slot_index;
    uint64_t           slot_seq;
    SpineHead          head;
    SpineRibbon        ribbon;
    SpineRoot          roots[TALLY_DIGESTS + 1u];
    uint32_t           rooted;        /* a bit of each tree whose authoritative root it names */
    /* The DIGESTS tree, walked down: rungs[level], 0 the leaves, `top` the root. */
    LecternRung       *rungs;
    uint64_t           top;
    /* The nodes said read past a copy that did not hold, by their first block. */
    uint64_t          *said;
    uint64_t           nsaid, capsaid;
    /* The ribbon. */
    LecternStitch    **stitches;
    uint64_t           nstitches, capstitches;
    LecternSkip       *skip;
    uint64_t           nskip, capskip;
    uint64_t          *refused;       /* the stitches not applied */
    uint64_t           nrefused, caprefused;
    uint64_t          *interest;      /* files a Labeled puts in a role */
    uint64_t           ninterest, capinterest;
    /* The files followed, by fid, rising; the rotten ranges of FILES. */
    LecternFile       *files;
    uint64_t           nfiles, capfiles;
    TallyRange        *rotten;
    uint64_t           nrotten, caprotten;
    /* The digests the ribbon carries for the blocks of the kernels to try, by block. */
    LecternDigest     *overlay;
    uint64_t           noverlay, capoverlay;
} LecternBook;

/* ------------------------------------------------------------------------- */
/* tagboot_lectern.c                                                         */
/* ------------------------------------------------------------------------- */

void    LecternSay(const LecternBook *b, const LecternLine *l);
void   *LecternTake(LecternBook *b, uint64_t bytes);
void    LecternGive(const LecternBook *b, void *room, uint64_t bytes);
int     LecternGrow(LecternBook *b, void **at, uint64_t n, uint64_t *cap, uint64_t size);
void    LecternZero(void *p, uint64_t n);
void    LecternCopy(void *to, const void *from, uint64_t n);
int     LecternListHas(const uint64_t *list, uint64_t n, uint64_t v);
int     LecternListAdd(LecternBook *b, uint64_t **list, uint64_t *n, uint64_t *cap, uint64_t v);

/* `sectors` sectors from `vlba` into `into`, each the medium will not give
 * read as zeros — no sector of the book. */
void    LecternSectors(LecternBook *b, uint64_t vlba, uint32_t sectors, uint8_t *into);

/* The node `ptr` names into *n from the first of its copies that holds as a
 * node of the range `r` (§2.5) and is a node of `tree` and `level`, the copies
 * before it that did not hold in *failed — said, once a node, by
 * LecternCopiesSay. */
LecternNodeSays LecternNodeRead(LecternBook *b, uint64_t tree, uint64_t level, const BookPtr *ptr,
                                const TallyRange *r, LecternNode *n, uint32_t *failed);
void    LecternNodeGive(const LecternBook *b, LecternNode *n);
void    LecternCopiesSay(LecternBook *b, uint64_t tree, uint64_t level, const BookPtr *ptr, uint32_t failed);

/* The book stood on, or b->why: 1 stood, 0 not, -1 no room. */
int     LecternStand(LecternBook *b);

/* The kernel `who` — `blocks` blocks of its runs in file order, loaded at
 * `image` — held to the book, the digests the ribbon carries first when
 * `overlay`; its verdict said and in *verdict. */
AttestSays LecternAttest(LecternBook *b, const char *who, const LecternRun *runs, uint64_t nruns,
                         uint64_t blocks, const uint8_t *image, int overlay, LecternLine *verdict);

void    LecternClose(LecternBook *b);

/* ------------------------------------------------------------------------- */
/* tagboot_lectern_tree.c                                                    */
/* ------------------------------------------------------------------------- */

/* The files FILES holds that are kept in a role or that the ribbon puts in
 * one (b->interest). 1, or 0 no room. */
int     LecternFiles(LecternBook *b);

LecternFile *LecternFileOf(LecternBook *b, uint64_t fid);
/* A file put into its place by fid (*f taken over); 0 no room. */
int     LecternFilePut(LecternBook *b, const LecternFile *f);
/* A file's arrays given back. */
void    LecternFileGive(const LecternBook *b, LecternFile *f);
/* `to` a deep copy of `from`; 0 no room. */
int     LecternFileCopy(LecternBook *b, LecternFile *to, const LecternFile *from);

/* ------------------------------------------------------------------------- */
/* tagboot_lectern_ribbon.c                                                  */
/* ------------------------------------------------------------------------- */

/* The stitches past the slot's `at`, sewn on into b->stitches. 1, or 0 no room. */
int     LecternSew(LecternBook *b);
void    LecternStitchGive(const LecternBook *b, LecternStitch *u);

/* ------------------------------------------------------------------------- */
/* tagboot_lectern_replay.c                                                  */
/* ------------------------------------------------------------------------- */

/* Before the files: the files the ribbon puts in a role. */
void    LecternBefore(LecternBook *b);
/* The Undones, then each stitch's records for the files followed. 1, or 0. */
int     LecternReplay(LecternBook *b);
/* The digests the ribbon carries of every block of the files `kernels`. 1, or 0. */
int     LecternOverlay(LecternBook *b, LecternFile *const *kernels, uint64_t n);
/* The digest of block `v` the ribbon carries: 1 with it in *digest, or 0. */
int     LecternOverlaid(const LecternBook *b, uint64_t v, uint64_t *digest);

#endif /* TAGBOOT_LECTERN_BOOK_H */
