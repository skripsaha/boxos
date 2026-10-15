/*
 * quilltest_book.c — files made, named, tagged and given up on a format-v2
 * book, whose kernel lays them into stitches and no byte of a file's data
 * (logcheck book_circle, book_stitchcut).
 *
 * ‼ NAMES: EVERY KIND OF RECORD A STITCH CARRIES FOR A FILE, EACH ROUND
 * ANCHORED. Three rounds, the volume anchored after each, so each lands in a
 * stitch of its own: NAME-a.n is made wearing NAME:a and NAME-b.n bare (two
 * Borns, their tags Coineds); NAME-a.n is renamed NAME-kept.n and NAME-b.n
 * tagged NAME:b (two Labeleds); NAME-b.n is given up and NAME-kept.n's tag
 * taken off (a Quitclaim and a Labeled). What is left — NAME-kept.n alone, no
 * tag of NAME's — is what volume-read's census and the next mount must hold.
 *
 * ‼ NAMECUT: A STITCH OF MORE THAN ONE ATOM, CUT AT ITS LAST. A kernel built
 * SETTLECUT=stitch stops the machine before the last atom of the stitch that
 * settles the first anchor waited for (settle_cut.h). Each round makes NAME<r>.n
 * wearing two tags of TAG_PAD bytes — Coineds that take the stitch past one
 * atom — says its fid, and anchors it; an anchor answered at once (the cadence
 * settled the file first) waits for nothing, and the next round's does. The
 * machine stops inside some round: the last fid said is a file of the torn
 * stitch, said straight to the debug port before its anchor. NAMECHECK, the
 * next boot: which of NAME<r>.n are present — never the torn one — and the fid
 * a file made now takes: the next the mount gives, past every one the torn
 * stitch's sectors that landed name (C2).
 *
 * ‼ GROUND: A FILE WITH BLOCKS GIVEN UP, AND THE RIBBON RUN PAST ITS QUIRE.
 * FILE, one the maker laid with bytes of its own, is given up and anchored —
 * a Quitclaim of a file with ground, which the next mount finds the ribbon let
 * go (M-1) — then NAME-x.n is renamed back and forth, each rename anchored, a
 * stitch of an atom each, for more stitches than a Quire of the maker's holds:
 * the ribbon goes on into its next_quire past the Quitclaim's Quire, and the
 * file's ground comes back in this session once the lag settled past it.
 *
 * Each says its verdict in quilltest's four words.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/error.h"
#include "box/debug.h"
#include "quilltest.h"

#define QUILL_BOOK_NAME_MAX  12u      /* NAME, so every name it makes stays inside 31 bytes */
#define QUILL_BOOK_CAP       32u
#define QUILL_BOOK_ROUNDS    8u       /* namecut's rounds before it gives up */
#define QUILL_BOOK_PAD       3000u    /* a tag's value: two of them take a stitch past one atom */
#define QUILL_BOOK_TURNS     300u     /* anchored renames: more stitches of an atom than a Quire of 256 holds */

static char s_pad[QUILL_BOOK_PAD + QUILL_BOOK_CAP];

/* `out` = NAME, then the decimal `number` when `with`, then `rest`. */
static void QuillBookName(char out[QUILL_BOOK_CAP], const char *name, bool with, uint32_t number, const char *rest)
{
    char n[12];
    strcpy(out, name);
    if (with)
        strcat(out, uint_to_str(number, n, sizeof(n)));
    strcat(out, rest);
}

static bool QuillBookNameFits(const char *mode, const char *name)
{
    if (strlen(name) == 0 || strlen(name) > QUILL_BOOK_NAME_MAX) {
        printf("[QUILL] %s: FAIL — NAME must be 1 to %u bytes\n", mode, QUILL_BOOK_NAME_MAX);
        return false;
    }
    return true;
}

/* A step of names: true when it answered, else the FAIL said. */
static bool QuillBookStep(const char *what, int64_t rc)
{
    if (rc >= 0)
        return true;
    printf("[QUILL] names: FAIL — %s answered %lld (error %u)\n", what, (long long)rc,
           (unsigned)box_errno_of((int)rc));
    return false;
}

/* How many files are called `name`, the first in *fid. */
static int QuillBookFind(const char *name, file_id_t *fid)
{
    file_id_t ids[4];
    int       n = QuillFindEverywhere(name, ids, 4);
    if (n > 0)
        *fid = ids[0];
    return n;
}

/* Every file called `name` given up: 0, or the refusal. */
static int QuillBookForget(const char *name)
{
    file_id_t fid = 0;
    int       n;
    while ((n = QuillBookFind(name, &fid)) > 0) {
        int rc = delete(fid);
        if (rc < 0)
            return rc;
    }
    return n;
}

/* The three rounds, each anchored. */
static bool QuillBookRounds(const char *name, int64_t *kept, int64_t *gone)
{
    char a[QUILL_BOOK_CAP], b[QUILL_BOOK_CAP], k[QUILL_BOOK_CAP], tag[QUILL_BOOK_CAP];
    QuillBookName(a, name, false, 0, "-a.n");
    QuillBookName(b, name, false, 0, "-b.n");
    QuillBookName(k, name, false, 0, "-kept.n");
    if (!QuillBookStep("forgetting the kept name", QuillBookForget(k)))
        return false;
    QuillBookName(tag, name, false, 0, ":a");
    *kept = QuillCreate(a);
    *gone = QuillCreate(b);
    if (!QuillBookStep("the first create", *kept) || !QuillBookStep("the second create", *gone) ||
        !QuillBookStep("the tag of the first", tag_add((file_id_t)*kept, tag)) ||
        !QuillBookStep("the first anchor", anchor(0)))
        return false;
    QuillBookName(tag, name, false, 0, ":b");
    if (!QuillBookStep("the rename", file_rename((file_id_t)*kept, k)) ||
        !QuillBookStep("the tag put on", tag_add((file_id_t)*gone, tag)) ||
        !QuillBookStep("the second anchor", anchor(0)))
        return false;
    return QuillBookStep("the delete", delete((file_id_t)*gone)) &&
           QuillBookStep("the tag taken off", tag_remove((file_id_t)*kept, name)) &&
           QuillBookStep("the third anchor", anchor(0));
}

QuillVerdict QuillBookNames(const char *const *args)
{
    const char *name = args[0];
    int64_t     kept = 0, gone = 0;
    file_id_t   found = 0;
    char        k[QUILL_BOOK_CAP], b[QUILL_BOOK_CAP];
    file_info_t info;
    if (!QuillBookNameFits("names", name) || !QuillBookRounds(name, &kept, &gone))
        return QUILL_FAIL;
    QuillBookName(k, name, false, 0, "-kept.n");
    QuillBookName(b, name, false, 0, "-b.n");
    int  nk   = QuillBookFind(k, &found), nb = QuillBookFind(b, &found);
    int  tags = file_info((file_id_t)kept, &info) == 0 ? (int)info.tag_count : -1;
    if (nk == 1 && nb == 0 && tags == 1) {
        printf("[QUILL] names: PASS — %s (fid %lld) made, tagged, renamed and its tag taken off, %s (fid %lld) "
               "tagged and given up, each round anchored\n", k, (long long)kept, b, (long long)gone);
        return QUILL_PASS;
    }
    printf("[QUILL] names: FAIL — %d file(s) called %s, %d called %s, the kept file wearing %d tag(s)\n", nk, k, nb,
           b, tags);
    return QUILL_FAIL;
}

/* Round r's file wearing two tags of QUILL_BOOK_PAD bytes: its fid, or the
 * refusal (<= 0). */
static int64_t QuillBookPadded(const char *name, uint32_t r, char file[QUILL_BOOK_CAP])
{
    QuillBookName(file, name, true, r, ".n");
    int64_t fid = QuillCreate(file);
    for (uint32_t t = 0; fid > 0 && t < 2u; t++) {
        char   tail[4] = { 'p', (char)('a' + t), ':', '\0' };
        QuillBookName(s_pad, name, true, r, tail);
        size_t at = strlen(s_pad);
        memset(s_pad + at, 'a' + (int)((r + t) % 26u), QUILL_BOOK_PAD);
        s_pad[at + QUILL_BOOK_PAD] = '\0';
        int rc = tag_add((file_id_t)fid, s_pad);
        if (rc < 0)
            fid = rc;
    }
    return fid;
}

QuillVerdict QuillBookNameCut(const char *const *args)
{
    const char *name = args[0];
    char        file[QUILL_BOOK_CAP];
    if (!QuillBookNameFits("namecut", name))
        return QUILL_FAIL;
    for (uint32_t r = 0; r < QUILL_BOOK_ROUNDS; r++) {
        int64_t fid = QuillBookPadded(name, r, file);
        if (fid <= 0) {
            printf("[QUILL] namecut: FAIL — round %u: %s answered %lld (error %u)\n", r, file, (long long)fid,
                   (unsigned)box_errno_of((int)fid));
            return QUILL_FAIL;
        }
        /* Straight to the debug port: the machine may stop inside this
         * round's anchor, before the console has drawn a line said to it. */
        kdbg_print("[QUILL] namecut: round %u, %s fid %ld made with two tags of %u bytes — anchoring", r, file,
                   (long)fid, QUILL_BOOK_PAD);
        int rc = anchor((file_id_t)fid);
        if (rc != 0) {
            printf("[QUILL] namecut: FAIL — round %u: the anchor answered %d (error %u)\n", r, rc,
                   (unsigned)box_errno_of(rc));
            return QUILL_FAIL;
        }
    }
    printf("[QUILL] namecut: FAIL — %u anchors answered and the machine never stopped: no kernel built "
           "SETTLECUT=stitch, or no anchor of them waited\n", QUILL_BOOK_ROUNDS);
    return QUILL_FAIL;
}

QuillVerdict QuillBookNameCheck(const char *const *args)
{
    const char *name = args[0];
    char        file[QUILL_BOOK_CAP], present[QUILL_BOOK_ROUNDS * 4u + 8u], n[12];
    file_id_t   fid = 0;
    if (!QuillBookNameFits("namecheck", name))
        return QUILL_FAIL;
    present[0] = '\0';
    for (uint32_t r = 0; r < QUILL_BOOK_ROUNDS; r++) {
        QuillBookName(file, name, true, r, ".n");
        if (QuillBookFind(file, &fid) <= 0)
            continue;
        if (present[0])
            strcat(present, ",");
        strcat(present, uint_to_str(r, n, sizeof(n)));
    }
    QuillBookName(file, name, false, 0, "-after.n");
    int64_t made = QuillCreate(file);
    if (made <= 0) {
        printf("[QUILL] namecheck: FAIL — %s could not be made (answer %lld)\n", file, (long long)made);
        return QUILL_FAIL;
    }
    printf("[QUILL] namecheck: PASS — rounds present: %s; a file made now takes fid %lld\n",
           present[0] ? present : "none", (long long)made);
    return QUILL_PASS;
}

/* NAME-x.n renamed to NAME-y.n and back, `turns` times, each anchored: 0, or
 * the refusal. */
static int QuillBookTurns(const char *name, uint32_t turns)
{
    char      x[QUILL_BOOK_CAP], y[QUILL_BOOK_CAP];
    file_id_t fid = 0;
    QuillBookName(x, name, false, 0, "-x.n");
    QuillBookName(y, name, false, 0, "-y.n");
    int rc = QuillBookForget(x);
    if (rc >= 0)
        rc = QuillBookForget(y);
    int64_t made = rc >= 0 ? QuillCreate(x) : rc;
    if (made <= 0)
        return made < 0 ? (int)made : -1;
    fid = (file_id_t)made;
    for (uint32_t t = 0; rc >= 0 && t < turns; t++) {
        rc = file_rename(fid, t % 2u ? x : y);
        if (rc >= 0)
            rc = anchor(0);
    }
    if (rc >= 0)
        rc = delete(fid);
    return rc >= 0 ? anchor(0) : rc;
}

QuillVerdict QuillBookGround(const char *const *args)
{
    const char *name = args[0], *file = args[1];
    file_id_t   fid  = 0;
    file_info_t info;
    if (!QuillBookNameFits("ground", name))
        return QUILL_FAIL;
    if (QuillBookFind(file, &fid) != 1 || file_info(fid, &info) != 0 || info.size == 0) {
        printf("[QUILL] ground: FAIL — %s is not one file with bytes of its own\n", file);
        return QUILL_FAIL;
    }
    int rc = delete(fid);
    if (rc >= 0)
        rc = anchor(0);
    if (rc < 0) {
        printf("[QUILL] ground: FAIL — giving up %s answered %d (error %u)\n", file, rc, (unsigned)box_errno_of(rc));
        return QUILL_FAIL;
    }
    rc = QuillBookTurns(name, QUILL_BOOK_TURNS);
    if (rc < 0) {
        printf("[QUILL] ground: FAIL — the renames answered %d (error %u)\n", rc, (unsigned)box_errno_of(rc));
        return QUILL_FAIL;
    }
    printf("[QUILL] ground: PASS — %s (fid %llu, %llu bytes) given up and anchored, then %u renames anchored "
           "past it\n", file, (unsigned long long)fid, (unsigned long long)info.size, QUILL_BOOK_TURNS);
    return QUILL_PASS;
}
