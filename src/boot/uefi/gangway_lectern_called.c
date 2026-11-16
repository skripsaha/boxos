/*
 * A file the loader asks the volume for (LecternCalled) — beside the kernel,
 * in the same reading of the book. By its name: among the files the Lectern
 * follows (gangway_lectern_tree.c, _replay.c follow those called so, as the
 * book and the ribbon leave their names), those the book does not keep deleted,
 * the first made — the least fid, as BoxLib takes a program's settings file —
 * said when there are more. By the tags a person's settings ask for, when the
 * desk asks so (LecternDesk.ask): among the scenes the Lectern follows (every
 * file called *.scene, its tags as the book and the ribbon leave them), those
 * the book does not keep deleted that wear, for each tag asked, a tag of the
 * ids TAGS and the ribbon's Coined give it (LecternTagIds), the one `pick`
 * falls on, counted by fid — said, with how many there were. What may be read of it — no run of an extension
 * this loader does not read, no ground outside the data run, no more bytes
 * than asked — its blocks read as the medium gives them and held to the book
 * as a kernel's are (LecternAttest, the digest the ribbon's record of a block
 * carries before DIGESTS), the Slips of the ribbon laid on its last. One
 * found broken is not handed over. Line for line volume-read's bootread.py
 * Called.
 */

#include "gangway_lectern_book.h"

static uint64_t BlocksOf(uint64_t bytes)
{
    return bytes / VOLUME_BLOCK_BYTES + (bytes % VOLUME_BLOCK_BYTES != 0);
}

/* "<name> <what>[n<then>[m<last>]]", said. */
static void Said(LecternBook *b, const LecternLine *who, const char *what, uint64_t n, const char *then, uint64_t m,
                 const char *last)
{
    LecternLine l;
    LineStart(&l);
    LinePut(&l, who->text);
    LinePut(&l, what);
    if (then) {
        LineNum(&l, n);
        LinePut(&l, then);
    }
    if (last) {
        LineNum(&l, m);
        LinePut(&l, last);
    }
    LecternSay(b, &l);
}

/* The file followed that is called as asked file `which` is and not kept
 * deleted, the first made, and how many are. */
static LecternFile *FirstCalled(LecternBook *b, uint32_t which, uint64_t *count)
{
    LecternFile *first = (LecternFile *)0;
    *count = 0;
    for (uint64_t i = 0; i < b->nfiles; i++) {
        LecternFile *f = &b->files[i];
        if (f->gone || f->called != (int)which + 1 || (f->flags & TALLY_FILE_DELETED)) continue;
        if (!first) first = f;
        (*count)++;
    }
    return first;
}

/* The tags asked, as a line says them: "boot, light". */
static void TagsPut(LecternLine *l, const LecternCalled *c)
{
    for (uint32_t k = 0; k < c->ntags; k++) {
        if (k) LinePut(l, ", ");
        LinePut(l, c->tags[k]);
    }
}

/* The scene followed that the book does not keep deleted and that wears a tag
 * of every one asked, the one `pick` falls on counted by fid, and how many
 * do; NULL when none does, or with no room (b->starved). */
static LecternFile *Picked(LecternBook *b, const LecternCalled *c, uint64_t *count)
{
    const uint64_t words  = c->ntags;
    uint64_t     **ids    = (uint64_t **)LecternTake(b, words * sizeof(uint64_t *));
    uint64_t      *n      = (uint64_t *)LecternTake(b, words * sizeof(uint64_t));
    uint64_t      *cap    = (uint64_t *)LecternTake(b, words * sizeof(uint64_t));
    LecternFile   *picked = (LecternFile *)0;
    int            room   = ids && n && cap;

    *count = 0;
    for (uint64_t k = 0; room && k < words; k++) {
        ids[k] = (uint64_t *)0;
        n[k] = cap[k] = 0;
    }
    for (uint64_t k = 0; room && k < words; k++) room = LecternTagIds(b, c->tags[k], &ids[k], &n[k], &cap[k]);
    for (int round = 0; room && round < 2 && (round == 0 || *count); round++) {
        uint64_t seen = 0, at = round ? c->pick % *count : 0;
        for (uint64_t i = 0; i < b->nfiles && !picked; i++) {
            const LecternFile *f     = &b->files[i];
            int                wears = !f->gone && !(f->flags & TALLY_FILE_DELETED) &&
                        LecternSceneNamed((const uint8_t *)f->name, f->name_len);
            for (uint64_t k = 0; wears && k < words; k++) {
                int one = 0;
                for (uint64_t j = 0; !one && j < n[k]; j++) one = LecternListHas(f->tags, f->ntags, ids[k][j]);
                wears = one;
            }
            if (!wears) continue;
            if (round == 0) (*count)++;
            else if (seen++ == at) picked = &b->files[i];
        }
    }
    for (uint64_t k = 0; ids && n && cap && k < words; k++) LecternGive(b, ids[k], cap[k] * sizeof(uint64_t));
    LecternGive(b, ids, words * sizeof(uint64_t *));
    LecternGive(b, n, words * sizeof(uint64_t));
    LecternGive(b, cap, words * sizeof(uint64_t));
    return picked;
}

/* What of the file may be read: 1, or 0 refused, said. */
static int Readable(LecternBook *b, const LecternLine *who, const LecternFile *f, uint64_t most)
{
    const char *none = (const char *)0;
    uint64_t    low = b->desk->frame.data_block, end = b->desk->block_end;
    for (uint64_t r = 0; r < f->nruns; r++) {
        if (f->runs[r].carried == RUN_CARRIED_NOT) {
            Said(b, who, " cannot be read: a run of it carries an extension this loader does not read (CODEC or COPY, "
                         "§8)", 0, none, 0, none);
            return 0;
        }
        if (!f->runs[r].hole &&
            (f->runs[r].start < low || f->runs[r].start > end || f->runs[r].count > end - f->runs[r].start)) {
            Said(b, who, " cannot be read: it names ground outside the data run", 0, none, 0, none);
            return 0;
        }
    }
    if (f->size > most) {
        Said(b, who, " cannot be read: it is ", f->size, " bytes, more than the ", most, " this loader reads");
        return 0;
    }
    return 1;
}

/* The file's blocks read in file order into the desk's room, a run at a
 * time: the room, or NULL — no room, or a block will not read — said. */
static uint8_t *Read(LecternBook *b, const LecternLine *who, const LecternFile *f, uint64_t blocks)
{
    const char *none = (const char *)0;
    uint8_t    *room = b->desk->take(b->desk->ctx, blocks * VOLUME_BLOCK_BYTES);
    uint64_t    k    = 0;

    if (!room) {
        Said(b, who, " cannot be read: the loader has no room for it", 0, none, 0, none);
        return room;
    }
    for (uint64_t r = 0; r < f->nruns && k < blocks; r++) {
        uint64_t n   = f->runs[r].count < blocks - k ? f->runs[r].count : blocks - k;
        uint64_t got = LecternRunRead(b, (int)f->runs[r].hole, f->runs[r].start, n, room + k * VOLUME_BLOCK_BYTES);
        k += got;
        if (got == n) continue;
        b->desk->give(b->desk->ctx, room, blocks * VOLUME_BLOCK_BYTES);
        Said(b, who, " cannot be read: its block ", k, " (volume block ", f->runs[r].start + got, ") will not read");
        return (uint8_t *)0;
    }
    return room;
}

void LecternCalledTake(LecternBook *b, uint32_t which)
{
    LecternCalled *c    = &b->called[which];
    const char    *none = (const char *)0;
    LecternLine  who, verdict;
    LecternFile *f;
    uint64_t     count, blocks;

    if (b->desk->ask) b->desk->ask(b->desk->ctx, c, which);
    c->says  = CALLED_NONE;
    c->bytes = (uint8_t *)0;
    c->room  = 0;
    c->size  = 0;
    c->fid   = 0;
    LineStart(&who);
    if (c->ntags) {
        LinePut(&who, "the scene carrying ");
        TagsPut(&who, c);
    } else {
        LinePut(&who, c->name);
    }
    if (!b->stood) {
        LecternLine l;
        LineStart(&l);
        LinePut(&l, who.text);
        LinePut(&l, " is not read: ");
        LinePut(&l, b->why.text);
        LecternSay(b, &l);
        return;
    }
    if (b->booked != 1) {
        Said(b, &who, " is not read: the loader had no room to read the book's files", 0, none, 0, none);
        return;
    }
    if (c->ntags) {
        LecternLine l;
        f = Picked(b, c, &count);
        if (b->starved) {
            Said(b, &who, " is not read: the loader had no room to pick it", 0, none, 0, none);
            return;
        }
        LineStart(&l);
        if (!f) {
            /* Nothing the person asked for: the file by its name instead,
             * never no file at all for a word mistyped. */
            LinePut(&l, "no scene on this volume carries ");
            TagsPut(&l, c);
            LinePut(&l, " — ");
            LinePut(&l, c->name);
            LinePut(&l, " is read in its place");
            LecternSay(b, &l);
            c->ntags = 0;
            LineStart(&who);
            LinePut(&who, c->name);
        } else {
            LineNum(&l, count);
            LinePut(&l, count == 1 ? " scene carries " : " scenes carry ");
            TagsPut(&l, c);
            LinePut(&l, " — fid ");
            LineNum(&l, f->fid);
            LinePut(&l, ", the one picked, is read");
            LecternSay(b, &l);
            LineStart(&who);
            LinePut(&who, f->name);
        }
    }
    if (!c->ntags && !(f = FirstCalled(b, which, &count))) {
        LecternLine l;
        LineStart(&l);
        LinePut(&l, "no file on this volume is called ");
        LinePut(&l, c->name);
        LecternSay(b, &l);
        return;
    }
    if (!c->ntags && count > 1) {
        LecternLine l;
        LineStart(&l);
        LineNum(&l, count);
        LinePut(&l, " files are called ");
        LinePut(&l, c->name);
        LinePut(&l, " — fid ");
        LineNum(&l, f->fid);
        LinePut(&l, ", the first made, is read");
        LecternSay(b, &l);
    }
    LinePut(&who, " (fid ");
    LineNum(&who, f->fid);
    LinePut(&who, ")");
    if (!Readable(b, &who, f, c->most)) return;
    if (!f->size) {
        Said(b, &who, " is empty", 0, none, 0, none);
        c->says = CALLED_HELD;
        c->fid  = f->fid;
        return;
    }
    blocks = BlocksOf(f->size);
    if (!LecternOverlay(b, &f, 1)) {
        Said(b, &who, " cannot be read: the loader has no room for it", 0, none, 0, none);
        return;
    }
    uint8_t *room = Read(b, &who, f, blocks);
    if (!room) return;
    AttestSays says = LecternAttest(b, who.text, f->runs, f->nruns, blocks, room, 1, &verdict);
    if (says == ATTEST_BROKEN) {
        b->desk->give(b->desk->ctx, room, blocks * VOLUME_BLOCK_BYTES);
        return;
    }
    LecternTailLaid(f->tail, room + (blocks - 1u) * VOLUME_BLOCK_BYTES);
    c->says  = says == ATTEST_HELD ? CALLED_HELD : CALLED_UNCHECKED;
    c->bytes = room;
    c->room  = blocks * VOLUME_BLOCK_BYTES;
    c->size  = f->size;
    c->fid   = f->fid;
}
