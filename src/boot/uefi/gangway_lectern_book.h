#ifndef GANGWAY_LECTERN_BOOK_H
#define GANGWAY_LECTERN_BOOK_H

/*
 * What crosses between the files of the Lectern (gangway_lectern.h), and
 * nothing else:
 *
 *   gangway_lectern.c         memory and lines; a unit and a node read off
 *                             the medium; the slot stood on; the DIGESTS tree
 *                             walked down to a block's digest; Attest
 *   gangway_lectern_tree.c    a tree's leaves walked in key order: the files
 *                             of FILES kept in a role
 *   gangway_lectern_ribbon.c  the stitches past the slot's `at`, sewn on by
 *                             rules (а)/(б)/(б′)/(в)
 *   gangway_lectern_replay.c  their records replayed for the files followed,
 *                             and the digests they carry
 *   gangway_succession.c      the kernels tried in turn, and the choice
 *   gangway_lectern_called.c  the file asked for by its name, read and held
 *
 * The loader's integer types are uefi.h's (it builds -nostdinc, with no
 * <stdint.h>); the host stand's are the host's.
 */

#if __STDC_HOSTED__
#include <stddef.h>
#include <stdint.h>
#else
#include "uefi.h"
#endif

#include "boxos_scene.h"      /* BOX_SCENE_SUFFIX */
#include "gangway_lectern.h"

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

/* Which of the `n` files the loader asks for by name (LecternCalled) the name
 * `name` (`len` bytes, not NUL-terminated) is: 1 + its place among them, 0
 * none. */
static inline int LecternNamed(const LecternCalled *called, uint32_t n, const uint8_t *name, uint64_t len)
{
    for (uint32_t k = 0; k < n; k++) {
        const char *asked = called[k].name;
        uint64_t    i     = 0;
        while (i < len && asked[i] && (uint8_t)asked[i] == name[i]) i++;
        if (i == len && asked[i] == '\0') return (int)k + 1;
    }
    return 0;
}

/* The role of a file, from its flags: TALLY_FILE_ROLE_KERNEL, _BOOT, _SYSTEM,
 * or 0 for a file the system keeps in none — a file's tags are its user's,
 * and the loader reads none of them but those a person's settings ask a
 * scene by. A kernel is looked for in the order of the roles, the larger
 * first (§16.7 п.3). */
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
 * that a Quitclaim took; `called` 1 + which of the files the loader asks for
 * by name (LecternCalled) it is called as the book and the ribbon leave it, 0
 * none. Its name — no more of it than a line holds — and the ids of the tags
 * it wears, rising, as the book and the ribbon leave them: what a scene is
 * picked by when a person's settings ask by tags. */
typedef struct {
    uint64_t    fid;
    uint64_t    gen;
    uint64_t    size;
    uint64_t    flags;
    LecternRun *runs;
    uint64_t    nruns, capruns;
    uint8_t    *tail;
    int         gone;
    int         called;
    char        name[LECTERN_LINE_BYTES];
    uint32_t    name_len;
    uint64_t   *tags;
    uint64_t    ntags, captags;
} LecternFile;

/* Whether `name` (`len` bytes) is a scene's: it ends in BOX_SCENE_SUFFIX. */
static inline int LecternSceneNamed(const uint8_t *name, uint64_t len)
{
    const uint64_t suffix = sizeof(BOX_SCENE_SUFFIX) - 1u;
    if (len <= suffix) return 0;
    for (uint64_t i = 0; i < suffix; i++)
        if (name[len - suffix + i] != (uint8_t)BOX_SCENE_SUFFIX[i]) return 0;
    return 1;
}

/* A tag as a person spells it — "key" or "key:value", blanks around it not
 * part of it, the first ':' splitting the two and "key:" "key" (tag_spelling.h)
 * — in place in the word it was read from. */
typedef struct {
    const uint8_t *key, *value;
    uint64_t       key_len, value_len;
    int            has_value;
} LecternSpelling;

static inline void LecternSpell(const char *word, LecternSpelling *s)
{
    const uint8_t *w = (const uint8_t *)word;
    uint64_t       from = 0, to = 0, colon;
    while (w[to]) to++;
    while (from < to && (w[from] == ' ' || w[from] == '\t')) from++;
    while (to > from && (w[to - 1u] == ' ' || w[to - 1u] == '\t')) to--;
    for (colon = from; colon < to && w[colon] != ':'; colon++) {}
    s->key       = w + from;
    s->key_len   = colon - from;
    s->value     = colon < to ? w + colon + 1u : w + to;
    s->value_len = colon < to ? to - colon - 1u : 0;
    s->has_value = s->value_len != 0;
}

/* Whether the book's tag `t` is the one the person spelled in `s`: a retired
 * tag is none, and neither is a program's own (OWNED) — the settings speak the
 * person's words, and a program's tag of the same text is another word. */
static inline int LecternSpells(const LecternSpelling *s, const TallyTag *t)
{
    if ((t->flags & (TALLY_TAG_RETIRED | TALLY_TAG_OWNED)) || t->key_len != s->key_len ||
        !(t->flags & TALLY_TAG_HAS_VALUE) != !s->has_value || (s->has_value && t->value_len != s->value_len))
        return 0;
    for (uint64_t i = 0; i < s->key_len; i++)
        if (t->key[i] != s->key[i]) return 0;
    for (uint64_t i = 0; s->has_value && i < s->value_len; i++)
        if (t->value[i] != s->value[i]) return 0;
    return 1;
}

/* The file's name kept, no more of it than a line holds. */
static inline void LecternFileNamed(LecternFile *f, const uint8_t *name, uint64_t len)
{
    f->name_len = len < LECTERN_LINE_BYTES - 1u ? (uint32_t)len : LECTERN_LINE_BYTES - 1u;
    for (uint32_t i = 0; i < f->name_len; i++) f->name[i] = (char)name[i];
    f->name[f->name_len] = '\0';
}

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
    LecternCalled     *called;        /* the files asked for by name, ncalled of them */
    uint32_t           ncalled;
    int                booked;        /* the book past its Tally read (BookRead): 1, -1 that failed, 0 not asked */
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
    uint64_t          *interest;      /* files a Labeled puts in a role, or names as a file asked for or a scene */
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
/* gangway_lectern.c                                                         */
/* ------------------------------------------------------------------------- */

void    LecternSay(const LecternBook *b, const LecternLine *l);
void   *LecternTake(LecternBook *b, uint64_t bytes);
void    LecternGive(const LecternBook *b, void *room, uint64_t bytes);
int     LecternGrow(LecternBook *b, void **at, uint64_t n, uint64_t *cap, uint64_t size);
void    LecternZero(void *p, uint64_t n);
void    LecternCopy(void *to, const void *from, uint64_t n);
int     LecternListHas(const uint64_t *list, uint64_t n, uint64_t v);
int     LecternListAdd(LecternBook *b, uint64_t **list, uint64_t *n, uint64_t *cap, uint64_t v);

/* The most sectors one command asks of the medium: a mebibyte. A command of
 * the firmware costs about as much for one block as for many, so the Lectern
 * reads in long ones — and in none longer, for some firmware stumbles on a
 * read past this (the Linux EFI stub reads its files a mebibyte at a time for
 * the same reason). */
#define LECTERN_READ_MOST 2048u

/* `sectors` sectors from `vlba` into `into`, in commands of LECTERN_READ_MOST
 * at the most, each sector the medium will not give read as zeros — no sector
 * of the book. */
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

/* `count` blocks of a run from volume block `v` — none for a hole, its zeros —
 * read into `into` in as few commands as LECTERN_READ_MOST allows, a command
 * the medium refuses read again a block at a time: the blocks read, `count`,
 * or the place of the first the medium will not give. */
uint64_t LecternRunRead(const LecternBook *b, int hole, uint64_t v, uint64_t count, uint8_t *into);
/* The Slips `tail` (a file's, NULL none) laid on its last block, at `block`. */
void    LecternTailLaid(const uint8_t *tail, uint8_t *block);

/* The kernel `who` — `blocks` blocks of its runs in file order, loaded at
 * `image` — held to the book, the digests the ribbon carries first when
 * `overlay`; its verdict said and in *verdict. */
AttestSays LecternAttest(LecternBook *b, const char *who, const LecternRun *runs, uint64_t nruns,
                         uint64_t blocks, const uint8_t *image, int overlay, LecternLine *verdict);

void    LecternClose(LecternBook *b);

/* ------------------------------------------------------------------------- */
/* gangway_lectern_tree.c                                                    */
/* ------------------------------------------------------------------------- */

/* The files FILES holds that are kept in a role, that the ribbon puts in one
 * or names as the file asked for (b->interest), or that are called so. 1, or
 * 0 no room. */
int     LecternFiles(LecternBook *b);

LecternFile *LecternFileOf(LecternBook *b, uint64_t fid);
/* The `n` tag ids a list of the book's (TallyIdsPut) of `bytes` holds, as the
 * file's tags: 1, or 0 no room. */
int          LecternFileTags(LecternBook *b, LecternFile *f, const uint8_t *ids, uint32_t bytes, uint64_t n);
/* The ids of the tags `word` spells — "key" or "key:value", the first ':'
 * splitting them — as the slot's TAGS and the ribbon's Coined leave them, a
 * retired one none, into the rising list *ids: 1, or 0 no room. */
int          LecternTagIds(LecternBook *b, const char *word, uint64_t **ids, uint64_t *n, uint64_t *cap);
/* The ribbon's Coined, in its order through the stitches it applies: an id
 * given the tag `s` spells into *ids, one given another taken out. 1, or 0 no
 * room. */
int          LecternCoinedIds(LecternBook *b, const LecternSpelling *s, uint64_t **ids, uint64_t *n, uint64_t *cap);
/* A file put into its place by fid (*f taken over); 0 no room. */
int     LecternFilePut(LecternBook *b, const LecternFile *f);
/* A file's arrays given back. */
void    LecternFileGive(const LecternBook *b, LecternFile *f);
/* `to` a deep copy of `from`; 0 no room. */
int     LecternFileCopy(LecternBook *b, LecternFile *to, const LecternFile *from);

/* ------------------------------------------------------------------------- */
/* gangway_lectern_called.c                                                  */
/* ------------------------------------------------------------------------- */

/* The file b->called[which] names, read into it as LecternCalled says — the
 * book's files read first (b->booked) — or why not, said. */
void    LecternCalledTake(LecternBook *b, uint32_t which);

/* ------------------------------------------------------------------------- */
/* gangway_lectern_ribbon.c                                                  */
/* ------------------------------------------------------------------------- */

/* The stitches past the slot's `at`, sewn on into b->stitches. 1, or 0 no room. */
int     LecternSew(LecternBook *b);
void    LecternStitchGive(const LecternBook *b, LecternStitch *u);

/* ------------------------------------------------------------------------- */
/* gangway_lectern_replay.c                                                  */
/* ------------------------------------------------------------------------- */

/* Before the files: the files the ribbon puts in a role, or names as the
 * file asked for. */
void    LecternBefore(LecternBook *b);
/* The Undones, then each stitch's records for the files followed. 1, or 0. */
int     LecternReplay(LecternBook *b);
/* The digests the ribbon carries of every block of the files `kernels` — the
 * kernels to try, or the file asked for by name. 1, or 0 no room. */
int     LecternOverlay(LecternBook *b, LecternFile *const *kernels, uint64_t n);
/* The digest of block `v` the ribbon carries: 1 with it in *digest, or 0. */
int     LecternOverlaid(const LecternBook *b, uint64_t v, uint64_t *digest);

#endif /* GANGWAY_LECTERN_BOOK_H */
