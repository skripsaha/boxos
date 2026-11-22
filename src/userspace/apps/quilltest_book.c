/*
 * quilltest_book.c — files made, named, tagged, written and given up on a
 * book, whose kernel lays them into stitches (logcheck book_circle,
 * book_big, book_stitchcut).
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
 * ‼ DATA: EVERY CHANGE OF A FILE'S BYTES, EACH ROUND ANCHORED. NAME-d.d is
 * written three blocks and a tail, appended to, written over whole in a block
 * and in part across two, cut, grown across a hole, and written inside the
 * hole — each round a stitch of its own on fresh ground — and read back whole
 * against the bytes the rounds wrote; its size and the FNV-1a of its bytes are
 * said for volume-read to hold the image to (logcheck book_circle).
 *
 * ‼ BIG: A FILE PAST THE ROOM A MACHINE IS GIVEN, IN ONE WRITE. With MIB,
 * NAME-b.d is made and written MIB mebibytes in ONE write — the kernel cuts it
 * into windows and inks each, and its stitch carries a digest for every block
 * — anchored, and read back a mebibyte at a time, each byte held to the one a
 * function of its offset gives, so no second copy of the whole is kept;
 * without, NAME-b.d is found and read back the same (logcheck book_big, the
 * next mount). Its size and the FNV-1a of its bytes are said.
 *
 * LOGGER: A LINE A SECOND, ANCHORED RARELY. NAME-g.log gets one line of 80
 * bytes each second for SECONDS seconds, by the machine's own clock, and an
 * anchor after every ANCHOR_EVERY-th line (0: none but the last) whose wait is
 * timed, saying every QUILL_LOGGER_SAY lines how many it has appended — the
 * console's, never the volume's: the record it is judged by holds no write of
 * it. What a logger leaves for the cadence to decide is the lines between
 * the anchors: the Slip, the window of five seconds and the Tally by useful
 * bytes (logcheck loggerhour).
 *
 * Each says its verdict in quilltest's four words.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/error.h"
#include "box/debug.h"
#include "box/memory.h"
#include "box/sync.h"
#include "box/time.h"
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
        !QuillBookStep("the tag of the first", tag_add(MY_TAGS, (file_id_t)*kept, tag)) ||
        !QuillBookStep("the first anchor", anchor(0)))
        return false;
    QuillBookName(tag, name, false, 0, ":b");
    if (!QuillBookStep("the rename", file_rename((file_id_t)*kept, k)) ||
        !QuillBookStep("the tag put on", tag_add(MY_TAGS, (file_id_t)*gone, tag)) ||
        !QuillBookStep("the second anchor", anchor(0)))
        return false;
    return QuillBookStep("the delete", delete((file_id_t)*gone)) &&
           QuillBookStep("the tag taken off", tag_remove(MY_TAGS, (file_id_t)*kept, name)) &&
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
    int  tags = file_info(MY_TAGS, (file_id_t)kept, &info) == 0 ? (int)info.tag_count : -1;
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
        int rc = tag_add(MY_TAGS, (file_id_t)fid, s_pad);
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
    if (QuillBookFind(file, &fid) != 1 || file_info(MY_TAGS, fid, &info) != 0 || info.size == 0) {
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

// ----------------------------------------------------------------------------
// data — every change of a file's bytes, each anchored
// ----------------------------------------------------------------------------

#define QUILL_DATA_BLOCK  4096u
#define QUILL_DATA_ROOM   (8u * QUILL_DATA_BLOCK)

static uint8_t s_data[QUILL_DATA_ROOM];
static uint8_t s_data_back[QUILL_DATA_ROOM];

/* Round r's byte at offset o of the file. */
static uint8_t QuillDataByte(uint64_t o, uint32_t r)
{
    return (uint8_t)(o * 31u + r * 17u + (o >> 12));
}

/* `bytes` of round r written at `at`, into the file and the model, and the
 * volume anchored: 0, or the refusal. */
static int64_t QuillDataWrite(file_id_t fid, uint64_t *size, uint64_t at, uint32_t bytes, uint32_t r)
{
    for (uint32_t i = 0; i < bytes; i++)
        s_data[at + i] = QuillDataByte(at + i, r);
    int64_t wrote = fwrite(fid, at, s_data + at, bytes);
    if (wrote != (int64_t)bytes)
        return wrote < 0 ? wrote : -1;
    if (at + bytes > *size)
        *size = at + bytes;
    return anchor(0);
}

/* The file spanned to `to`, the model with it, and the volume anchored. */
static int64_t QuillDataSpan(file_id_t fid, uint64_t *size, uint64_t to)
{
    int rc = file_span(fid, to, 0);
    if (rc < 0)
        return rc;
    if (to > *size)
        memset(s_data + *size, 0, to - *size);
    *size = to;
    return anchor(0);
}

/* The rounds, in order: OK, or the refusal and in *what the round. */
static int64_t QuillDataRounds(file_id_t fid, uint64_t *size, const char **what)
{
    const uint32_t B = QUILL_DATA_BLOCK;
    int64_t        rc;
    if ((rc = QuillDataWrite(fid, size, 0, 3u * B + 100u, 1)) < 0)
        return *what = "the first write", rc;
    if ((rc = QuillDataWrite(fid, size, *size, 5000u, 2)) < 0)
        return *what = "the append", rc;
    if ((rc = QuillDataWrite(fid, size, B, B, 3)) < 0)
        return *what = "a block written over whole", rc;
    if ((rc = QuillDataWrite(fid, size, 4000u, 300u, 4)) < 0)
        return *what = "two blocks written over in part", rc;
    if ((rc = QuillDataSpan(fid, size, 2u * B + 10u)) < 0)
        return *what = "the cut", rc;
    if ((rc = QuillDataSpan(fid, size, 6u * B + 7u)) < 0)
        return *what = "the growth across a hole", rc;
    if ((rc = QuillDataWrite(fid, size, 4u * B + 3000u, 200u, 5)) < 0)
        return *what = "a write inside the hole", rc;
    return 0;
}

#define QUILL_FNV_BASIS 0xCBF29CE484222325ull

/* FNV-1a of `n` bytes, folded into `h`. */
static uint64_t QuillFnv(uint64_t h, const uint8_t *b, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/* `h` as sixteen hex digits into `out`: this printf has no width for a 64-bit
 * number. */
static void QuillHex(uint64_t h, char out[17])
{
    for (int d = 15; d >= 0; d--, h >>= 4)
        out[d] = "0123456789ABCDEF"[h & 0xFu];
    out[16] = '\0';
}

QuillVerdict QuillBookData(const char *const *args)
{
    const char *name = args[0], *what = "the create";
    char        file[QUILL_BOOK_CAP];
    uint64_t    size = 0;
    if (!QuillBookNameFits("data", name))
        return QUILL_FAIL;
    QuillBookName(file, name, false, 0, "-d.d");
    memset(s_data, 0, sizeof(s_data));
    int64_t fid = QuillCreate(file);
    int64_t rc  = fid > 0 ? QuillDataRounds((file_id_t)fid, &size, &what) : fid;
    if (rc < 0) {
        printf("[QUILL] data: FAIL — %s answered %lld (error %u)\n", what, (long long)rc,
               (unsigned)box_errno_of((int)rc));
        return QUILL_FAIL;
    }
    file_info_t info;
    int64_t     got = fread((file_id_t)fid, 0, s_data_back, sizeof(s_data_back));
    if (file_info(MY_TAGS, (file_id_t)fid, &info) != 0 || info.size != size || got != (int64_t)size ||
        memcmp(s_data_back, s_data, size) != 0) {
        printf("[QUILL] data: FAIL — %s reads %lld byte(s) back, %s, and should be %llu\n", file, (long long)got,
               got == (int64_t)size ? "not the bytes the rounds wrote" : "not as many", (unsigned long long)size);
        return QUILL_FAIL;
    }
    char print[17];
    QuillHex(QuillFnv(QUILL_FNV_BASIS, s_data, size), print);
    printf("[QUILL] data: PASS — %s (fid %lld) written, appended, written over whole and in part, cut, grown across "
           "a hole and written inside it, each round anchored: %llu bytes, print 0x%s\n", file, (long long)fid,
           (unsigned long long)size, print);
    return QUILL_PASS;
}

// ----------------------------------------------------------------------------
// big — a file past the room a machine is given, in one write
// ----------------------------------------------------------------------------

#define QUILL_BIG_MIB_MOST  1024u
#define QUILL_BIG_CHUNK     (1u << 20)

/* The file's byte at offset o — a function of where it lies alone, so every
 * part of it is checked as it is read, with no second copy of the whole. */
static uint8_t QuillBigByte(uint64_t o)
{
    return (uint8_t)(o * 131u + (o >> 12) * 7u + (o >> 20));
}

/* The file of `size` bytes read back a mebibyte at a time, each byte held to
 * QuillBigByte: 0 and the print of its bytes in *print, or the refusal — -1
 * for a short read, -2 for a byte that differs and its offset in *print. */
static int64_t QuillBigRead(file_id_t fid, uint8_t *buf, uint64_t size, uint64_t *print)
{
    uint64_t h = QUILL_FNV_BASIS;
    for (uint64_t at = 0; at < size;) {
        uint64_t want = size - at < QUILL_BIG_CHUNK ? size - at : QUILL_BIG_CHUNK;
        int64_t  got  = fread(fid, at, buf, want);
        if (got != (int64_t)want)
            return got < 0 ? got : -1;
        for (uint64_t i = 0; i < want; i++)
            if (buf[i] != QuillBigByte(at + i))
                return *print = at + i, -2;
        h = QuillFnv(h, buf, want);
        at += want;
    }
    *print = h;
    return 0;
}

/* `file` made anew and written whole in ONE write of `bytes`, then anchored:
 * its file id, or the refusal and in *what the step it came at. */
static int64_t QuillBigWrite(const char *file, uint64_t bytes, const char **what)
{
    uint8_t *buf = malloc(bytes);
    int64_t  rc  = -1;
    *what = "the buffer";
    if (!buf)
        return rc;
    for (uint64_t o = 0; o < bytes; o++)
        buf[o] = QuillBigByte(o);
    *what = "the create";
    int64_t fid = QuillCreate(file);
    if (fid > 0) {
        int64_t wrote = fwrite((file_id_t)fid, 0, buf, bytes);
        *what = "the one write";
        rc    = wrote == (int64_t)bytes ? 0 : wrote < 0 ? wrote : -1;
        if (rc == 0) {
            *what = "the anchor";
            rc    = anchor(0);
        }
    } else {
        rc = fid;
    }
    free(buf);
    return rc < 0 ? rc : fid;
}

QuillVerdict QuillBookBig(const char *const *args)
{
    const char *name = args[0], *what = "finding it";
    char        file[QUILL_BOOK_CAP], print[17];
    uint32_t    mib  = 0;
    file_id_t   ids[2];
    uint64_t    seen = 0;
    if (!QuillBookNameFits("big", name))
        return QUILL_FAIL;
    if (args[1] && (!QuillNumber(args[1], &mib) || mib == 0 || mib > QUILL_BIG_MIB_MOST)) {
        printf("[QUILL] big: FAIL — MIB must be 1 to %u, and was %s\n", QUILL_BIG_MIB_MOST, args[1]);
        return QUILL_FAIL;
    }
    QuillBookName(file, name, false, 0, "-b.d");
    int64_t fid = -1;
    if (mib)
        fid = QuillBigWrite(file, (uint64_t)mib << 20, &what);
    else if (QuillFindEverywhere(file, ids, 2) == 1)
        fid = (int64_t)ids[0];
    uint64_t size = fid > 0 ? QuillSize((file_id_t)fid) : 0;
    uint8_t *buf  = fid > 0 ? malloc(QUILL_BIG_CHUNK) : NULL;
    int64_t  rc   = fid;
    if (fid > 0) {
        what = "the read back";
        rc   = buf ? QuillBigRead((file_id_t)fid, buf, size, &seen) : -1;
    }
    free(buf);
    if (rc < 0 || (mib && size != (uint64_t)mib << 20)) {
        printf("[QUILL] big: FAIL — %s of %s answered %lld (error %u)%s, the file %llu bytes\n", what, file,
               (long long)rc, (unsigned)box_errno_of((int)rc), rc == -2 ? " at a byte that differs" : "",
               (unsigned long long)size);
        if (rc == -2)
            printf("[QUILL] big: the byte at offset %llu is not the one written\n", (unsigned long long)seen);
        return QUILL_FAIL;
    }
    QuillHex(seen, print);
    printf("[QUILL] big: PASS — %s (fid %lld) %s, every byte read back: %llu bytes, print 0x%s\n", file,
           (long long)fid, mib ? "written in one write and anchored" : "found", (unsigned long long)size, print);
    return QUILL_PASS;
}

// ----------------------------------------------------------------------------
// log — a logger: a line a stitch, anchored
// ----------------------------------------------------------------------------

#define QUILL_LOG_LINE   48u
#define QUILL_LOG_MOST   20000u

/* Line n of the log, QUILL_LOG_LINE bytes ending in a newline. */
static void QuillLogLine(uint8_t *out, uint32_t n)
{
    static const char digits[] = "0123456789";
    memset(out, '.', QUILL_LOG_LINE);
    for (int d = 7; d >= 0; d--, n /= 10u)
        out[d] = (uint8_t)digits[n % 10u];
    out[QUILL_LOG_LINE - 1u] = '\n';
}

/* The number LINES says, or 0 when it is none in [1, QUILL_LOG_MOST]. */
static uint32_t QuillLogLines(const char *word)
{
    uint32_t n = 0;
    for (const char *c = word; *c; c++) {
        if (*c < '0' || *c > '9' || n > QUILL_LOG_MOST)
            return 0;
        n = n * 10u + (uint32_t)(*c - '0');
    }
    return n <= QUILL_LOG_MOST ? n : 0;
}

QuillVerdict QuillBookLog(const char *const *args)
{
    const char *name  = args[0];
    uint32_t    lines = QuillLogLines(args[1]);
    char        file[QUILL_BOOK_CAP], hex[17];
    uint8_t     line[QUILL_LOG_LINE];
    uint64_t    h = QUILL_FNV_BASIS;
    if (!QuillBookNameFits("log", name))
        return QUILL_FAIL;
    if (!lines) {
        printf("[QUILL] log: FAIL — LINES is a number from 1 to %u\n", QUILL_LOG_MOST);
        return QUILL_FAIL;
    }
    QuillBookName(file, name, false, 0, "-l.log");
    int64_t fid = QuillCreate(file), rc = fid > 0 ? 0 : fid;
    for (uint32_t n = 0; rc >= 0 && n < lines; n++) {
        QuillLogLine(line, n);
        h      = QuillFnv(h, line, QUILL_LOG_LINE);
        int64_t wrote = fwrite((file_id_t)fid, (uint64_t)n * QUILL_LOG_LINE, line, QUILL_LOG_LINE);
        rc     = wrote == (int64_t)QUILL_LOG_LINE ? anchor(0) : (wrote < 0 ? wrote : -1);
    }
    if (rc < 0) {
        printf("[QUILL] log: FAIL — an append answered %lld (error %u)\n", (long long)rc,
               (unsigned)box_errno_of((int)rc));
        return QUILL_FAIL;
    }
    uint64_t back = QUILL_FNV_BASIS, size = (uint64_t)lines * QUILL_LOG_LINE;
    for (uint64_t at = 0; at < size; at += sizeof(s_data_back)) {
        uint64_t n = size - at < sizeof(s_data_back) ? size - at : sizeof(s_data_back);
        if (fread((file_id_t)fid, at, s_data_back, n) != (int64_t)n) {
            printf("[QUILL] log: FAIL — %s would not be read back at byte %llu\n", file, (unsigned long long)at);
            return QUILL_FAIL;
        }
        back = QuillFnv(back, s_data_back, n);
    }
    QuillHex(h, hex);
    if (back != h) {
        printf("[QUILL] log: FAIL — %s reads back other bytes than the lines appended\n", file);
        return QUILL_FAIL;
    }
    printf("[QUILL] log: PASS — %s (fid %lld): %u lines appended and anchored one by one, %llu bytes, print 0x%s\n",
           file, (long long)fid, lines, (unsigned long long)size, hex);
    return QUILL_PASS;
}

// ----------------------------------------------------------------------------
// logger — a line a second, anchored every so often
// ----------------------------------------------------------------------------

#define QUILL_LOGGER_LINE  80u
#define QUILL_LOGGER_MOST  86400u
/* How often the logger says how far it got: half the quiet window a stand
 * holds a machine to (tools/stand.sh WAIT_QUIET_S, 60 s), so a logger that
 * stops is a machine gone quiet, and a wait as long as the run — an hour —
 * counts its runaway guard from the newest line said, not from its start. */
#define QUILL_LOGGER_SAY   30u

static volatile uint64_t s_logger_pause;

/* Line n of the logger, QUILL_LOGGER_LINE bytes ending in a newline. */
static void QuillLoggerLine(uint8_t *out, uint32_t n)
{
    memset(out, '.', QUILL_LOGGER_LINE);
    for (int d = 7; d >= 0; d--, n /= 10u)
        out[d] = (uint8_t)('0' + n % 10u);
    out[QUILL_LOGGER_LINE - 1u] = '\n';
}

/* The machine's clock reaches `until_ms`: parked on a word nobody wakes, for
 * what is left of the second, as often as it takes. */
static void QuillLoggerWait(uint64_t until_ms)
{
    uint64_t now = 0;
    for (time_uptime_ms(&now); now < until_ms; time_uptime_ms(&now))
        addr_park(&s_logger_pause, 0, (uint32_t)(until_ms - now));
}

typedef struct {
    uint32_t anchors;
    uint64_t longest_ms;
} QuillLoggerAnchors;

/* An anchor, its wait timed into *seen. */
static int64_t QuillLoggerAnchor(QuillLoggerAnchors *seen)
{
    uint64_t from = 0, to = 0;
    time_uptime_ms(&from);
    int64_t rc = anchor(0);
    time_uptime_ms(&to);
    seen->anchors++;
    if (to - from > seen->longest_ms)
        seen->longest_ms = to - from;
    return rc;
}

QuillVerdict QuillBookLogger(const char *const *args)
{
    const char *name = args[0];
    uint32_t    seconds = 0, every = 0;
    char        file[QUILL_BOOK_CAP], hex[17];
    uint8_t     line[QUILL_LOGGER_LINE];
    uint64_t    h = QUILL_FNV_BASIS, began = 0, ended = 0;
    QuillLoggerAnchors seen = { 0, 0 };
    if (!QuillBookNameFits("logger", name))
        return QUILL_FAIL;
    if (!QuillNumber(args[1], &seconds) || seconds == 0 || seconds > QUILL_LOGGER_MOST ||
        (args[2] && !QuillNumber(args[2], &every))) {
        printf("[QUILL] logger: FAIL — SECONDS is a number from 1 to %u and ANCHOR_EVERY a number\n",
               QUILL_LOGGER_MOST);
        return QUILL_FAIL;
    }
    QuillBookName(file, name, false, 0, "-g.log");
    int64_t fid = QuillCreate(file), rc = fid > 0 ? 0 : fid;
    time_uptime_ms(&began);
    for (uint32_t n = 0; rc >= 0 && n < seconds; n++) {
        QuillLoggerLine(line, n);
        h = QuillFnv(h, line, QUILL_LOGGER_LINE);
        int64_t wrote = fwrite((file_id_t)fid, (uint64_t)n * QUILL_LOGGER_LINE, line, QUILL_LOGGER_LINE);
        rc = wrote == (int64_t)QUILL_LOGGER_LINE ? 0 : (wrote < 0 ? wrote : -1);
        if (rc >= 0 && every && (n + 1u) % every == 0 && n + 1u < seconds)
            rc = QuillLoggerAnchor(&seen);
        if (rc >= 0 && (n + 1u) % QUILL_LOGGER_SAY == 0 && n + 1u < seconds)
            printf("[QUILL] logger: %u of %u lines appended, %u anchor(s)\n", n + 1u, seconds, seen.anchors);
        if (rc >= 0 && n + 1u < seconds)
            QuillLoggerWait(began + (uint64_t)(n + 1u) * 1000u);
    }
    time_uptime_ms(&ended);
    if (rc >= 0)
        rc = QuillLoggerAnchor(&seen);
    if (rc < 0) {
        printf("[QUILL] logger: FAIL — an append or an anchor answered %lld (error %u)\n", (long long)rc,
               (unsigned)box_errno_of((int)rc));
        return QUILL_FAIL;
    }
    uint64_t back = QUILL_FNV_BASIS, size = (uint64_t)seconds * QUILL_LOGGER_LINE;
    for (uint64_t at = 0; at < size; at += sizeof(s_data_back)) {
        uint64_t n = size - at < sizeof(s_data_back) ? size - at : sizeof(s_data_back);
        if (fread((file_id_t)fid, at, s_data_back, n) != (int64_t)n) {
            printf("[QUILL] logger: FAIL — %s would not be read back at byte %llu\n", file, (unsigned long long)at);
            return QUILL_FAIL;
        }
        back = QuillFnv(back, s_data_back, n);
    }
    QuillHex(h, hex);
    if (back != h) {
        printf("[QUILL] logger: FAIL — %s reads back other bytes than the lines appended\n", file);
        return QUILL_FAIL;
    }
    printf("[QUILL] logger: PASS — %s (fid %lld): %u lines in %llu ms, %u anchor(s) the longest answered in "
           "%llu ms, %llu bytes, print 0x%s\n", file, (long long)fid, seconds, (unsigned long long)(ended - began),
           seen.anchors, (unsigned long long)seen.longest_ms, (unsigned long long)size, hex);
    return QUILL_PASS;
}
