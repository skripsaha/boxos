/*
 * quilltest_locks.c — abba: the tag registry and the file table grown while
 * three cabins of the case's own rename, create and tag beside them, on every
 * core the machine has. A throwaway image's case (VOLUME_TESTS=on builds).
 *
 * ‼ TWO CHAINS TOOK THE STATE LOCK UNDER THEIR OWN, AND EVERY OTHER CALL TAKES
 * THEM UNDER IT.
 * A chain that grew took its new block under its own lock: the registry's
 * flush (tag_registry.c) and the file table's (file_table.c) each called
 * tagfs_alloc_blocks, which takes the volume's state lock, while they held
 * theirs. A rename, a create, a delete, a tag and an untag take the state lock
 * first and the table's — a rename the registry's as well — inside it. Every
 * TagFS lock keeps its core's interrupts off, so two K-Cores that met there
 * the two ways round each spun for good on the lock the other held: the two
 * ABBAs of MP-D4. Step 1.1 took the block out from under both locks — a Spare
 * is stamped with no lock held (scribe.h) — and LockRank's order line, the
 * evidence that does not depend on timing, must now never come. This case is
 * the other half: the machine asked to grow both chains on all of its cores
 * while the other calls run, and whether it comes back. It is a guard: it says
 * PASS or FAIL, and a machine that wedges says nothing at all, which the host
 * judges (logcheck abba).
 *
 * ‼ A DRIVER AND THREE WORKERS, EACH IN A CABIN OF ITS OWN.
 * A cabin's system calls are pockets, each served by one K-Core, the least
 * loaded taking the next: four cabins asking at once are up to four K-Cores
 * inside TagFS at once — the way four of a person's programs would be. The
 * driver, abba, grows the chains: files each wearing a tag no other file
 * wears, the registry flushed as each is made, and an anchor every
 * QUILL_ABBA_ANCHOR_EVERY of them, whose sync writes the file table — grown
 * once a file id past its last block has been handed out. The three workers,
 * abbaworker, are started by the driver, and each loops one kind of call on
 * files of the case's own that take the locks the other way round. They share
 * one Bay with it, the board — every call each worker made, its first
 * refusal, and the driver's word to stop — so a progress line of the
 * driver's says how far all four have gone, and the verdict can name any of
 * them. What the driver waits for — a worker's first calls, a worker's end —
 * is said on the board's tag in Touch, the one event one cabin sends another:
 * a park on a word of the Bay would not do, since addr_park keys a wait by the
 * parker's own address space (sync/addr_wait.h), and a wake from another
 * cabin never reaches it.
 *
 * ‼ AIMED BY THE HOST, AT BOTH EDGES.
 * TAGS is how many new tags to make and EDGE the first file id the file
 * table's chain on the medium had no row for, both where the boot's own
 * startup tests left the chains — the kernel says so after its last test, and
 * the host aims from that line (logcheck abba): TAGS past the room the
 * registry's last block has, and a file id at or past EDGE, grow both chains
 * while the workers run, the table by the block the next anchor's sync links
 * for that id. A file id spent at or past EDGE before the case grows anything
 * — by its own lay-down, or by a worker — only makes that sync come sooner.
 *
 * ‼ WHY ONLY A THROWAWAY IMAGE.
 * The case deletes nothing but files of its own names, but every tag it makes
 * stays in the registry for good — the registry keeps a tag whose last file
 * is gone — and every file id it and its create worker spend is spent for
 * good. See quilltest.c for the rest.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/bay.h"
#include "box/string.h"
#include "box/strand.h"
#include "box/system.h"
#include "box/time.h"
#include "box/touch.h"
#include "box/error.h"
#include "quilltest.h"

#ifdef CONFIG_VOLUME_TESTS

#define QUILL_ABBA_NAME_MAX     12u   /* NAME's bytes, so a grow name keeps 8 digits of index */
#define QUILL_ABBA_NAME_CAP     32u   /* the 31 bytes a file's name may have, and its NUL */
#define QUILL_ABBA_RUN_DIGITS   10u
#define QUILL_ABBA_PROGRESS     25u   /* a progress line every this many new tags */
#define QUILL_ABBA_ANCHOR_EVERY 20u   /* an anchor, and so a sync, every this many */
#define QUILL_ABBA_ROLES        3u

typedef enum { QUILL_ABBA_RENAME, QUILL_ABBA_CREATE, QUILL_ABBA_TAG } QuillAbbaRole;

static const char *const g_abba_roles[QUILL_ABBA_ROLES] = { "rename", "create", "tag" };

/* One worker's line on the board. Written by that worker alone. */
typedef struct {
    volatile uint32_t calls;       /* calls answered as due */
    volatile uint32_t refused_at;  /* the call, from 1, that was not; 0 while none was */
    volatile int32_t  refusal;     /* its answer */
} QuillAbbaCount;

/* The board: one Bay, the same memory in the driver's cabin and in every
 * worker's, and its tag, on which the workers say what the driver waits for. */
typedef struct {
    volatile uint32_t stop;        /* the driver's word: finish the pair of calls in hand and leave */
    QuillAbbaCount    counts[QUILL_ABBA_ROLES];
} QuillAbbaBoard;

/* A worker as the driver sees it: its incarnation, and how it ended, heard by
 * a strand parked on its end and said on the board's tag. */
typedef struct {
    uint32_t          pid, gen;
    int               grc;         /* process_gone's own answer */
    int32_t           code;        /* the worker's exit */
    volatile uint32_t gone;
    TouchTagPair      pair;
} QuillAbbaWatch;

/* What the driver made, and when it began, by the machine's clock. */
typedef struct {
    uint64_t t0;
    uint32_t  made, anchors, edge;
    file_id_t first_fid, last_fid;
    int      refusal;              /* the answer that stopped it, 0 when none did */
    const char *refused;           /* what stopped it: "create", "anchor", "names", or NULL */
    char     name[QUILL_ABBA_NAME_CAP];
} QuillAbbaGrowth;

static QuillAbbaWatch g_abba_watch[QUILL_ABBA_ROLES];
static uint32_t       g_abba_watcher[QUILL_ABBA_ROLES];

/* =========================================================================
 *  Names, and the board
 * ========================================================================= */

/* NAME is 1 to QUILL_ABBA_NAME_MAX letters, digits, '_' or '-': every name
 * made from it is one word on a command line and one tag key, with no dot to
 * cut a stem short and no colon to read as a value. */
static bool QuillAbbaNameOk(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len > QUILL_ABBA_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* NAME and a tail: _qa + "ra" is _qara. */
static void QuillAbbaName(char out[QUILL_ABBA_NAME_CAP], const char *name, const char *tail)
{
    size_t n = strlen(name), t = strlen(tail);
    memcpy(out, name, n);
    memcpy(out + n, tail, t + 1);
}

/* The index'th new tag's name: NAME, 'g', the run's number in ten digits, and
 * the index in the digits left to 31 bytes — every one as long as a name may
 * be, so each is the biggest registry record a name makes (6 bytes and 31)
 * and the host counts how many fill a block. The run's number is the file id
 * of its rename file, which no earlier run was given, so every run's tags are
 * new — ten digits carry every id below 10^10, past every 32-bit id a format v1
 * volume issues. False once the index no longer fits. */
static bool QuillAbbaGrowName(char out[QUILL_ABBA_NAME_CAP], const char *name, file_id_t run,
                              uint32_t index)
{
    size_t n = strlen(name);
    size_t width = QUILL_ABBA_NAME_CAP - 1 - n - 1 - QUILL_ABBA_RUN_DIGITS;
    memcpy(out, name, n);
    out[n] = 'g';
    for (size_t d = QUILL_ABBA_RUN_DIGITS; d > 0; d--, run /= 10)
        out[n + d] = (char)('0' + run % 10);
    char *at = out + n + 1 + QUILL_ABBA_RUN_DIGITS;
    for (size_t d = width; d > 0; d--, index /= 10)
        at[d - 1] = (char)('0' + index % 10);
    at[width] = '\0';
    return index == 0;
}

static void QuillAbbaBoardTag(char out[48], const char *name)
{
    memcpy(out, "quilltest:abba:", 15);
    memcpy(out + 15, name, strlen(name) + 1);
}

/* Something the driver waits for has happened to ROLE's worker: said on the
 * board's tag, after the board or the watch already shows it. */
static void QuillAbbaTell(TouchTagPair pair, uint32_t role)
{
    touch_send(pair, &role, sizeof(role), 0);
}

/* =========================================================================
 *  abbaworker — one kind of call, looped until the driver says stop
 * ========================================================================= */

/* One pair of calls of ROLE on the worker's file — one call, and the one that
 * undoes it, so the file is as it was between two pairs. 0, or the answer of
 * the call that was not as due, *which saying which of the two it was. */
static int QuillAbbaPair(QuillAbbaRole role, file_id_t fid, const char *a, const char *b,
                         uint32_t *which)
{
    int rc;
    *which = 0;
    switch (role) {
    case QUILL_ABBA_RENAME:
        rc = file_rename(fid, b);
        if (rc != 0)
            return rc;
        *which = 1;
        return file_rename(fid, a);
    case QUILL_ABBA_CREATE: {
        int64_t made = create_everywhere(a, NULL);
        if (made <= 0)
            return made ? (int)made : -1;
        *which = 1;
        return delete((uint32_t)made);
    }
    case QUILL_ABBA_TAG:
        rc = tag_add(fid, b);
        if (rc != 0)
            return rc;
        *which = 1;
        return tag_remove(fid, b);
    }
    return -1;
}

/* The worker's two names, and the file id of the file it works on (0 for
 * create, which makes its own): rename turns NAMEra into NAMErb and back,
 * create makes NAMEc.tmp and deletes it, tag puts NAMEtk on NAMEt and takes
 * it off. The driver laid all of them down and put every stem and the tag in
 * the registry first, so no call of a worker's adds a record to it. */
static int QuillAbbaWorkerFile(QuillAbbaRole role, const char *name, char a[QUILL_ABBA_NAME_CAP],
                               char b[QUILL_ABBA_NAME_CAP], file_id_t *fid)
{
    static const char *const tails[QUILL_ABBA_ROLES][2] = {
        { "ra", "rb" }, { "c.tmp", "" }, { "t", "tk" },
    };
    QuillAbbaName(a, name, tails[role][0]);
    QuillAbbaName(b, name, tails[role][1]);
    *fid = 0;
    if (role == QUILL_ABBA_CREATE)
        return 1;
    return QuillFindEverywhere(a, fid, 1);
}

QuillVerdict QuillAbbaWorker(const char *const *args)
{
    const char *name = args[0];
    uint32_t    role = QUILL_ABBA_ROLES;
    for (uint32_t r = 0; r < QUILL_ABBA_ROLES; r++)
        if (args[1] && strcmp(args[1], g_abba_roles[r]) == 0)
            role = r;
    char tag[48];
    QuillAbbaBoard *board = NULL;
    if (QuillAbbaNameOk(name) && role < QUILL_ABBA_ROLES) {
        QuillAbbaBoardTag(tag, name);
        board = bay_open(tag, 0, BAY_OPEN);
    }
    if (!board) {
        printf("[QUILL] abbaworker: FAIL — '%s %s' names no worker of a running abba: NAME as abba "
               "was given it and ROLE rename, create or tag, with abba's board open (0 calls "
               "made)\n", name, args[1]);
        return QUILL_FAIL;
    }

    /* The driver hears this worker's first calls on the board's tag, and its
     * end from a watch of its own; a worker that could not say the first
     * leaves at once, and the end says it for it. */
    QuillAbbaCount *count = &board->counts[role];
    TouchTagPair    pair  = touch_intern(tag);
    char a[QUILL_ABBA_NAME_CAP], b[QUILL_ABBA_NAME_CAP];
    file_id_t fid   = 0;
    uint32_t  which = 0;
    int n = QuillAbbaWorkerFile((QuillAbbaRole)role, name, a, b, &fid);
    bool heard = touch_pair_choose(pair) != TOUCH_TAG_INVALID;
    int  rc = !heard ? -1 : n == 1 ? 0 : (n < 0 ? n : -1);
    uint32_t calls = 0;
    while (rc == 0 && !__atomic_load_n(&board->stop, __ATOMIC_ACQUIRE)) {
        rc = QuillAbbaPair((QuillAbbaRole)role, fid, a, b, &which);
        calls += rc == 0 ? 2 : which;
        __atomic_store_n(&count->calls, calls, __ATOMIC_RELEASE);
        if (calls == 2 && rc == 0)
            QuillAbbaTell(pair, role);
    }
    if (rc != 0) {
        __atomic_store_n(&count->refusal, rc, __ATOMIC_RELAXED);
        __atomic_store_n(&count->refused_at, calls + 1, __ATOMIC_RELEASE);
    }
    bay_release(board);

    if (!heard) {
        printf("[QUILL] abbaworker: FAIL — %s: the board's tag %s would not intern, so abba could "
               "not hear it (0 calls made)\n", g_abba_roles[role], tag);
        return QUILL_FAIL;
    }
    if (n != 1) {
        printf("[QUILL] abbaworker: FAIL — %s: the volume has %d file(s) called %s where abba laid "
               "down one (0 calls made)\n", g_abba_roles[role], n, a);
        return QUILL_FAIL;
    }
    if (rc != 0) {
        printf("[QUILL] abbaworker: FAIL — %s on %s: call %u answered %d, after %u answered as due\n",
               g_abba_roles[role], a, calls + 1, rc, calls);
        return QUILL_FAIL;
    }
    printf("[QUILL] abbaworker: PASS — %s on %s: %u calls, each answered as due, until abba said "
           "stop\n", g_abba_roles[role], a, calls);
    return QUILL_PASS;
}

/* =========================================================================
 *  abba — the driver
 * ========================================================================= */

/* The board opened and cleared, and its tag claimed before any worker can say
 * anything on it. NULL, having said why, when either would not. */
static QuillAbbaBoard *QuillAbbaBoardUp(const char *tag, TouchTagPair *pair, TouchTag *heard)
{
    QuillAbbaBoard *board = bay_open(tag, sizeof(QuillAbbaBoard), BAY_CREATE);
    *pair  = touch_intern(tag);
    *heard = touch_pair_choose(*pair);
    int crc = *heard != TOUCH_TAG_INVALID ? touch_claim(*heard, TOUCH_REST, 0, 0) : -1;
    if (board && crc == 0) {
        memset((void *)board, 0, sizeof(*board));
        return board;
    }
    if (crc == 0)
        touch_release(*heard);
    if (board)
        bay_release(board);
    printf("[QUILL] abba: FAIL — its board %s would not stand: the Bay %s, and its tag %u answered "
           "the claim %d (nothing was made)\n", tag, board ? "opened" : "would not open",
           (uint32_t)*heard, crc);
    return NULL;
}

static void QuillAbbaBoardDown(QuillAbbaBoard *board, TouchTag heard)
{
    touch_release(heard);
    bay_release(board);
}

/* The workers' files laid down, and every stem and the tag they use put in
 * the registry. Returns the run's number — the rename file's id, which no
 * earlier run was given — and in *highest the last file id this gave out; 0
 * with *what saying which name would not be laid down, *rc its answer. */
static file_id_t QuillAbbaLayDown(const char *name, file_id_t *highest, char what[QUILL_ABBA_NAME_CAP],
                                 int *rc)
{
    static const char *const made[] = { "rb", "c.tmp", "ra", "t" };
    file_id_t fids[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 4; i++) {
        QuillAbbaName(what, name, made[i]);
        int64_t fid = QuillCreate(what);
        if (fid <= 0) {
            *rc = (int)fid;
            return 0;
        }
        fids[i] = (file_id_t)fid;
    }
    /* NAMErb and NAMEc.tmp only had to put their stems in the registry. */
    for (uint32_t i = 0; i < 2; i++) {
        QuillAbbaName(what, name, made[i]);
        if ((*rc = delete(fids[i])) != 0)
            return 0;
    }
    QuillAbbaName(what, name, "tk");
    if ((*rc = tag_add(fids[3], what)) != 0 || (*rc = tag_remove(fids[3], what)) != 0)
        return 0;
    *highest = fids[3];
    return fids[2];
}

/* A worker's end, heard: process_gone asked as a state, so a worker already
 * gone answers at once. */
static void QuillAbbaWatchOne(void *arg)
{
    QuillAbbaWatch *w = arg;
    w->grc = process_gone(w->pid, w->gen, &w->code);
    __atomic_store_n(&w->gone, 1u, __ATOMIC_RELEASE);
    QuillAbbaTell(w->pair, (uint32_t)(w - g_abba_watch));
}

/* True once every started worker has made its first calls, refused, or gone. */
static bool QuillAbbaAllStarted(const QuillAbbaBoard *board, uint32_t started)
{
    for (uint32_t r = 0; r < started; r++)
        if (__atomic_load_n(&board->counts[r].calls, __ATOMIC_ACQUIRE) == 0 &&
            __atomic_load_n(&board->counts[r].refused_at, __ATOMIC_ACQUIRE) == 0 &&
            !__atomic_load_n(&g_abba_watch[r].gone, __ATOMIC_ACQUIRE))
            return false;
    return true;
}

static bool QuillAbbaAllGone(const QuillAbbaBoard *board, uint32_t started)
{
    (void)board;
    for (uint32_t r = 0; r < started; r++)
        if (!__atomic_load_n(&g_abba_watch[r].gone, __ATOMIC_ACQUIRE))
            return false;
    return true;
}

/* Wait on the board's tag until `done` holds of the first `started` workers.
 * Every event it waits for is said on the tag after the board or the watch
 * shows it, and the tag keeps what it is told until it is asked: a look that
 * finds `done` false waits only while nothing has been said since. */
static void QuillAbbaAwait(TouchTag heard, const QuillAbbaBoard *board, uint32_t started,
                           bool (*done)(const QuillAbbaBoard *, uint32_t))
{
    Touch said;
    while (!done(board, started))
        touch_await(heard, &said, 0);
}

/* The workers started, each with a strand parked on its end. Returns how
 * many run and are watched; *rc is the answer that stopped the next one —
 * proc_exec_gen's, or, when its watch would not start, that of the
 * proc_finish that ended it, with *unwatched set. */
static uint32_t QuillAbbaStart(TouchTagPair pair, const char *name, int *rc, bool *unwatched)
{
    uint32_t r = 0;
    *unwatched = false;
    for (; r < QUILL_ABBA_ROLES; r++) {
        char line[64];
        memcpy(line, "quilltest abbaworker ", 21);
        size_t n = strlen(name);
        memcpy(line + 21, name, n);
        line[21 + n] = ' ';
        memcpy(line + 22 + n, g_abba_roles[r], strlen(g_abba_roles[r]) + 1);
        uint32_t gen = 0;
        *rc = proc_exec_gen(line, NULL, &gen);
        if (*rc <= 0)
            return r;
        g_abba_watch[r] = (QuillAbbaWatch){ .pid = (uint32_t)*rc, .gen = gen, .pair = pair };
        g_abba_watcher[r] = strand_spawn_joinable(QuillAbbaWatchOne, &g_abba_watch[r]);
        if (!g_abba_watcher[r]) {
            *rc = proc_finish((uint32_t)*rc, gen);
            *unwatched = true;
            return r;
        }
    }
    return r;
}

/* The driver's word to stop, and every started worker gone. */
static void QuillAbbaStop(QuillAbbaBoard *board, TouchTag heard, uint32_t started)
{
    __atomic_store_n(&board->stop, 1u, __ATOMIC_RELEASE);
    QuillAbbaAwait(heard, board, started, QuillAbbaAllGone);
    for (uint32_t r = 0; r < started; r++)
        if (g_abba_watcher[r])
            strand_release(g_abba_watcher[r]);
}

/* A worker that did not do its part: refused a call, left before the driver
 * said stop, or never got to its first call. Its index, or -1 for none. */
static int QuillAbbaFailed(const QuillAbbaBoard *board, uint32_t started, bool stopped)
{
    for (uint32_t r = 0; r < started; r++) {
        const QuillAbbaCount *c = &board->counts[r];
        if (c->refused_at || c->calls == 0 ||
            (!stopped && __atomic_load_n(&g_abba_watch[r].gone, __ATOMIC_ACQUIRE)) ||
            (stopped && (g_abba_watch[r].grc != 0 || g_abba_watch[r].code != 0)))
            return (int)r;
    }
    return -1;
}

/* Milliseconds since the driver began: every progress line says when it was
 * printed, by the machine's own clock, so the host can time the run's pace
 * even when a burst of lines reaches the console at once. */
static uint64_t QuillAbbaSince(const QuillAbbaGrowth *g)
{
    uint64_t now = 0;
    time_uptime_ms(&now);
    return now - g->t0;
}

static void QuillAbbaProgress(const QuillAbbaBoard *board, const QuillAbbaGrowth *g, uint32_t tags)
{
    printf("[QUILL] abba: %u of %u new tags made at %llu ms, the last at fid %lu (the table's edge "
           "at fid %u), %u anchor(s); rename %u, create %u, tag %u calls\n", g->made, tags,
           (unsigned long long)QuillAbbaSince(g), g->last_fid, g->edge, g->anchors,
           board->counts[QUILL_ABBA_RENAME].calls, board->counts[QUILL_ABBA_CREATE].calls,
           board->counts[QUILL_ABBA_TAG].calls);
}

/* The chains grown: new tags until `tags` are made and one of them was given
 * a file id past the edge, an anchor every QUILL_ABBA_ANCHOR_EVERY, and one
 * more once they are all made, while the workers still run — the sync that
 * grows the table if no earlier one has. Stops at the first refusal, of its
 * own or a worker's. */
static void QuillAbbaGrow(QuillAbbaBoard *board, const char *name, file_id_t run, uint32_t tags,
                          QuillAbbaGrowth *g)
{
    while (g->made < tags || g->last_fid < g->edge) {
        if (!QuillAbbaGrowName(g->name, name, run, g->made)) {
            g->refused = "names";
            return;
        }
        int64_t fid = create_everywhere(g->name, NULL);
        if (fid <= 0) {
            g->refusal = fid ? (int)fid : -1;
            g->refused = "create";
            return;
        }
        g->made++;
        g->first_fid = g->first_fid ? g->first_fid : (file_id_t)fid;
        g->last_fid = (file_id_t)fid;
        if (g->made % QUILL_ABBA_ANCHOR_EVERY == 0) {
            if ((g->refusal = anchor(0)) != 0) {
                g->refused = "anchor";
                return;
            }
            g->anchors++;
        }
        if (g->made % QUILL_ABBA_PROGRESS == 0) {
            QuillAbbaProgress(board, g, tags);
            if (QuillAbbaFailed(board, QUILL_ABBA_ROLES, false) >= 0)
                return;
        }
    }
    if ((g->refusal = anchor(0)) != 0) {
        g->refused = "anchor";
        return;
    }
    g->anchors++;
}

/* The driver's verdict: every worker's calls, its own growth, and the
 * anchor after the workers were gone. */
static QuillVerdict QuillAbbaSay(const QuillAbbaBoard *board, const QuillAbbaGrowth *g,
                                 uint32_t tags, int final, uint64_t ms)
{
    const QuillAbbaCount *c = board->counts;
    int bad = QuillAbbaFailed(board, QUILL_ABBA_ROLES, true);
    if (!g->refused && bad < 0 && final == 0) {
        printf("[QUILL] abba: PASS — made %u new tags, fids %lu..%lu, the last at or past the table's "
               "edge at fid %u, and %u anchors while rename answered %u calls, create %u and tag "
               "%u, each in a cabin of its own and each as due; anchored, %llu ms in all\n", g->made,
               g->first_fid, g->last_fid, g->edge, g->anchors, c[QUILL_ABBA_RENAME].calls,
               c[QUILL_ABBA_CREATE].calls, c[QUILL_ABBA_TAG].calls, (unsigned long long)ms);
        return QUILL_PASS;
    }
    if (g->refused && strcmp(g->refused, "names") == 0)
        printf("[QUILL] abba: FAIL — new tag %u has no name of 31 bytes left, and fid %lu was still "
               "short of the table's edge at fid %u; rename %u calls, create %u, tag %u (anchor "
               "after %d)\n", g->made + 1, g->last_fid, g->edge, c[QUILL_ABBA_RENAME].calls,
               c[QUILL_ABBA_CREATE].calls, c[QUILL_ABBA_TAG].calls, final);
    else if (g->refused)
        printf("[QUILL] abba: FAIL — %s answered %d at new tag %u of %u (%s), after %u anchors; "
               "rename %u calls, create %u, tag %u (anchor after %d)\n", g->refused, g->refusal,
               g->made + 1, tags, g->name, g->anchors, c[QUILL_ABBA_RENAME].calls,
               c[QUILL_ABBA_CREATE].calls, c[QUILL_ABBA_TAG].calls, final);
    else if (bad >= 0)
        printf("[QUILL] abba: FAIL — the %s worker (pid %u) made %u calls, its call %u answered "
               "%d, and it ended with exit %d (gone %d), while %u new tags were made and %u "
               "anchors (anchor after %d)\n", g_abba_roles[bad], g_abba_watch[bad].pid,
               c[bad].calls, c[bad].refused_at, c[bad].refusal, g_abba_watch[bad].code,
               g_abba_watch[bad].grc, g->made, g->anchors, final);
    else
        printf("[QUILL] abba: FAIL — %u new tags and %u anchors made beside every worker as due, "
               "and the anchor after them answered %d\n", g->made, g->anchors, final);
    return QUILL_FAIL;
}

QuillVerdict QuillAbba(const char *const *args)
{
    const char *name = args[0];
    uint32_t    tags = 0, edge = 0;
    if (!QuillAbbaNameOk(name) || !QuillNumber(args[1], &tags) || tags == 0 ||
        !QuillNumber(args[2], &edge) || edge == 0) {
        printf("[QUILL] abba: FAIL — NAME is 1 to %u letters, digits, '_' or '-', TAGS a count "
               "above 0 and EDGE a file id above 0; '%s', '%s' and '%s' are not (nothing was "
               "made)\n", QUILL_ABBA_NAME_MAX, name, args[1], args[2]);
        return QUILL_FAIL;
    }
    char         tag[48];
    TouchTagPair pair;
    TouchTag     heard;
    QuillAbbaBoardTag(tag, name);
    QuillAbbaBoard *board = QuillAbbaBoardUp(tag, &pair, &heard);
    if (!board)
        return QUILL_FAIL;

    QuillAbbaGrowth g;
    memset(&g, 0, sizeof(g));
    time_uptime_ms(&g.t0);
    file_id_t highest = 0;
    int       rc = 0;
    file_id_t run = QuillAbbaLayDown(name, &highest, g.name, &rc);
    if (!run) {
        int arc = anchor(0);
        QuillAbbaBoardDown(board, heard);
        printf("[QUILL] abba: FAIL — %s would not be laid down for its worker (answer %d); no "
               "worker started (anchor %d)\n", g.name, rc, arc);
        return QUILL_FAIL;
    }
    g.edge = edge;

    bool     unwatched = false;
    uint32_t started = QuillAbbaStart(pair, name, &rc, &unwatched);
    if (started == QUILL_ABBA_ROLES)
        QuillAbbaAwait(heard, board, started, QuillAbbaAllStarted);
    if (started < QUILL_ABBA_ROLES || QuillAbbaFailed(board, started, false) >= 0) {
        QuillAbbaStop(board, heard, started);
        int arc = anchor(0);
        if (started < QUILL_ABBA_ROLES)
            printf("[QUILL] abba: FAIL — the %s worker %s (answer %d); the %u started before it "
                   "were stopped, and nothing was grown (anchor %d)\n", g_abba_roles[started],
                   unwatched ? "started and its watch would not, so it was ended"
                             : "would not start", rc, started, arc);
        else
            QuillAbbaSay(board, &g, tags, arc, 0);
        QuillAbbaBoardDown(board, heard);
        return QUILL_FAIL;
    }
    printf("[QUILL] abba: workers rename pid %u, create pid %u, tag pid %u have each answered "
           "their first calls at %llu ms; %u new tags to make from fid %lu, the table's edge at fid "
           "%u\n", g_abba_watch[QUILL_ABBA_RENAME].pid, g_abba_watch[QUILL_ABBA_CREATE].pid,
           g_abba_watch[QUILL_ABBA_TAG].pid, (unsigned long long)QuillAbbaSince(&g), tags,
           highest + 1, g.edge);

    QuillAbbaGrow(board, name, run, tags, &g);
    QuillAbbaStop(board, heard, started);
    int arc = anchor(0);
    QuillVerdict v = QuillAbbaSay(board, &g, tags, arc, QuillAbbaSince(&g));
    QuillAbbaBoardDown(board, heard);
    return v;
}

#endif /* CONFIG_VOLUME_TESTS */
