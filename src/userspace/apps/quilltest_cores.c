/*
 * quilltest_cores.c — the volume asked from many cabins at once (logcheck
 * cores): many writers, each with a file of its own, and appenders and a
 * spanner on one file. quilltest_ground.c holds the other two: a read while
 * the file is deleted, and defrag beside delete.
 *
 * ‼ A DRIVER AND ITS WORKERS, EACH IN A CABIN OF ITS OWN.
 * A cabin's calls are pockets, each served by one K-Core, the least loaded
 * taking the next: cabins asking at once are as many K-Cores inside TagFS at
 * once, and their writes go through the port and are pumped where its answers
 * continue, beside them — the way a person's programs would ask. The driver
 * starts its workers and waits for each one's end as a state (process_gone),
 * never on a clock; a worker says what it did in its own line and its exit.
 * Workers that share more than their exit share a Bay with the driver, the
 * board, and are told what they wait for on the board's tag in Touch.
 *
 * ‼ EVERY BYTE SAYS WHERE IT BELONGS (QuillFill), and every file here is
 * read back whole before anything is said of it; the host reads the same
 * files off the medium after the machine is stopped (logcheck cores), so a
 * file the kernel shows rightly and the medium holds wrongly is caught too.
 *
 * These cases change only files of their own names and stand in every build.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/bay.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/system.h"
#include "box/touch.h"
#include "box/error.h"
#include "box/timeouts.h"
#include "box/core/manifest.h"
#include "quilltest.h"

/* =========================================================================
 *  Cabins
 * ========================================================================= */

int QuillCabinStart(QuillCabin *c, const char *line)
{
    c->pid = 0;
    c->gen = 0;
    int pid = proc_exec_gen(line, NULL, &c->gen);
    if (pid > 0)
        c->pid = (uint32_t)pid;
    return pid;
}

int QuillCabinEnd(const QuillCabin *c, int32_t *code)
{
    *code = -1;
    return c->pid ? process_gone(c->pid, c->gen, code) : -1;
}

/* "quilltest WORD A B": a worker's line, its numbers in decimal. */
void QuillCabinLine(char out[QUILL_LINE_CAP], const char *word, uint32_t a, uint32_t b,
                    bool with_b)
{
    char n[12];
    strcpy(out, "quilltest ");
    strcat(out, word);
    strcat(out, " ");
    strcat(out, uint_to_str(a, n, sizeof(n)));
    if (with_b) {
        strcat(out, " ");
        strcat(out, uint_to_str(b, n, sizeof(n)));
    }
}

/* NAME with two digits of `index` before ".bin": "_qw" and 3 is "_qw03.bin". */
void QuillIndexedName(char out[QUILL_NAME_CAP], const char *stem, uint32_t index)
{
    size_t n = strlen(stem);
    memcpy(out, stem, n);
    out[n]     = (char)('0' + (index / 10) % 10);
    out[n + 1] = (char)('0' + index % 10);
    memcpy(out + n + 2, ".bin", 5);
}

/* =========================================================================
 *  writers — every core a writer with a file of its own
 * ========================================================================= */

#define QUILL_WRITERS_MAX    16u
#define QUILL_WRITERS_ROUNDS 16u          /* a round's stream is (index, round) in one byte */
#define QUILL_WRITERS_STEM   "_qw"

/* The size writer `i` leaves its file at in round `r`: one to five windows of
 * 128 KiB, a few blocks more, and a tail that ends inside a block — so a round
 * grows the file, or cuts it, by windows and by bytes. */
static uint64_t QuillWriterSize(uint32_t i, uint32_t r)
{
    return (uint64_t)(1u + (i + 3u * r) % 5u) * (128u << 10) + 4096u * ((i * 7u + r) % 9u) +
           8u * ((i * 131u + r * 17u) % 500u) + (i + r) % 8u;
}

/* QuillWord keys its stream by the seed's low byte: index and round in it. */
static uint32_t QuillWriterSeed(uint32_t i, uint32_t r)
{
    return (i << 4) | r;
}

static uint64_t QuillWriterMost(uint32_t rounds, uint64_t *least)
{
    uint64_t most = 0;
    *least = UINT64_MAX;
    for (uint32_t i = 0; i < QUILL_WRITERS_MAX; i++)
        for (uint32_t r = 0; r < rounds; r++) {
            uint64_t s = QuillWriterSize(i, r);
            most   = s > most ? s : most;
            *least = s < *least ? s : *least;
        }
    return most;
}

static bool QuillWritersArgs(const char *who, const char *const *args, uint32_t *count,
                             uint32_t *rounds)
{
    if (QuillNumber(args[0], count) && *count >= 1 && *count <= QUILL_WRITERS_MAX &&
        QuillNumber(args[1], rounds) && *rounds >= 1 && *rounds <= QUILL_WRITERS_ROUNDS)
        return true;
    printf("[QUILL] %s: FAIL — COUNT is 1 to %u and ROUNDS 1 to %u; given '%s %s'\n", who,
           QUILL_WRITERS_MAX, QUILL_WRITERS_ROUNDS, args[0], args[1]);
    return false;
}

/* One writer: its file made afresh, then each round the file cut to the
 * round's size when it is longer — a span, the K-Core's — and the round's
 * stream written over it from byte 0, the port's; read back whole after each. */
QuillVerdict QuillWriter(const char *const *args)
{
    uint32_t i = 0, rounds = 0;
    if (!QuillNumber(args[0], &i) || i >= QUILL_WRITERS_MAX || !QuillNumber(args[1], &rounds) ||
        rounds == 0 || rounds > QUILL_WRITERS_ROUNDS) {
        printf("[QUILL] writer: FAIL — INDEX is 0 to %u and ROUNDS 1 to %u; given '%s %s'\n",
               QUILL_WRITERS_MAX - 1, QUILL_WRITERS_ROUNDS, args[0], args[1]);
        return QUILL_FAIL;
    }
    char name[QUILL_NAME_CAP];
    QuillIndexedName(name, QUILL_WRITERS_STEM, i);
    uint64_t  least = 0;
    uint64_t  most  = QuillWriterMost(rounds, &least);
    uint64_t *buf   = malloc((most + 7) & ~7ull);
    int64_t   fid   = buf ? QuillCreate(name) : 0;
    if (fid <= 0) {
        printf("[QUILL] writer: FAIL — %s: a buffer of %llu bytes %s, and the file answered %ld\n",
               name, (unsigned long long)most, buf ? "was had" : "was not had", fid);
        return QUILL_FAIL;
    }

    for (uint32_t r = 0; r < rounds; r++) {
        uint64_t size = QuillWriterSize(i, r);
        uint32_t seed = QuillWriterSeed(i, r);
        QuillFill(buf, seed, 0, (size + 7) & ~7ull);
        uint64_t was  = QuillSize((file_id_t)fid);
        int      cut  = size < was ? file_span((file_id_t)fid, size, 0) : 0;
        int64_t  put  = cut == 0 ? fwrite((file_id_t)fid, 0, buf, size) : 0;
        uint64_t now  = QuillSize((file_id_t)fid);
        int64_t  rerr = 0;
        uint64_t good = put == (int64_t)size && now == size
                            ? QuillReadBack((file_id_t)fid, seed, size, &rerr) : 0;
        if (good != size) {
            printf("[QUILL] writer: FAIL — %s (fid %ld) round %u: %llu bytes over %llu, the cut "
                   "answered %d and the write %lld; the file holds %llu bytes, read back good to "
                   "byte %llu (read answer %lld)\n", name, fid, r, (unsigned long long)size,
                   (unsigned long long)was, cut, (long long)put, (unsigned long long)now,
                   (unsigned long long)good, (long long)rerr);
            return QUILL_FAIL;
        }
    }
    uint32_t last = rounds - 1;
    printf("[QUILL] writer: PASS — %s (fid %ld) holds %llu bytes of stream %u after %u rounds, "
           "each read back whole\n", name, fid, (unsigned long long)QuillWriterSize(i, last),
           QuillWriterSeed(i, last), rounds);
    return QUILL_PASS;
}

QuillVerdict QuillWriters(const char *const *args)
{
    uint32_t count = 0, rounds = 0;
    if (!QuillWritersArgs("writers", args, &count, &rounds))
        return QUILL_FAIL;

    QuillCabin cabins[QUILL_WRITERS_MAX];
    uint32_t   started = 0;
    int        refused = 0;
    for (; started < count; started++) {
        char line[QUILL_LINE_CAP];
        QuillCabinLine(line, "writer", started, rounds, true);
        if ((refused = QuillCabinStart(&cabins[started], line)) <= 0)
            break;
    }
    uint32_t failed = 0, first = QUILL_WRITERS_MAX;
    int32_t  code   = 0;
    for (uint32_t i = 0; i < started; i++) {
        int32_t c   = 0;
        int     grc = QuillCabinEnd(&cabins[i], &c);
        if (grc != 0 || c != 0) {
            failed++;
            if (first == QUILL_WRITERS_MAX) {
                first = i;
                code  = grc != 0 ? grc : c;
            }
        }
    }
    int      arc   = anchor(0);
    uint64_t least = 0;
    uint64_t most  = QuillWriterMost(rounds, &least);
    if (started == count && failed == 0 && arc == 0) {
        printf("[QUILL] writers: PASS — %u cabins each wrote and rewrote a file of its own %u "
               "times, %llu to %llu bytes a round, every round read back whole; anchored\n",
               count, rounds, (unsigned long long)least, (unsigned long long)most);
        return QUILL_PASS;
    }
    printf("[QUILL] writers: FAIL — %u of %u writer cabins started (the next answered %d), %u "
           "failed, the first writer %u with %d; anchor %d\n", started, count,
           started < count ? refused : 0, failed, first, code, arc);
    return QUILL_FAIL;
}

/* After a reboot: every writer's file, exactly one of its name, as its last
 * round left it. */
QuillVerdict QuillWritersBack(const char *const *args)
{
    uint32_t count = 0, rounds = 0;
    if (!QuillWritersArgs("writersback", args, &count, &rounds))
        return QUILL_FAIL;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        char     name[QUILL_NAME_CAP];
        file_id_t ids[4];
        QuillIndexedName(name, QUILL_WRITERS_STEM, i);
        int      n    = QuillFindEverywhere(name, ids, 4);
        uint64_t size = QuillWriterSize(i, rounds - 1);
        uint64_t has  = n == 1 ? QuillSize(ids[0]) : 0;
        int64_t  rerr = 0;
        uint64_t good = n == 1 && has == size
                            ? QuillReadBack(ids[0], QuillWriterSeed(i, rounds - 1), size, &rerr) : 0;
        if (good != size) {
            printf("[QUILL] writersback: FAIL — %s: %d file(s) of that name, holding %llu bytes "
                   "where its last round left %llu of stream %u, read back good to byte %llu "
                   "(read answer %lld); %u file(s) before it were whole\n", name, n,
                   (unsigned long long)has, (unsigned long long)size, QuillWriterSeed(i, rounds - 1),
                   (unsigned long long)good, (long long)rerr, i);
            return QUILL_FAIL;
        }
        total += size;
    }
    printf("[QUILL] writersback: PASS — after the reboot the %u writers' files are there, one of "
           "each name, %llu bytes in all, each read back whole as its round %u left it\n", count,
           (unsigned long long)total, rounds - 1);
    return QUILL_PASS;
}

/* =========================================================================
 *  append — appenders and a spanner on one file
 * ========================================================================= */

#define QUILL_APPEND_NAME     "_qap.bin"
#define QUILL_APPEND_BOARD    "quilltest:append"
#define QUILL_APPEND_WRITERS  4u
#define QUILL_APPEND_RECORDS  256u
#define QUILL_APPEND_MAGIC    0x3143455250504151ull   /* "QAPPREC1" */
#define QUILL_APPEND_HEAD     24u
#define QUILL_APPEND_MOST     (QUILL_APPEND_HEAD + 8u * 38400u)
#define QUILL_APPEND_GROW     (3u * 4096u + 1000u)    /* a span's growth: blocks and a few bytes */

/* A record: its head, then its stream — keyed by the writer, and by its own
 * number in the offset, so no two records anywhere hold the same bytes. */
typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t writer;
    uint32_t seq;
    uint32_t len;
    uint32_t check;
} QuillAppendHead;

_Static_assert(sizeof(QuillAppendHead) == QUILL_APPEND_HEAD, "an append record's head");

typedef struct {
    volatile uint64_t at;          /* where the answer said the record begins */
    volatile uint32_t len;
    volatile uint32_t landed;      /* 1 once its append was answered whole */
} QuillAppendLanding;

typedef struct {
    volatile uint32_t  stop;
    volatile file_id_t fid;
    volatile uint32_t  made[QUILL_APPEND_WRITERS];
    volatile int32_t   refusal[QUILL_APPEND_WRITERS];
    volatile uint32_t  spans;
    volatile uint32_t  span_refusal_at;
    volatile int32_t   span_refusal;
    volatile uint64_t  asked;      /* the growth the spans asked for, in bytes */
    QuillAppendLanding landing[QUILL_APPEND_WRITERS][QUILL_APPEND_RECORDS];
} QuillAppendBoard;

/* Two of three records end inside a block — a few to some thousands of bytes;
 * the third runs to some windows. */
static uint32_t QuillAppendLen(uint32_t w, uint32_t k)
{
    uint32_t h = (k * 2654435761u) ^ (w * 40503u);
    if (k % 3u != 2u)
        return QUILL_APPEND_HEAD + 8u * (1u + h % 600u);
    return QUILL_APPEND_HEAD + 8u * (1u + h % 38400u);
}

static uint32_t QuillAppendCheck(uint32_t w, uint32_t k)
{
    uint64_t at = ((uint64_t)w << 32) | k;
    uint64_t x  = at * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x ^ (x >> 29));
}

static void QuillAppendMake(uint64_t *buf, uint32_t w, uint32_t k, uint32_t len)
{
    QuillAppendHead head = { QUILL_APPEND_MAGIC, w, k, len, QuillAppendCheck(w, k) };
    QuillFill(buf + QUILL_APPEND_HEAD / 8, 0xA0u + w, (uint64_t)k << 32, len - QUILL_APPEND_HEAD);
    memcpy(buf, &head, sizeof(head));
}

/* One write at the file's size (STORAGE_WRITE_APPEND): the bytes the file
 * holds of it, or a refusal; *size the size it left. */
static int64_t QuillAppendOnce(file_id_t fid, const void *buf, uint32_t len, uint64_t *size)
{
    uint8_t  params[20];
    uint64_t offset = 0;
    uint32_t flags  = STORAGE_WRITE_APPEND;
    memcpy(params, &fid, 8);
    memcpy(params + 8, &offset, 8);
    memcpy(params + 16, &flags, 4);
    uint8_t  out[16] = { 0 };
    uint32_t got     = 0;
    int rc = MfCall1(DECK_STORAGE, STORAGE_OBJ_WRITE, params, sizeof(params), buf, len, out,
                     sizeof(out), &got, BOX_ANSWER_GUARANTEED, NULL);
    uint64_t inked = 0;
    *size = 0;
    if (got >= 16) {
        memcpy(&inked, out, 8);
        memcpy(size, out + 8, 8);
    }
    if (rc != 0)
        return inked > 0 ? (int64_t)inked : box_fail(rc);
    return (int64_t)inked;
}

static QuillAppendBoard *QuillAppendBoardOpen(const char *who)
{
    QuillAppendBoard *board = bay_open(QUILL_APPEND_BOARD, 0, BAY_OPEN);
    if (!board)
        printf("[QUILL] %s: FAIL — the board %s is not open: it is append's, and stands only "
               "while append runs (0 calls made)\n", who, QUILL_APPEND_BOARD);
    return board;
}

QuillVerdict QuillAppender(const char *const *args)
{
    uint32_t w = 0, records = 0;
    if (!QuillNumber(args[0], &w) || w >= QUILL_APPEND_WRITERS || !QuillNumber(args[1], &records) ||
        records == 0 || records > QUILL_APPEND_RECORDS) {
        printf("[QUILL] appender: FAIL — INDEX is 0 to %u and RECORDS 1 to %u; given '%s %s'\n",
               QUILL_APPEND_WRITERS - 1, QUILL_APPEND_RECORDS, args[0], args[1]);
        return QUILL_FAIL;
    }
    QuillAppendBoard *board = QuillAppendBoardOpen("appender");
    uint64_t         *buf   = board ? malloc(QUILL_APPEND_MOST) : NULL;
    if (!buf) {
        if (board)
            bay_release(board);
        return QUILL_FAIL;
    }
    TouchTagPair pair = touch_intern(QUILL_APPEND_BOARD);
    file_id_t    fid  = board->fid;
    int64_t      got  = 0;
    uint64_t     size = 0;
    uint32_t     k    = 0;
    for (; k < records; k++) {
        uint32_t len = QuillAppendLen(w, k);
        QuillAppendMake(buf, w, k, len);
        got = QuillAppendOnce(fid, buf, len, &size);
        if (got != (int64_t)len)
            break;
        board->landing[w][k].at  = size - len;
        board->landing[w][k].len = len;
        __atomic_store_n(&board->landing[w][k].landed, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&board->made[w], k + 1, __ATOMIC_RELEASE);
        touch_send(pair, &w, sizeof(w), 0);
    }
    if (k < records)
        board->refusal[w] = got < 0 ? (int32_t)got : -1;
    bay_release(board);
    if (k < records) {
        printf("[QUILL] appender: FAIL — appender %u: append %u of %u bytes answered %lld, the "
               "size it left %llu, after %u answered whole\n", w, k, QuillAppendLen(w, k),
               (long long)got, (unsigned long long)size, k);
        return QUILL_FAIL;
    }
    printf("[QUILL] appender: PASS — appender %u made %u appends to fid %lu, each answered whole "
           "with where it landed\n", w, records, fid);
    return QUILL_PASS;
}

/* The spanner: once an append is heard, the file grown by QUILL_APPEND_GROW
 * zeros past the size it read — STORAGE_SPAN_GROW, so an append that came
 * between the read and the span is never cut back. */
QuillVerdict QuillSpanner(const char *const *args)
{
    (void)args;
    QuillAppendBoard *board = QuillAppendBoardOpen("spanner");
    if (!board)
        return QUILL_FAIL;
    TouchTagPair pair  = touch_intern(QUILL_APPEND_BOARD);
    TouchTag     heard = touch_pair_choose(pair);
    int          crc   = heard != TOUCH_TAG_INVALID ? touch_claim(heard, TOUCH_REST, 0, 0) : -1;
    file_id_t    fid   = board->fid;
    int          rc    = 0;
    while (crc == 0 && !__atomic_load_n(&board->stop, __ATOMIC_ACQUIRE)) {
        Touch said;
        touch_await(heard, &said, 0);
        if (__atomic_load_n(&board->stop, __ATOMIC_ACQUIRE))
            break;
        uint64_t size = QuillSize(fid);
        if ((rc = file_span(fid, size + QUILL_APPEND_GROW, STORAGE_SPAN_GROW)) != 0) {
            board->span_refusal_at = board->spans + 1;
            board->span_refusal    = rc;
            break;
        }
        board->asked += QUILL_APPEND_GROW;
        __atomic_store_n(&board->spans, board->spans + 1, __ATOMIC_RELEASE);
    }
    uint32_t spans = board->spans;
    if (crc == 0)
        touch_release(heard);
    bay_release(board);
    if (crc != 0 || rc != 0) {
        printf("[QUILL] spanner: FAIL — the board's tag answered the claim %d; span %u answered "
               "%d\n", crc, spans + 1, rc);
        return QUILL_FAIL;
    }
    printf("[QUILL] spanner: PASS — %u spans of fid %lu, each answered\n", spans, fid);
    return QUILL_PASS;
}

/* The landings in the order they lie in the file. */
static uint32_t QuillAppendOrder(const QuillAppendBoard *b, uint32_t writers, uint32_t records,
                                 uint16_t *order)
{
    uint32_t n = 0;
    for (uint32_t w = 0; w < writers; w++)
        for (uint32_t k = 0; k < records; k++) {
            if (!b->landing[w][k].landed)
                continue;
            uint16_t me = (uint16_t)(w * QUILL_APPEND_RECORDS + k);
            uint32_t j  = n++;
            while (j > 0) {
                uint16_t o = order[j - 1];
                if (b->landing[o / QUILL_APPEND_RECORDS][o % QUILL_APPEND_RECORDS].at <=
                    b->landing[w][k].at)
                    break;
                order[j] = o;
                j--;
            }
            order[j] = me;
        }
    return n;
}

/* The file read whole and held against the board: each record where its
 * answer said, byte for byte; nothing but zeros between two; the zeros no
 * more than the spans asked for. A sentence saying the first thing that is
 * not so, or NULL, into `why`. */
static bool QuillAppendJudge(const QuillAppendBoard *b, uint32_t writers, uint32_t records,
                             const uint8_t *file, uint64_t size, uint64_t *zeros, char *why)
{
    static uint16_t order[QUILL_APPEND_WRITERS * QUILL_APPEND_RECORDS];
    static uint64_t want[QUILL_APPEND_MOST / 8];
    uint32_t n   = QuillAppendOrder(b, writers, records, order);
    uint64_t pos = 0;
    char     a[24], c[24];
    *zeros = 0;
    for (uint32_t j = 0; j <= n; j++) {
        uint64_t next = j < n ? b->landing[order[j] / QUILL_APPEND_RECORDS]
                                          [order[j] % QUILL_APPEND_RECORDS].at : size;
        if (next < pos || next > size) {
            strcpy(why, "two appends were answered with places that overlap, or past the size, at byte ");
            strcat(why, uint64_to_str(next, a, sizeof(a)));
            return false;
        }
        for (; pos < next; pos++, (*zeros)++)
            if (file[pos] != 0) {
                strcpy(why, "byte ");
                strcat(why, uint64_to_str(pos, a, sizeof(a)));
                strcat(why, " lies in no append and is not a span's zero");
                return false;
            }
        if (j == n)
            break;
        uint32_t w = order[j] / QUILL_APPEND_RECORDS, k = order[j] % QUILL_APPEND_RECORDS;
        uint32_t len = b->landing[w][k].len;
        QuillAppendMake(want, w, k, len);
        if (next + len > size || memcmp(file + next, want, len) != 0) {
            uint32_t at = 0;
            while (next + at < size && at < len && file[next + at] == ((uint8_t *)want)[at])
                at++;
            strcpy(why, "append ");
            strcat(why, uint_to_str(k, c, sizeof(c)));
            strcat(why, " of appender ");
            strcat(why, uint_to_str(w, a, sizeof(a)));
            strcat(why, " is not where its answer put it: its bytes differ from byte ");
            strcat(why, uint_to_str(at, c, sizeof(c)));
            strcat(why, " of it, file byte ");
            strcat(why, uint64_to_str(next + at, a, sizeof(a)));
            return false;
        }
        pos = next + len;
    }
    return true;
}

static bool QuillAppendArgs(const char *const *args, uint32_t *writers, uint32_t *records,
                            uint32_t *spanners)
{
    if (QuillNumber(args[0], writers) && *writers >= 1 && *writers <= QUILL_APPEND_WRITERS &&
        QuillNumber(args[1], records) && *records >= 1 && *records <= QUILL_APPEND_RECORDS &&
        QuillNumber(args[2], spanners) && *spanners <= 1)
        return true;
    printf("[QUILL] append: FAIL — WRITERS is 1 to %u, RECORDS 1 to %u and SPANNERS 0 or 1; "
           "given '%s %s %s'\n", QUILL_APPEND_WRITERS, QUILL_APPEND_RECORDS, args[0], args[1],
           args[2]);
    return false;
}

/* Every worker started, every appender's end waited for, then the spanner
 * told to stop. How many started. */
static uint32_t QuillAppendRun(QuillAppendBoard *board, TouchTagPair pair, uint32_t writers,
                               uint32_t records, uint32_t spanners, uint32_t *failed)
{
    QuillCabin cabins[QUILL_APPEND_WRITERS + 1];
    uint32_t   started = 0, want = writers + spanners;
    for (; started < want; started++) {
        char line[QUILL_LINE_CAP];
        if (started < writers)
            QuillCabinLine(line, "appender", started, records, true);
        else
            strcpy(line, "quilltest spanner");
        if (QuillCabinStart(&cabins[started], line) <= 0)
            break;
    }
    *failed = 0;
    for (uint32_t i = 0; i < started; i++) {
        if (i == writers) {      /* every appender has ended: the spanner is told to stop */
            __atomic_store_n(&board->stop, 1u, __ATOMIC_RELEASE);
            touch_send(pair, &i, sizeof(i), 0);
        }
        int32_t code = 0;
        if (QuillCabinEnd(&cabins[i], &code) != 0 || code != 0)
            (*failed)++;
    }
    return started;
}

QuillVerdict QuillAppend(const char *const *args)
{
    uint32_t writers = 0, records = 0, spanners = 0;
    if (!QuillAppendArgs(args, &writers, &records, &spanners))
        return QUILL_FAIL;
    QuillAppendBoard *board = bay_open(QUILL_APPEND_BOARD, sizeof(QuillAppendBoard), BAY_CREATE);
    int64_t           fid   = board ? QuillCreate(QUILL_APPEND_NAME) : 0;
    if (fid <= 0) {
        printf("[QUILL] append: FAIL — its board %s, and %s answered %ld\n",
               board ? "opened" : "would not open", QUILL_APPEND_NAME, fid);
        if (board)
            bay_release(board);
        return QUILL_FAIL;
    }
    memset((void *)board, 0, sizeof(*board));
    board->fid = (file_id_t)fid;
    TouchTagPair pair = touch_intern(QUILL_APPEND_BOARD);

    uint32_t failed  = 0;
    uint32_t started = QuillAppendRun(board, pair, writers, records, spanners, &failed);
    uint64_t size    = QuillSize((file_id_t)fid);
    uint8_t *file    = size ? bay_open("quilltest:append:read", size, BAY_CREATE) : NULL;
    int64_t  rerr    = 0;
    for (uint64_t at = 0; file && at < size && rerr >= 0; at += (1u << 20)) {
        uint64_t n = size - at < (1u << 20) ? size - at : (1u << 20);
        rerr = fread((file_id_t)fid, at, file + at, n);
        if (rerr >= 0 && (uint64_t)rerr != n)
            rerr = -1;
    }
    if (rerr > 0)
        rerr = 0;
    static char why[192];
    uint64_t zeros  = 0;
    bool     read   = file && rerr == 0;
    bool     judged = read && QuillAppendJudge(board, writers, records, file, size, &zeros, why);
    uint64_t sum    = 0;
    uint32_t made   = 0;
    for (uint32_t w = 0; w < writers; w++)
        for (uint32_t k = 0; k < board->made[w]; k++, made++)
            sum += board->landing[w][k].len;
    uint32_t spans = board->spans;
    uint64_t asked = board->asked;
    if (file)
        bay_release(file);
    bay_release(board);
    int arc = anchor(0);

    if (started == writers + spanners && failed == 0 && made == writers * records && judged &&
        zeros <= asked && sum + zeros == size && arc == 0) {
        printf("[QUILL] append: PASS — %u appender(s) made %u appends to %s, each read back where "
               "its answer put it and whole; %u span(s) grew it by %llu zeros of the %llu they "
               "asked for, and nothing else is in it: %llu bytes = %llu appended + %llu zeros; "
               "anchored\n", writers, made, QUILL_APPEND_NAME, spans, (unsigned long long)zeros,
               (unsigned long long)asked, (unsigned long long)size, (unsigned long long)sum,
               (unsigned long long)zeros);
        return QUILL_PASS;
    }
    if (judged && zeros > asked) {
        strcpy(why, "the file holds more zeros between its appends than every span asked for");
        judged = false;
    }
    if (read && !judged) {
        printf("[QUILL] append: DEFECT — %s; %u of %u appends were answered whole, %llu bytes of "
               "them, %u span(s) asked %llu bytes, and the file holds %llu bytes\n", why, made,
               writers * records, (unsigned long long)sum, spans, (unsigned long long)asked,
               (unsigned long long)size);
        return QUILL_DEFECT;
    }
    printf("[QUILL] append: FAIL — %u of %u workers started, %u failed; %u of %u appends "
           "answered whole, %llu bytes of them; %u span(s) asked %llu bytes; the file holds %llu "
           "bytes, %llu of them zeros between appends (read answer %lld); anchor %d\n", started,
           writers + spanners, failed, made, writers * records, (unsigned long long)sum, spans,
           (unsigned long long)asked, (unsigned long long)size, (unsigned long long)zeros,
           (long long)rerr, arc);
    return QUILL_FAIL;
}
