/*
 * quilltest_turns.c — the machine's turns, shared (RT-11): a write and its
 * anchor beside cabins that would keep the kernel's time to themselves.
 *
 * ‼ THE BOUNDARY IS THE POCKET, NOT THE PROGRAM.
 * A K-Core served one program's ring until it was empty, and a program that
 * hands pockets in faster than they are served never let it be: every other
 * program's pocket queued behind it, the drain core ran no baton — the
 * answer a write waits for — and the committer's core no Settle cycle. flood
 * is that program: the cheapest fire-and-forget call there is, handed in for
 * as long as it lives. And on one core the scheduler gave the idle context —
 * where a write's next window runs — a quantum beside a program that never
 * parks only while a Settle cycle was owed: hog (quilltest_powercut.c) is
 * that program.
 *
 * ‼ ALONE, THEN BESIDE THEM, AND HOW LONG EACH TOOK.
 * beside writes a file of its own and anchors it twice: once alone, and once
 * with COUNT cabins of KIND running — a write that needs the machine's turns,
 * its windows' service points or its blocks' answers, and an anchor that
 * needs a Settle cycle. It says both times, and how many calls the floods had
 * answered meanwhile, so the host can judge that both kept going; a machine
 * that gives its turns to the floods alone says nothing at all, which the
 * host judges too (logcheck turns). The floods count on a board both see — a
 * Bay, the same memory in every cabin.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/bay.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/system.h"
#include "box/time.h"
#include "box/error.h"
#include "box/core/manifest.h"
#include "box/core/result.h"
#include "box/core/cabin.h"
#include "quilltest.h"

#define QUILL_TURNS_BOARD    "quilltest:turns"
#define QUILL_TURNS_MAX      16u        /* floods a board has room for */
#define QUILL_TURNS_MIB_MAX  128u       /* the largest write beside them, in MiB */
#define QUILL_TURNS_SEED     0x7E5u
#define QUILL_TURNS_TIMER    0x11       /* the hardware deck's milliseconds, as time.c names it */

/* What every flood has had answered, each in its own slot. */
typedef struct {
    volatile uint64_t answered[QUILL_TURNS_MAX];
} QuillTurnsBoard;

/* =========================================================================
 *  flood — calls without end
 * ========================================================================= */

/* The call, made once: the timer's milliseconds into a word of the flood's
 * own. Static, all of it: the kernel reads the crate and writes the word
 * after the submit has returned. */
static uint8_t  g_flood_manifest[64];
static Crate    g_flood_crate;
static uint64_t g_flood_ms;

QuillVerdict QuillFlood(const char *const *args)
{
    uint32_t         slot  = QUILL_TURNS_MAX;
    QuillTurnsBoard *board = NULL;
    ManifestBuilder  mb;
    if (QuillNumber(args[0], &slot) && slot < QUILL_TURNS_MAX)
        board = bay_open(QUILL_TURNS_BOARD, 0, BAY_OPEN);
    CrateSetOutput(&g_flood_crate, &g_flood_ms, sizeof(g_flood_ms));
    bool made = ManifestBuilderInit(&mb, g_flood_manifest, sizeof(g_flood_manifest)) == 0 &&
                ManifestBuilderAddOp(&mb, DECK_HARDWARE, QUILL_TURNS_TIMER, 0, CRATE_INDEX_NONE,
                                     0, NULL, 0) == 0 &&
                ManifestBuilderFinalize(&mb) == 0;
    if (!board || !made) {
        printf("[QUILL] flood: FAIL — slot '%s' of beside's board %s (board %s, call %s), so "
               "nothing was handed in\n", args[0], QUILL_TURNS_BOARD,
               board ? "open" : "not open", made ? "made" : "not made");
        if (board)
            bay_release(board);
        return QUILL_FAIL;
    }

    printf("[QUILL] flood: pid %u hands the kernel calls without waiting for them, without end "
           "(slot %u)\n", cabin_info()->pid, slot);
    uint64_t answered = 0;
    for (;;) {
        /* A full ring is waited out inside the submit, never refused. */
        (void)ManifestSubmitNoWait((const Manifest *)g_flood_manifest, &g_flood_crate, 1, 0);
        Result r;
        while (result_pop(&r))
            answered++;
        __atomic_store_n(&board->answered[slot], answered, __ATOMIC_RELEASE);
    }
}

/* =========================================================================
 *  beside — a write and its anchor, alone and beside them
 * ========================================================================= */

typedef struct {
    int64_t  wrote;
    int      anchored;
    uint64_t ms;
} QuillTurnsWrite;

/* The whole buffer written over the file from its first byte, and anchored. */
static QuillTurnsWrite QuillTurnsWriteOnce(file_id_t fid, const uint64_t *buf, uint64_t bytes)
{
    QuillTurnsWrite w = { 0, -1, 0 };
    uint64_t t0 = 0, t1 = 0;
    time_uptime_ms(&t0);
    w.wrote = fwrite(fid, 0, buf, bytes);
    if (w.wrote == (int64_t)bytes)
        w.anchored = anchor(fid);
    time_uptime_ms(&t1);
    w.ms = t1 - t0;
    return w;
}

static bool QuillTurnsWhole(const QuillTurnsWrite *w, uint64_t bytes)
{
    return w->wrote == (int64_t)bytes && w->anchored == 0;
}

typedef struct {
    uint32_t pid[QUILL_TURNS_MAX];
    uint32_t gen[QUILL_TURNS_MAX];
    uint32_t started;
} QuillTurnsCabins;

/* COUNT cabins of `quilltest KIND`, a flood with its board slot; how many
 * started. */
static void QuillTurnsStart(QuillTurnsCabins *c, bool flood, uint32_t count)
{
    c->started = 0;
    for (uint32_t i = 0; i < count; i++) {
        char line[32] = "quilltest hog";
        if (flood) {
            memcpy(line, "quilltest flood ", 16);
            uint_to_str(i, line + 16, sizeof(line) - 16);
        }
        int pid = proc_exec_gen(line, NULL, &c->gen[i]);
        if (pid <= 0)
            return;
        c->pid[i] = (uint32_t)pid;
        c->started++;
    }
}

/* Every cabin it started finished; how many were refused. */
static uint32_t QuillTurnsFinish(const QuillTurnsCabins *c)
{
    uint32_t refused = 0;
    for (uint32_t i = 0; i < c->started; i++)
        if (proc_finish(c->pid[i], c->gen[i]) != 0)
            refused++;
    return refused;
}

/* The fewest calls one flood had answered between `before` and now. */
static uint64_t QuillTurnsLeast(const QuillTurnsBoard *board, const uint64_t *before,
                                uint32_t count)
{
    uint64_t least = UINT64_MAX;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t got = __atomic_load_n(&board->answered[i], __ATOMIC_ACQUIRE) - before[i];
        if (got < least)
            least = got;
    }
    return count ? least : 0;
}

QuillVerdict QuillBeside(const char *const *args)
{
    bool     flood = strcmp(args[0], "flood") == 0;
    uint32_t count = 0, mib = 0;
    if ((!flood && strcmp(args[0], "hog") != 0) || !QuillNumber(args[1], &count) || count == 0 ||
        count > QUILL_TURNS_MAX || !QuillNumber(args[2], &mib) || mib == 0 ||
        mib > QUILL_TURNS_MIB_MAX) {
        printf("[QUILL] beside: FAIL — KIND is flood or hog, COUNT 1 to %u and MIB 1 to %u; "
               "given '%s %s %s'\n", QUILL_TURNS_MAX, QUILL_TURNS_MIB_MAX, args[0], args[1],
               args[2]);
        return QUILL_FAIL;
    }

    uint64_t         bytes = (uint64_t)mib << 20;
    QuillTurnsBoard *board = bay_open(QUILL_TURNS_BOARD, sizeof(QuillTurnsBoard), BAY_CREATE);
    uint64_t        *buf   = bay_open("quilltest:beside", bytes, BAY_CREATE);
    int64_t          fid   = (board && buf) ? QuillCreate("quillbeside.bin") : 0;
    if (fid <= 0) {
        printf("[QUILL] beside: FAIL — its board %s, its buffer of %u MiB %s, and "
               "quillbeside.bin answered %ld\n", board ? "opened" : "would not open", mib,
               buf ? "opened" : "would not open", fid);
        if (buf)
            bay_release(buf);
        if (board)
            bay_release(board);
        return QUILL_FAIL;
    }
    memset((void *)board, 0, sizeof(*board));
    QuillFill(buf, QUILL_TURNS_SEED, 0, bytes);

    QuillTurnsWrite alone = QuillTurnsWriteOnce((file_id_t)fid, buf, bytes);

    QuillTurnsCabins cabins;
    uint64_t         before[QUILL_TURNS_MAX] = { 0 };
    QuillTurnsStart(&cabins, flood, count);
    for (uint32_t i = 0; i < cabins.started; i++)
        before[i] = __atomic_load_n(&board->answered[i], __ATOMIC_ACQUIRE);
    printf("[QUILL] beside: %u %s cabin(s) of %u started; the write goes again beside them\n",
           cabins.started, args[0], count);
    QuillTurnsWrite beside = QuillTurnsWriteOnce((file_id_t)fid, buf, bytes);
    uint64_t        least  = flood ? QuillTurnsLeast(board, before, cabins.started) : 0;
    uint32_t        stuck  = QuillTurnsFinish(&cabins);
    bay_release(buf);
    bay_release(board);

    uint64_t size = QuillSize((file_id_t)fid);
    int64_t  rerr = 0;
    uint64_t good = size == bytes ? QuillReadBack((file_id_t)fid, QUILL_TURNS_SEED, size, &rerr) : 0;
    delete((file_id_t)fid);
    int arc = anchor(0);

    bool whole = cabins.started == count && stuck == 0 && QuillTurnsWhole(&alone, bytes) &&
                 QuillTurnsWhole(&beside, bytes) && good == bytes && arc == 0 &&
                 (!flood || least > 0);
    /* The floods' share, said only of floods: a hog answers no call. */
    char floods[96] = "";
    if (flood) {
        char n[24];
        strcpy(floods, ", each flood answered at least ");
        strcat(floods, uint64_to_str(least, n, sizeof(n)));
        strcat(floods, " call(s) meanwhile");
    }
    printf("[QUILL] beside: %s — %u MiB written and anchored in %llu ms alone and in %llu ms "
           "beside %u %s cabin(s)%s; read back good to byte %llu of %llu (writes %lld and %lld, "
           "anchors %d and %d, read %lld, %u cabin(s) would not finish, anchor %d)\n",
           whole ? "PASS" : "FAIL", mib, (unsigned long long)alone.ms,
           (unsigned long long)beside.ms, cabins.started, args[0], floods,
           (unsigned long long)good, (unsigned long long)bytes, (long long)alone.wrote,
           (long long)beside.wrote, alone.anchored, beside.anchored, (long long)rerr, stuck, arc);
    return whole ? QUILL_PASS : QUILL_FAIL;
}
