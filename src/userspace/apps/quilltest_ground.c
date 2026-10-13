/*
 * quilltest_ground.c — the ground a file stands on, asked from many cabins at
 * once (logcheck cores): a big file read while another cabin deletes it, and
 * files defragged while another cabin deletes them and makes them again. See
 * quilltest_cores.c for how a driver and its workers go.
 *
 * ‼ A READ HOLDS NO HELM, SO WHAT KEEPS ITS BLOCKS IS FALLOW.
 * A read walks the record its open copied; a delete under it gives the file's
 * ground up, and that ground rests while a handle opened before the delete is
 * open (fallow.h, condition 3). readdelete aims its delete into a window that
 * is being read, runs the Settle cycles that would hand the ground back, and
 * writes a file of the same size — which takes the lowest free run, where the
 * deleted file lay — before the window's read has its answer. The window must
 * deliver the deleted file's own bytes, and every window after it a clean
 * refusal: never the filler's.
 *
 * ‼ A DEFRAG HOLDS THE HELM, AND A DELETE DOES NOT WAIT FOR IT.
 * The delete gives the file up at once; the defrag finds it given up at its
 * next window or at its Ink, and gives back the run it took (helm_work.c).
 * defragdelete keeps two defraggers going round every slot while it deletes
 * each slot's file and makes the next, written 4 KiB at a time beside its
 * pair's so its ground comes in several runs; afterwards every slot holds its
 * last file, whole, and nothing it deleted — and the same again after the
 * power button and a reboot (defragback).
 */

#include "box/print.h"
#include "box/file.h"
#include "box/bay.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"
#include "box/touch.h"
#include "box/error.h"
#include "quilltest.h"

/* =========================================================================
 *  readdelete — a big file read in windows while another cabin deletes it
 * ========================================================================= */

#define QUILL_RD_NAME      "_qrd.bin"
#define QUILL_RD_FILLER    "_qrdf.bin"
#define QUILL_RD_SCRATCH   "_qrdx.bin"
#define QUILL_RD_BOARD     "quilltest:readdelete"
#define QUILL_RD_READER    "quilltest:readdelete:reader"
#define QUILL_RD_DELETER   "quilltest:readdelete:deleter"
#define QUILL_RD_WINDOWS   4u
#define QUILL_RD_AT        1u     /* the window the delete is aimed into */
#define QUILL_RD_CYCLES    4u     /* Settle cycles after the delete: FallowLag() + 2, and one more */
#define QUILL_RD_MIB_MAX   64u
#define QUILL_RD_ROUNDS    8u

typedef struct {
    volatile uint32_t stop;
    volatile uint32_t ready;       /* 1 the deleter listens; 2 it could not */
    volatile uint32_t mib;
    volatile uint32_t round;       /* from 1 */
    volatile uint32_t aimed;       /* the round whose aimed window is being read */
    volatile file_id_t fid;
    volatile uint32_t in_read;     /* the window being read, from 1; 0 while none is */
    volatile uint32_t done;        /* the rounds the deleter finished */
    volatile int32_t  deleted;     /* the delete's answer */
    volatile int32_t  cycled;      /* the first refusal of a cycle's write or anchor, 0 */
    volatile int64_t  filled;      /* the filler's write: its bytes, or a refusal */
    volatile uint32_t at_delete;   /* in_read once the delete was answered */
    volatile uint32_t at_end;      /* ... and once the filler was written */
} QuillRdBoard;

/* What one round of the reader saw. */
typedef struct {
    int64_t  got[QUILL_RD_WINDOWS];
    uint64_t wrong[QUILL_RD_WINDOWS];     /* bytes of a window that are not the file's */
    uint64_t first[QUILL_RD_WINDOWS];     /* ... the first of them, as a file offset */
} QuillRdRound;

static void QuillRdWait(TouchTag heard, volatile uint32_t *word, uint32_t want)
{
    Touch said;
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) < want)
        touch_await(heard, &said, 0);
}

/* A change the next anchor has to settle — the scratch rewritten — and the
 * anchor: one Settle cycle. 0, or the first refusal. */
static int QuillRdCycle(file_id_t scratch, const void *block)
{
    int64_t put = fwrite(scratch, 0, block, 4096);
    if (put != 4096)
        return put < 0 ? (int)put : -1;
    return anchor(0);
}

QuillVerdict QuillDeleter(const char *const *args)
{
    (void)args;
    QuillRdBoard *board = bay_open(QUILL_RD_BOARD, 0, BAY_OPEN);
    if (!board) {
        printf("[QUILL] deleter: FAIL — the board %s is not open: it is readdelete's (nothing was "
               "deleted)\n", QUILL_RD_BOARD);
        return QUILL_FAIL;
    }
    TouchTagPair reader  = touch_intern(QUILL_RD_READER);
    TouchTagPair mine    = touch_intern(QUILL_RD_DELETER);
    TouchTag     heard   = touch_pair_choose(mine);
    int          crc     = heard != TOUCH_TAG_INVALID ? touch_claim(heard, TOUCH_REST, 0, 0) : -1;
    uint64_t     bytes   = (uint64_t)board->mib << 20;
    uint64_t    *fill    = crc == 0 ? bay_open("quilltest:readdelete:fill", bytes, BAY_CREATE) : NULL;
    static uint64_t block[512];
    int64_t      scratch = fill ? QuillCreate(QUILL_RD_SCRATCH) : 0;
    bool         up      = scratch > 0;
    __atomic_store_n(&board->ready, up ? 1u : 2u, __ATOMIC_RELEASE);
    touch_send(reader, &crc, sizeof(crc), 0);

    uint32_t rounds = 0;
    while (up) {
        QuillRdWait(heard, &board->aimed, rounds + 1);
        if (__atomic_load_n(&board->stop, __ATOMIC_ACQUIRE))
            break;
        uint32_t r = board->aimed;
        /* One cycle first: the window's read is well in hand before the delete. */
        int cycled = QuillRdCycle((file_id_t)scratch, block);
        board->deleted   = delete(board->fid);
        board->at_delete = __atomic_load_n(&board->in_read, __ATOMIC_ACQUIRE);
        for (uint32_t c = 0; c < QUILL_RD_CYCLES && cycled == 0; c++)
            cycled = QuillRdCycle((file_id_t)scratch, block);
        board->cycled = cycled;
        int64_t filler = QuillCreate(QUILL_RD_FILLER);
        QuillFill(fill, 0xF0u + r, 0, bytes);
        board->filled = filler > 0 ? fwrite((file_id_t)filler, 0, fill, bytes) : filler;
        board->at_end = __atomic_load_n(&board->in_read, __ATOMIC_ACQUIRE);
        rounds = r;
        __atomic_store_n(&board->done, r, __ATOMIC_RELEASE);
        touch_send(reader, &r, sizeof(r), 0);
    }
    if (scratch > 0)
        delete((file_id_t)scratch);
    if (fill)
        bay_release(fill);
    if (crc == 0)
        touch_release(heard);
    bay_release(board);
    if (!up) {
        printf("[QUILL] deleter: FAIL — its tag answered the claim %d, its filler buffer of %llu "
               "bytes %s, and %s answered %ld (nothing was deleted)\n", crc,
               (unsigned long long)bytes, fill ? "was had" : "was not had", QUILL_RD_SCRATCH,
               scratch);
        return QUILL_FAIL;
    }
    printf("[QUILL] deleter: PASS — deleted the file being read in %u round(s), each followed by "
           "%u Settle cycles and a filler of %llu bytes\n", rounds, QUILL_RD_CYCLES,
           (unsigned long long)bytes);
    return QUILL_PASS;
}

/* The window `k` held against the stream: how many of its `got` bytes are
 * not the file's, the first of them in *first. */
static uint64_t QuillRdWrong(const uint8_t *got_bytes, int64_t got, uint32_t seed, uint64_t base,
                             uint64_t *want, uint64_t *first)
{
    uint64_t wrong = 0;
    *first = 0;
    if (got <= 0)
        return 0;
    QuillFill(want, seed, base, ((uint64_t)got + 7) & ~7ull);
    const uint8_t *w = (const uint8_t *)want;
    for (uint64_t i = 0; i < (uint64_t)got; i++)
        if (got_bytes[i] != w[i]) {
            if (wrong == 0)
                *first = base + i;
            wrong++;
        }
    return wrong;
}

/* One round: the file made and anchored, its windows read with the delete
 * aimed into QUILL_RD_AT, and the deleter's end of it waited for. 0, or the
 * answer that stopped the round before its first read. */
static int QuillRdRoundRun(QuillRdBoard *board, TouchTagPair deleter, TouchTag heard,
                           uint64_t *buf, uint64_t *want, uint32_t r, QuillRdRound *o)
{
    uint64_t bytes  = (uint64_t)board->mib << 20;
    uint64_t window = bytes / QUILL_RD_WINDOWS;
    uint32_t seed   = 0x60u + r;
    int64_t  fid    = QuillCreate(QUILL_RD_NAME);
    if (fid <= 0)
        return fid ? (int)fid : -1;
    QuillFill(buf, seed, 0, bytes);
    int64_t put = fwrite((file_id_t)fid, 0, buf, bytes);
    int     arc = put == (int64_t)bytes ? anchor((file_id_t)fid) : -1;
    if (arc != 0)
        return put != (int64_t)bytes ? (put < 0 ? (int)put : -1) : arc;

    memset(o, 0, sizeof(*o));
    board->fid = (file_id_t)fid;
    __atomic_store_n(&board->round, r + 1, __ATOMIC_RELEASE);
    for (uint32_t k = 0; k < QUILL_RD_WINDOWS; k++) {
        uint8_t *at = (uint8_t *)buf + k * window;
        __atomic_store_n(&board->in_read, k + 1, __ATOMIC_RELEASE);
        if (k == QUILL_RD_AT) {
            __atomic_store_n(&board->aimed, r + 1, __ATOMIC_RELEASE);
            touch_send(deleter, &r, sizeof(r), 0);
        }
        o->got[k] = fread((file_id_t)fid, k * window, at, window);
        __atomic_store_n(&board->in_read, 0u, __ATOMIC_RELEASE);
        o->wrong[k] = QuillRdWrong(at, o->got[k], seed, k * window, want, &o->first[k]);
    }
    QuillRdWait(heard, &board->done, r + 1);
    return 0;
}

/* The round as it must be: the delete answered, every window before the aim
 * delivered whole, the aimed one whole or refused, every one after refused as
 * a file not found — and no window held a byte that is not the file's. *in_flight
 * when the aimed window was read across the delete, its cycles and the filler. */
static bool QuillRdRoundJudge(const QuillRdBoard *b, const QuillRdRound *o, uint64_t window,
                              bool *in_flight, bool *wrong)
{
    bool ok = b->deleted == 0 && b->cycled == 0 && b->filled == (int64_t)(window * QUILL_RD_WINDOWS);
    *wrong  = false;
    for (uint32_t k = 0; k < QUILL_RD_WINDOWS; k++) {
        bool whole   = o->got[k] == (int64_t)window && o->wrong[k] == 0;
        bool refused = o->got[k] < 0 && box_errno_of((int)o->got[k]) == ERR_FILE_NOT_FOUND;
        *wrong |= o->wrong[k] != 0;
        if (k < QUILL_RD_AT)
            ok &= whole;
        else if (k == QUILL_RD_AT)
            ok &= whole || refused;
        else
            ok &= refused;
    }
    *in_flight = b->at_delete == QUILL_RD_AT + 1 && b->at_end == QUILL_RD_AT + 1 &&
                 o->got[QUILL_RD_AT] == (int64_t)window && o->wrong[QUILL_RD_AT] == 0;
    return ok && !*wrong;
}

static void QuillRdRoundSay(const QuillRdBoard *b, const QuillRdRound *o, uint32_t r, bool in_flight)
{
    printf("[QUILL] readdelete: round %u — windows answered %lld %lld %lld %lld (bytes not the "
           "file's: %llu %llu %llu %llu, the first at %llu); the delete answered %d with window "
           "%u being read, %u cycles answered %d, the filler %lld, window %u still being read "
           "after it: %s\n", r, (long long)o->got[0], (long long)o->got[1], (long long)o->got[2],
           (long long)o->got[3], (unsigned long long)o->wrong[0], (unsigned long long)o->wrong[1],
           (unsigned long long)o->wrong[2], (unsigned long long)o->wrong[3],
           (unsigned long long)(o->wrong[QUILL_RD_AT] ? o->first[QUILL_RD_AT] : o->first[0]),
           b->deleted, b->at_delete, QUILL_RD_CYCLES, b->cycled, (long long)b->filled,
           b->at_end, in_flight ? "yes" : "no");
}

/* The round's filler given up, so the next round's file lies where this
 * one's did. */
static void QuillRdDropFiller(void)
{
    file_id_t ids[4];
    int      n = QuillFindEverywhere(QUILL_RD_FILLER, ids, 4);
    for (int i = 0; i < n && i < 4; i++)
        delete(ids[i]);
}

/* The deleter started and listening, or a sentence why not. */
static bool QuillRdDeleterUp(QuillRdBoard *board, TouchTag heard, QuillCabin *deleter)
{
    int pid = QuillCabinStart(deleter, "quilltest deleter");
    if (pid > 0)
        QuillRdWait(heard, &board->ready, 1);
    if (pid > 0 && board->ready == 1)
        return true;
    printf("[QUILL] readdelete: FAIL — the deleter %s (answer %d)\n",
           pid > 0 ? "started and could not listen" : "would not start", pid);
    return false;
}

QuillVerdict QuillReadDelete(const char *const *args)
{
    uint32_t mib = 0, rounds = 0;
    if (!QuillNumber(args[0], &mib) || mib < QUILL_RD_WINDOWS || mib > QUILL_RD_MIB_MAX ||
        mib % QUILL_RD_WINDOWS || !QuillNumber(args[1], &rounds) || rounds == 0 ||
        rounds > QUILL_RD_ROUNDS) {
        printf("[QUILL] readdelete: FAIL — MIB is %u to %u, a multiple of %u, and ROUNDS 1 to %u; "
               "given '%s %s'\n", QUILL_RD_WINDOWS, QUILL_RD_MIB_MAX, QUILL_RD_WINDOWS,
               QUILL_RD_ROUNDS, args[0], args[1]);
        return QUILL_FAIL;
    }
    uint64_t      bytes  = (uint64_t)mib << 20;
    uint64_t      window = bytes / QUILL_RD_WINDOWS;
    QuillRdBoard *board  = bay_open(QUILL_RD_BOARD, sizeof(QuillRdBoard), BAY_CREATE);
    uint64_t     *buf    = board ? bay_open("quilltest:readdelete:buf", bytes, BAY_CREATE) : NULL;
    /* The window the stream is laid in for the comparison: a Bay, as every
     * buffer of this case is, and not the heap — a cabin's first allocation
     * of 2 MiB or more was measured refused twice in twenty-one starts on 16c
     * (SysHeapPrefault, the heap's first pages straddling the 2 MiB boundary
     * the allocation is laid at), and that is not this case's question. */
    uint64_t     *want   = buf ? bay_open("quilltest:readdelete:want", window, BAY_CREATE) : NULL;
    TouchTagPair  mine   = touch_intern(QUILL_RD_READER);
    TouchTag      heard  = touch_pair_choose(mine);
    int           crc    = want && heard != TOUCH_TAG_INVALID ? touch_claim(heard, TOUCH_REST, 0, 0) : -1;
    QuillCabin    deleter;
    memset(&deleter, 0, sizeof(deleter));
    if (board)
        memset((void *)board, 0, sizeof(*board));
    if (crc != 0 || !board) {
        printf("[QUILL] readdelete: FAIL — its board %s, its buffer of %llu bytes %s, its window "
               "of %llu bytes %s, its tag %s %u answered the claim %d (nothing was made)\n",
               board ? "opened" : "would not open", (unsigned long long)bytes,
               buf ? "opened" : "would not open", (unsigned long long)window,
               want ? "opened" : "would not open", QUILL_RD_READER, (uint32_t)heard, crc);
        return QUILL_FAIL;
    }
    board->mib = mib;
    if (!QuillRdDeleterUp(board, heard, &deleter))
        return QUILL_FAIL;

    TouchTagPair them  = touch_intern(QUILL_RD_DELETER);
    uint32_t     asked = 0, clean = 0, defect = 0;
    int          made  = 0;
    for (uint32_t r = 0; r < rounds && made == 0; r++) {
        QuillRdRound o;
        bool         in_flight = false, wrong = false;
        if ((made = QuillRdRoundRun(board, them, heard, buf, want, r, &o)) != 0)
            break;
        clean  += QuillRdRoundJudge(board, &o, window, &in_flight, &wrong);
        asked  += in_flight;
        defect += wrong;
        QuillRdRoundSay(board, &o, r + 1, in_flight);
        QuillRdDropFiller();
    }
    uint32_t done = board->done;
    __atomic_store_n(&board->stop, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&board->aimed, done + 1, __ATOMIC_RELEASE);
    touch_send(them, &done, sizeof(done), 0);
    int32_t code = 0;
    int     grc  = QuillCabinEnd(&deleter, &code);
    QuillRdDropFiller();
    touch_release(heard);
    bay_release(want);
    bay_release(buf);
    bay_release(board);
    int arc = anchor(0);

    if (defect) {
        printf("[QUILL] readdelete: DEFECT — in %u of %u round(s) a window of %s answered with "
               "bytes that are not its own, read while it was deleted and its ground written "
               "again\n", defect, rounds, QUILL_RD_NAME);
        return QUILL_DEFECT;
    }
    if (made == 0 && clean == rounds && asked > 0 && grc == 0 && code == 0 && arc == 0) {
        printf("[QUILL] readdelete: PASS — %u round(s) of %u MiB read in %u windows while another "
               "cabin deleted the file: every window the file's own bytes or a refusal as a file "
               "not found, and in %u round(s) the window being read outlived the delete, %u "
               "Settle cycles and a filler of the same size; anchored\n", rounds, mib,
               QUILL_RD_WINDOWS, asked, QUILL_RD_CYCLES);
        return QUILL_PASS;
    }
    printf("[QUILL] readdelete: FAIL — %u of %u round(s) as they must be, in %u the window being "
           "read outlived the delete and the filler%s; the round's file answered %d; the deleter "
           "ended %d with exit %d; anchor %d\n", clean, rounds, asked,
           asked ? "" : " — the case did not ask its question", made, grc, code, arc);
    return QUILL_FAIL;
}

/* =========================================================================
 *  defragdelete — defraggers going round while the slots are deleted and
 *  made again
 * ========================================================================= */

#define QUILL_DF_STEM       "_qd"
#define QUILL_DF_BOARD      "quilltest:defrag"
#define QUILL_DF_SLOTS      16u
#define QUILL_DF_ROUNDS     64u
#define QUILL_DF_DEFRAGGERS 2u
#define QUILL_DF_PIECE      4096u
#define QUILL_DF_MOST       ((192u << 10) + 4096u * 63u + 8u * 99u + 7u)
#define QUILL_DF_MAGIC      0x3147415246454451ull    /* "QDEFRAG1" */

/* What a slot's file says of itself in its first 32 bytes. */
typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t slot;
    uint32_t gen;
    uint64_t size;
    uint64_t check;
} QuillDfHead;

typedef struct {
    volatile uint32_t stop;
    volatile uint32_t slots;
    volatile file_id_t fid[QUILL_DF_SLOTS];
    volatile uint32_t passes[QUILL_DF_DEFRAGGERS];
    volatile uint32_t moved[QUILL_DF_DEFRAGGERS];   /* defrags answered with a score */
    volatile uint32_t gone[QUILL_DF_DEFRAGGERS];    /* ... with a file not found: deleted under it */
    volatile int32_t  refusal[QUILL_DF_DEFRAGGERS]; /* any other answer, the first */
} QuillDfBoard;

static uint64_t QuillDfSize(uint32_t s, uint32_t g)
{
    return (192u << 10) + 4096u * ((s * 37u + g * 11u) % 64u) + 8u * ((s + g) % 100u) + s % 8u;
}

/* The bytes of slot `s`'s file of gen `g`: its head, then its stream. */
static void QuillDfMake(uint64_t *buf, uint32_t s, uint32_t g)
{
    uint64_t    size = QuillDfSize(s, g);
    QuillDfHead head = { QUILL_DF_MAGIC, s, g, size, ((uint64_t)s << 32 | g) * 0x9E3779B97F4A7C15ull };
    QuillFill(buf, 0xD0u + s, (uint64_t)g << 40, (size + 7) & ~7ull);
    memcpy(buf, &head, sizeof(head));
}

/* The file `fid` read whole and held against slot `s`'s file of gen `g`: the
 * gen its head names (UINT32_MAX when it names none of this slot's) in *found,
 * and true when it is that file, byte for byte. */
static bool QuillDfIs(file_id_t fid, uint32_t s, uint32_t g, uint64_t *have, uint64_t *want,
                      uint32_t *found, uint64_t *size)
{
    *size  = QuillSize(fid);
    *found = UINT32_MAX;
    if (*size < sizeof(QuillDfHead) || *size > QUILL_DF_MOST)
        return false;
    int64_t got = fread(fid, 0, have, *size);
    if (got != (int64_t)*size)
        return false;
    QuillDfHead head;
    memcpy(&head, have, sizeof(head));
    if (head.magic == QUILL_DF_MAGIC && head.slot == s)
        *found = head.gen;
    QuillDfMake(want, s, g);
    return *size == QuillDfSize(s, g) && memcmp(have, want, *size) == 0;
}

QuillVerdict QuillDefragger(const char *const *args)
{
    uint32_t me = 0;
    QuillDfBoard *board = QuillNumber(args[0], &me) && me < QUILL_DF_DEFRAGGERS
                              ? bay_open(QUILL_DF_BOARD, 0, BAY_OPEN) : NULL;
    if (!board) {
        printf("[QUILL] defragger: FAIL — '%s' names no defragger of a running defragdelete "
               "(0 to %u, with its board open; nothing was defragged)\n", args[0],
               QUILL_DF_DEFRAGGERS - 1);
        return QUILL_FAIL;
    }
    int rc = 0;
    while (rc == 0 && !__atomic_load_n(&board->stop, __ATOMIC_ACQUIRE)) {
        for (uint32_t s = 0; s < board->slots && rc == 0; s++) {
            file_id_t fid = __atomic_load_n(&board->fid[s], __ATOMIC_ACQUIRE);
            if (fid == 0)
                continue;
            int d = defrag(fid);
            if (d >= 0)
                board->moved[me]++;
            else if (box_errno_of(d) == ERR_FILE_NOT_FOUND)
                board->gone[me]++;
            else
                rc = d;
        }
        board->passes[me]++;
    }
    board->refusal[me] = rc;
    uint32_t moved = board->moved[me], gone = board->gone[me], passes = board->passes[me];
    bay_release(board);
    if (rc != 0) {
        printf("[QUILL] defragger: FAIL — defragger %u: a defrag answered %d (error %u) after %u "
               "answered and %u found deleted\n", me, rc, box_errno_of(rc), moved, gone);
        return QUILL_FAIL;
    }
    printf("[QUILL] defragger: PASS — defragger %u went round the slots %u times: %u defrags "
           "answered, %u found the file deleted under them\n", me, passes, moved, gone);
    return QUILL_PASS;
}

/* Slots `s` and `s + 1` made at gen `g` and written 4 KiB at a time, in turn,
 * each published to the defraggers the moment it is made. 0, or the refusal. */
static int QuillDfMakePair(QuillDfBoard *b, uint32_t s, uint32_t g, uint64_t *bufs[2])
{
    file_id_t fids[2];
    uint64_t sizes[2];
    for (uint32_t j = 0; j < 2; j++) {
        char name[QUILL_NAME_CAP];
        QuillIndexedName(name, QUILL_DF_STEM, s + j);
        QuillDfMake(bufs[j], s + j, g);
        sizes[j] = QuillDfSize(s + j, g);
        int64_t fid  = QuillCreate(name);
        if (fid <= 0)
            return fid ? (int)fid : -1;
        fids[j] = (file_id_t)fid;
        __atomic_store_n(&b->fid[s + j], fids[j], __ATOMIC_RELEASE);
    }
    for (uint64_t off = 0; off < sizes[0] || off < sizes[1]; off += QUILL_DF_PIECE)
        for (uint32_t j = 0; j < 2; j++) {
            if (off >= sizes[j])
                continue;
            uint64_t n   = sizes[j] - off < QUILL_DF_PIECE ? sizes[j] - off : QUILL_DF_PIECE;
            int64_t  put = fwrite(fids[j], off, (uint8_t *)bufs[j] + off, n);
            if (put != (int64_t)n)
                return put < 0 ? (int)put : -1;
        }
    return 0;
}

/* Slots `s` and `s + 1` deleted: taken off the board, then given up. */
static int QuillDfDeletePair(QuillDfBoard *b, uint32_t s)
{
    for (uint32_t j = 0; j < 2; j++) {
        file_id_t fid = __atomic_exchange_n(&b->fid[s + j], 0u, __ATOMIC_ACQ_REL);
        int      rc  = fid ? delete(fid) : -1;
        if (rc != 0)
            return rc;
    }
    return 0;
}

static bool QuillDfArgs(const char *who, const char *const *args, uint32_t *slots, uint32_t *rounds)
{
    if (QuillNumber(args[0], slots) && *slots >= 2 && *slots <= QUILL_DF_SLOTS && *slots % 2 == 0 &&
        QuillNumber(args[1], rounds) && *rounds >= 1 && *rounds <= QUILL_DF_ROUNDS)
        return true;
    printf("[QUILL] %s: FAIL — SLOTS is an even 2 to %u and ROUNDS 1 to %u; given '%s %s'\n", who,
           QUILL_DF_SLOTS, QUILL_DF_ROUNDS, args[0], args[1]);
    return false;
}

/* Every slot made at gen 0, the defraggers started, and ROUNDS rounds of
 * every pair deleted and made again: 0, or the refusal that stopped it, with
 * *where saying what it was. */
static int QuillDfChurn(QuillDfBoard *b, uint32_t slots, uint32_t rounds, uint64_t *bufs[2],
                        QuillCabin cabins[QUILL_DF_DEFRAGGERS], uint32_t *started,
                        const char **where)
{
    int rc = 0;
    for (uint32_t s = 0; s < slots && rc == 0; s += 2)
        rc = QuillDfMakePair(b, s, 0, bufs);
    *where = "making gen 0";
    for (*started = 0; rc == 0 && *started < QUILL_DF_DEFRAGGERS; (*started)++) {
        char line[QUILL_LINE_CAP];
        QuillCabinLine(line, "defragger", *started, 0, false);
        if ((rc = QuillCabinStart(&cabins[*started], line)) <= 0) {
            *where = "starting a defragger";
            return rc ? rc : -1;
        }
        rc = 0;
    }
    for (uint32_t g = 1; g <= rounds && rc == 0; g++) {
        for (uint32_t s = 0; s < slots && rc == 0; s += 2) {
            *where = "deleting";
            if ((rc = QuillDfDeletePair(b, s)) == 0) {
                *where = "making";
                rc = QuillDfMakePair(b, s, g, bufs);
            }
        }
        uint32_t moved = 0, gone = 0;
        for (uint32_t d = 0; d < QUILL_DF_DEFRAGGERS; d++) {
            moved += b->moved[d];
            gone  += b->gone[d];
        }
        if (rc == 0)
            printf("[QUILL] defragdelete: round %u of %u — %u slots deleted and made again; the "
                   "defraggers have %u defrags answered and %u found deleted under them\n", g,
                   rounds, slots, moved, gone);
    }
    return rc;
}

QuillVerdict QuillDefragDelete(const char *const *args)
{
    uint32_t slots = 0, rounds = 0;
    if (!QuillDfArgs("defragdelete", args, &slots, &rounds))
        return QUILL_FAIL;
    QuillDfBoard *board = bay_open(QUILL_DF_BOARD, sizeof(QuillDfBoard), BAY_CREATE);
    uint64_t     *bufs[2] = { malloc(QUILL_DF_MOST + 8), malloc(QUILL_DF_MOST + 8) };
    if (!board || !bufs[0] || !bufs[1]) {
        printf("[QUILL] defragdelete: FAIL — its board and two buffers of %u bytes were not all "
               "had (nothing was made)\n", QUILL_DF_MOST);
        return QUILL_FAIL;
    }
    memset((void *)board, 0, sizeof(*board));
    board->slots = slots;

    QuillCabin  cabins[QUILL_DF_DEFRAGGERS];
    uint32_t    started = 0;
    const char *where   = "";
    int         rc      = QuillDfChurn(board, slots, rounds, bufs, cabins, &started, &where);
    __atomic_store_n(&board->stop, 1u, __ATOMIC_RELEASE);
    uint32_t failed = 0, moved = 0, gone = 0;
    for (uint32_t d = 0; d < started; d++) {
        int32_t code = 0;
        if (QuillCabinEnd(&cabins[d], &code) != 0 || code != 0)
            failed++;
        moved += board->moved[d];
        gone  += board->gone[d];
    }
    uint32_t whole = 0, bad = slots;
    for (uint32_t s = 0; s < slots && rc == 0; s++) {
        uint32_t found = 0;
        uint64_t size  = 0;
        if (QuillDfIs(board->fid[s], s, rounds, bufs[0], bufs[1], &found, &size))
            whole++;
        else if (bad == slots)
            bad = s;
    }
    bay_release(board);
    int arc = anchor(0);
    if (rc == 0 && started == QUILL_DF_DEFRAGGERS && failed == 0 && whole == slots && arc == 0) {
        printf("[QUILL] defragdelete: PENDING — %u slots deleted and made again %u times beside %u "
               "defraggers, which answered %u defrags and found the file deleted under them %u "
               "times, each as due; every slot holds its gen %u, read back whole; anchored — "
               "after a reboot, quilltest defragback %u %u reads them again\n", slots, rounds,
               QUILL_DF_DEFRAGGERS, moved, gone, rounds, slots, rounds);
        return QUILL_PENDING;
    }
    printf("[QUILL] defragdelete: FAIL — %s answered %d; %u of %u defraggers started, %u failed "
           "(%u defrags answered, %u found deleted); %u of %u slots read back whole, the first "
           "not slot %u; anchor %d\n", where, rc, started, QUILL_DF_DEFRAGGERS, failed, moved,
           gone, whole, slots, bad, arc);
    return QUILL_FAIL;
}

/* After the power button and a reboot: every slot one file, its last gen,
 * whole — a file of a gen it deleted, beside it or in its place, came back. */
QuillVerdict QuillDefragBack(const char *const *args)
{
    uint32_t slots = 0, rounds = 0;
    if (!QuillDfArgs("defragback", args, &slots, &rounds))
        return QUILL_FAIL;
    uint64_t *have = malloc(QUILL_DF_MOST + 8), *want = malloc(QUILL_DF_MOST + 8);
    if (!have || !want) {
        printf("[QUILL] defragback: FAIL — two buffers of %u bytes were not had\n", QUILL_DF_MOST);
        return QUILL_FAIL;
    }
    for (uint32_t s = 0; s < slots; s++) {
        char     name[QUILL_NAME_CAP];
        file_id_t ids[4];
        QuillIndexedName(name, QUILL_DF_STEM, s);
        int      n     = QuillFindEverywhere(name, ids, 4);
        uint32_t found = UINT32_MAX, other = UINT32_MAX;
        uint64_t size  = 0, osize = 0;
        bool     whole = n == 1 && QuillDfIs(ids[0], s, rounds, have, want, &found, &size);
        if (n == 2)
            (void)QuillDfIs(ids[1], s, rounds, have, want, &other, &osize);
        if (whole)
            continue;
        if (n > 1 || (n == 1 && found < rounds)) {
            printf("[QUILL] defragback: DEFECT — slot %u: %d file(s) called %s, the gens they say "
                   "%u and %u, where only gen %u is due — a file deleted before the power button "
                   "came back\n", s, n, name, found, other, rounds);
            return QUILL_DEFECT;
        }
        printf("[QUILL] defragback: FAIL — slot %u: %d file(s) called %s; the one there says gen "
               "%u in %llu bytes where gen %u in %llu bytes is due, and it is not that file\n", s,
               n, name, found, (unsigned long long)size, rounds,
               (unsigned long long)QuillDfSize(s, rounds));
        return QUILL_FAIL;
    }
    free(have);
    free(want);
    printf("[QUILL] defragback: PASS — after the reboot every one of the %u slots holds one file, "
           "its gen %u, read back whole: none of the %u files deleted came back\n", slots, rounds,
           slots * rounds);
    return QUILL_PASS;
}
