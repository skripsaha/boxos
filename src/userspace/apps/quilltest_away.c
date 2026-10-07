/*
 * quilltest_away.c — away: the medium a volume lives on leaves while a write
 * is in flight and writers wait behind it for the file's Helm (RT-13), and
 * comes back (logcheck replugwrite).
 *
 * ‼ ONE HOLDER ON THE MEDIUM, THE REST IN ITS FIFO.
 * away lays NAME down — one block of its own stream, anchored, so every write
 * the volume saw is kept by a flush before anything is in flight — and starts
 * COUNT cabins of awaywriter, each appending QUILL_AWAY_MIB MiB of a stream of
 * its own to NAME in one write. The first to take NAME's Helm writes through
 * the port while the others wait in its FIFO; the host has the medium's writes
 * slowed so that one is still going when it pulls the stick, and puts the
 * stick back — as it left, or changed on the host.
 *
 * ‼ EVERY WRITER ANSWERED, AND THEN EVERY BYTE OF NAME ONE WRITER'S.
 * Each writer says what it asks before it asks, and what it was answered once
 * it was: the bytes of its write the file holds, and the code — and marks its
 * slot on a Bay away sees. away waits for every writer to end, however long
 * the medium is away, and counts the slots marked. It does not read NAME
 * itself: a book given up is let go of
 * and the volume read again, and nothing tells a program when that is done —
 * so the host, which reads the kernel's word for it, asks awaycheck once the
 * volume is up. awaycheck reads NAME back through the volume mounted now: its
 * first block must be its own stream and every byte after it part of a chunk
 * — one writer's stream from that writer's first byte, the whole write or a
 * prefix of it, each writer once. It says the chunks in file order; the host
 * holds them to the writers' answers and to what the kernel did with the
 * return: a book taken up keeps every byte a writer was told the file holds,
 * one given up nothing no cycle settled, and never a waiter's.
 *
 * Every stream is QuillFill's, seeded SEED + k (only a seed's low byte tells
 * two apart): the base k = 0, writer k = 1..COUNT — so a host that knows SEED
 * knows every writer's bytes, and can look for them on the medium itself.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/bay.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/system.h"
#include "box/error.h"
#include "box/core/manifest.h"
#include "box/core/cabin.h"
#include "box/timeouts.h"
#include "boxos_decks.h"
#include "quilltest.h"

#define QUILL_AWAY_BOARD    "quilltest:away"
#define QUILL_AWAY_MAX      8u          /* writers one case starts */
#define QUILL_AWAY_MIB      4u          /* each writer's write */
#define QUILL_AWAY_BYTES    ((uint64_t)QUILL_AWAY_MIB << 20)
#define QUILL_AWAY_BASE     QUILL_CHUNK /* NAME's own first block */
#define QUILL_AWAY_WINDOW   (1u << 20)  /* read back a window at a time */
#define QUILL_WRITE_APPEND  1u          /* storage_ops.c OBJ_WRITE_APPEND_FLAG: at the file's size */

/* Which writer was answered, each in its own slot: a Bay away and its writers
 * all see. Marked by the writer once its write returned, so away counts
 * answers and not exits — an exit code asked of a writer already gone does
 * not come back. */
typedef struct {
    volatile uint32_t answered[QUILL_AWAY_MAX];
} QuillAwayBoard;

/* COUNT and SEED as away and awaycheck take them: false, said, for anything
 * a seed's low byte cannot hold COUNT more of. */
static bool QuillAwayWords(const char *mode, const char *const *args, uint32_t *count,
                           uint32_t *first)
{
    if (QuillNumber(args[1], count) && *count > 0 && *count <= QUILL_AWAY_MAX &&
        QuillNumber(args[2], first) && (*first & 0xFFu) + *count <= 0xFFu)
        return true;
    printf("[QUILL] %s: FAIL — COUNT is 1 to %u and SEED a number whose low byte leaves room "
           "for COUNT more; given '%s %s'\n", mode, QUILL_AWAY_MAX, args[1], args[2]);
    return false;
}

/* The word at offset `off` of the stream seeded `seed`. */
static uint64_t QuillAwayWord(uint32_t seed, uint64_t off)
{
    uint64_t w;
    QuillFill(&w, seed, off, sizeof(w));
    return w;
}

/* =========================================================================
 *  awaywriter — one write, appended, and its answer said
 * ========================================================================= */

/* `bytes` from `buf` written at the file's size, as the storage deck's append:
 * the answer's code, and in *written the bytes of the write the file holds —
 * the one contract every write answers (write_job.h, W-ERR). */
static int QuillAwayAppend(uint32_t fid, const void *buf, uint32_t bytes, uint64_t *written)
{
    uint8_t  params[16];
    uint64_t at    = 0;
    uint32_t flags = QUILL_WRITE_APPEND;
    memcpy(params, &fid, 4);
    memcpy(params + 4, &at, 8);
    memcpy(params + 12, &flags, 4);
    uint8_t  out[16] = { 0 };
    uint32_t got     = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_WRITE, params, sizeof(params), buf, bytes,
                     out, sizeof(out), &got, BOX_ANSWER_GUARANTEED, NULL);
    *written = 0;
    if (got >= 8)
        memcpy(written, out, 8);
    return rc;
}

/* The writer's buffer: a Bay of its own, as bigwrite's — not the heap, whose
 * first piece of 2 MiB or more a cabin can be refused (the kernel's first
 * heap pages may lie across the boundary it is placed at). */
static void *QuillAwayBuffer(uint32_t slot)
{
    char tag[32] = QUILL_AWAY_BOARD ":";
    char num[16];
    strcat(tag, uint_to_str(slot, num, sizeof(num)));
    return bay_open(tag, QUILL_AWAY_BYTES, BAY_CREATE);
}

QuillVerdict QuillAwayWriter(const char *const *args)
{
    const char *name = args[0];
    uint32_t    seed = 0, slot = QUILL_AWAY_MAX;
    uint32_t    ids[2];
    int         n     = QuillFindEverywhere(name, ids, 2);
    bool        asked = QuillNumber(args[1], &seed) && QuillNumber(args[2], &slot) &&
                        slot < QUILL_AWAY_MAX;
    void       *buf   = asked ? QuillAwayBuffer(slot) : NULL;
    QuillAwayBoard *board = buf && n == 1 ? bay_open(QUILL_AWAY_BOARD, 0, BAY_OPEN) : NULL;
    if (!board) {
        printf("[QUILL] awaywriter: FAIL — seed '%s', slot '%s', %d file(s) called %s, a buffer "
               "of %u MiB %s, and away's board %s\n", args[1], args[2], n, name, QUILL_AWAY_MIB,
               buf ? "had" : "refused", QUILL_AWAY_BOARD);
        if (buf)
            bay_release(buf);
        return QUILL_FAIL;
    }
    QuillFill(buf, seed, 0, QUILL_AWAY_BYTES);
    printf("[QUILL] awaywriter: %s fid=%u seed %u pid %u appends %llu bytes\n", name, ids[0],
           seed, cabin_info()->pid, (unsigned long long)QUILL_AWAY_BYTES);

    uint64_t written = 0;
    int      code    = QuillAwayAppend(ids[0], buf, (uint32_t)QUILL_AWAY_BYTES, &written);
    bay_release(buf);
    __atomic_store_n(&board->answered[slot], 1u, __ATOMIC_RELEASE);
    bay_release(board);
    printf("[QUILL] awaywriter: %s seed %u answered %llu of %llu byte(s) the file's (error %d)\n",
           name, seed, (unsigned long long)written, (unsigned long long)QUILL_AWAY_BYTES, code);
    return QUILL_PASS;
}

/* =========================================================================
 *  away — NAME laid down, the writers started and waited for
 * ========================================================================= */

/* COUNT cabins of awaywriter, seeds SEED + 1..COUNT in slots 0..COUNT - 1;
 * how many started, each pid and generation kept. */
static uint32_t QuillAwayStart(const char *name, uint32_t first, uint32_t count,
                               uint32_t *pid, uint32_t *gen)
{
    for (uint32_t k = 1; k <= count; k++) {
        char line[96] = "quilltest awaywriter ";
        char num[16];
        strcat(line, name);
        strcat(line, " ");
        strcat(line, uint_to_str(first + k, num, sizeof(num)));
        strcat(line, " ");
        strcat(line, uint_to_str(k - 1, num, sizeof(num)));
        int p = proc_exec_gen(line, NULL, &gen[k - 1]);
        if (p <= 0)
            return k - 1;
        pid[k - 1] = (uint32_t)p;
    }
    return count;
}

QuillVerdict QuillAway(const char *const *args)
{
    const char *name  = args[0];
    uint32_t    count = 0, first = 0;
    if (!QuillAwayWords("away", args, &count, &first))
        return QUILL_FAIL;

    static uint64_t base[QUILL_AWAY_BASE / 8];
    QuillFill(base, first, 0, sizeof(base));
    QuillAwayBoard *board = bay_open(QUILL_AWAY_BOARD, sizeof(QuillAwayBoard), BAY_CREATE);
    int     fid = board ? QuillCreate(name) : 0;
    int64_t w   = fid > 0 ? fwrite((uint32_t)fid, 0, base, sizeof(base)) : 0;
    int     arc = w == (int64_t)sizeof(base) ? anchor(0) : -1;
    if (!board || fid <= 0 || w != (int64_t)sizeof(base) || arc != 0) {
        printf("[QUILL] away: FAIL — %s could not be laid down (board %s, create %d, write %lld, "
               "anchor %d)\n", name, board ? "open" : "refused", fid, (long long)w, arc);
        if (board)
            bay_release(board);
        return QUILL_FAIL;
    }
    memset((void *)board, 0, sizeof(*board));
    printf("[QUILL] away: %s fid=%d laid down and anchored; %u writer(s) of %llu bytes each start, "
           "seeds %u to %u\n", name, fid, count, (unsigned long long)QUILL_AWAY_BYTES, first + 1,
           first + count);

    uint32_t pid[QUILL_AWAY_MAX], gen[QUILL_AWAY_MAX];
    uint32_t started  = QuillAwayStart(name, first, count, pid, gen);
    uint32_t answered = 0;
    for (uint32_t i = 0; i < started; i++) {
        (void)process_gone(pid[i], gen[i], NULL);
        answered += __atomic_load_n(&board->answered[i], __ATOMIC_ACQUIRE);
    }
    bay_release(board);
    bool whole = started == count && answered == count;
    printf("[QUILL] away: %s — %s fid=%d: %u of %u writer(s) started and %u answered\n",
           whole ? "PASS" : "FAIL", name, fid, started, count, answered);
    return whole ? QUILL_PASS : QUILL_FAIL;
}

/* =========================================================================
 *  awaycheck — NAME read back and cut into its writers' chunks
 * ========================================================================= */

typedef struct {
    uint32_t seed;           /* whose bytes: the base's or a writer's */
    uint64_t at, bytes;      /* where in the file, and how many */
} QuillAwayChunk;

typedef struct {
    QuillAwayChunk chunks[QUILL_AWAY_MAX + 1];
    uint32_t       count;
    uint64_t       size;
    uint64_t       stray;    /* the first byte that is no chunk's; == size when none */
    int64_t        refused;  /* the read that did not answer in full, 0 when none did */
} QuillAwayWalk;

/* The writer whose stream begins with `word`, among SEED + 1..COUNT that have
 * no chunk yet; 0 when none does. */
static uint32_t QuillAwayWhose(const QuillAwayWalk *walk, uint32_t first, uint32_t count,
                               uint64_t word)
{
    for (uint32_t k = 1; k <= count; k++) {
        bool had = false;
        for (uint32_t c = 0; c < walk->count; c++)
            had |= walk->chunks[c].seed == first + k;
        if (!had && QuillAwayWord(first + k, 0) == word)
            return first + k;
    }
    return 0;
}

/* One word of NAME at `off`: the chunk in hand goes on, or the next begins;
 * false when the word is nobody's. */
static bool QuillAwayStep(QuillAwayWalk *walk, uint32_t first, uint32_t count, uint64_t off,
                          uint64_t word)
{
    QuillAwayChunk *cur = walk->count ? &walk->chunks[walk->count - 1] : NULL;
    if (cur && off - cur->at < (cur->seed == first ? QUILL_AWAY_BASE : QUILL_AWAY_BYTES) &&
        word == QuillAwayWord(cur->seed, off - cur->at)) {
        cur->bytes += 8;
        return true;
    }
    uint32_t seed = off == 0 ? (word == QuillAwayWord(first, 0) ? first : 0)
                             : QuillAwayWhose(walk, first, count, word);
    if (seed == 0 || walk->count == QUILL_AWAY_MAX + 1)
        return false;
    cur = &walk->chunks[walk->count++];
    cur->seed  = seed;
    cur->at    = off;
    cur->bytes = 8;
    return true;
}

static void QuillAwayRead(uint32_t fid, uint32_t first, uint32_t count, QuillAwayWalk *walk)
{
    file_info_t info;
    memset(walk, 0, sizeof(*walk));
    int frc = file_info(fid, &info);
    if (frc != 0) {
        walk->refused = frc;
        return;
    }
    walk->size  = info.size;
    walk->stray = info.size;
    uint64_t *win = malloc(QUILL_AWAY_WINDOW);
    if (!win) {
        walk->refused = -(int64_t)ERR_NO_MEMORY;
        return;
    }
    for (uint64_t at = 0; at < walk->size && walk->stray == walk->size; at += QUILL_AWAY_WINDOW) {
        uint64_t want = walk->size - at < QUILL_AWAY_WINDOW ? walk->size - at : QUILL_AWAY_WINDOW;
        int64_t  got  = fread(fid, at, win, want);
        if (got != (int64_t)want) {
            walk->refused = got;
            walk->stray   = at;
            break;
        }
        for (uint64_t w = 0; w < want / 8; w++) {
            if (!QuillAwayStep(walk, first, count, at + w * 8, win[w])) {
                walk->stray = at + w * 8;
                break;
            }
        }
    }
    free(win);
}

QuillVerdict QuillAwayCheck(const char *const *args)
{
    const char *name  = args[0];
    uint32_t    count = 0, first = 0;
    uint32_t    ids[2];
    if (!QuillAwayWords("awaycheck", args, &count, &first))
        return QUILL_FAIL;
    int n = QuillFindEverywhere(name, ids, 2);
    if (n != 1) {
        printf("[QUILL] awaycheck: FAIL — the volume has %d file(s) called %s where one was laid "
               "down\n", n, name);
        return QUILL_FAIL;
    }

    QuillAwayWalk walk;
    QuillAwayRead(ids[0], first, count, &walk);
    char chunks[320] = "";
    char num[24];
    for (uint32_t c = 0; c < walk.count; c++) {
        strcat(chunks, c ? ", " : "");
        if (walk.chunks[c].seed == first) {
            strcat(chunks, "base ");
        } else {
            strcat(chunks, "seed ");
            strcat(chunks, uint_to_str(walk.chunks[c].seed, num, sizeof(num)));
            strcat(chunks, " ");
        }
        strcat(chunks, uint64_to_str(walk.chunks[c].bytes, num, sizeof(num)));
    }
    bool whole = walk.refused == 0 && walk.stray == walk.size && walk.count > 0 &&
                 walk.chunks[0].seed == first && walk.chunks[0].bytes == QUILL_AWAY_BASE;
    printf("[QUILL] awaycheck: %s — %s fid=%u holds %llu bytes: %s (the first byte no chunk's "
           "%llu, read %lld)\n", whole ? "PASS" : "FAIL", name, ids[0],
           (unsigned long long)walk.size, walk.count ? chunks : "no chunk",
           (unsigned long long)walk.stray, (long long)walk.refused);
    return whole ? QUILL_PASS : QUILL_FAIL;
}
