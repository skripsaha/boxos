/*
 * quilltest_powercut.c — the modes of quilltest the power-cut stand drives.
 *
 * tools/powercut.sh records every write the machine makes, pulls the power
 * at every flush and at every subset of the writes between two, and boots
 * what is left. These modes give it something to cut: a write left
 * unanchored while one core is busy (hog, settle), a program asked to run
 * beside that hog (hogged), a writer that never stops (churn), anchors
 * racing that writer (race), files that each add a record to the tag
 * registry and a row to the file table (crowd), and the read that says
 * afterwards whether a record survived (verify) — and, for the halt's stand
 * (logcheck haltcycle), a writer that anchors every record it writes
 * (keeper). Each says what it did in
 * quilltest's four words, with the numbers the judge needs to compute the
 * bytes a surviving file must hold. See quilltest.c for the rest.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/system.h"
#include "box/strand.h"
#include "box/time.h"
#include "box/touch.h"
#include "box/error.h"
#include "box/core/cabin.h"
#include "quilltest.h"

#define QUILL_CHURN_PROGRESS 100u
#define QUILL_CROWD_PROGRESS 50u
#define QUILL_CROWD_NAME     16u         /* "crowd", ten digits and the NUL */

/* =========================================================================
 *  The record of the power-cut modes
 * ========================================================================= */

/* persist's PersistRecord in shape and checksum (apps/persist.c), under its
 * own magic, so the PowerCut judge computes the 16 bytes a surviving file
 * must hold with the one formula it already has. */
#define QUILL_RECORD_MAGIC   0x5155494Cu /* 'QUIL' */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t checksum;
    uint32_t reserved;
} QuillRecord;

static uint32_t QuillChecksum(uint32_t magic, uint32_t seq)
{
    uint32_t x = magic ^ (seq * 2654435761u);
    x ^= x >> 16;
    return x * 0x85ebca6bu;
}

static QuillRecord QuillRecordOf(uint32_t seq)
{
    QuillRecord rec = {
        .magic    = QUILL_RECORD_MAGIC,
        .seq      = seq,
        .checksum = QuillChecksum(QUILL_RECORD_MAGIC, seq),
        .reserved = 0,
    };
    return rec;
}

static bool QuillRecordIsWhole(const QuillRecord *rec)
{
    return rec->magic == QUILL_RECORD_MAGIC && rec->reserved == 0 &&
           rec->checksum == QuillChecksum(rec->magic, rec->seq);
}

/* The seq of the record `fid` holds now, or 0 when it holds no whole one. */
static uint32_t QuillReadSeq(uint32_t fid)
{
    QuillRecord rec;
    memset(&rec, 0, sizeof(rec));
    if (fread(fid, 0, &rec, sizeof(rec)) != (int64_t)sizeof(rec) || !QuillRecordIsWhole(&rec))
        return 0;
    return rec.seq;
}

/* A pure compute loop: no syscall, no yield, never ends. What a machine
 * with one core has to keep settling writes under. */
__attribute__((noreturn)) QuillVerdict QuillHog(const char *const *args)
{
    (void)args;
    printf("[QUILL] hog: pid %u computes without end and never yields\n", cabin_info()->pid);
    volatile uint64_t x = cabin_info()->pid;
    for (;;)
        x = x * 6364136223846793005ull + 1442695040888963407ull;
}

/* A hog in another cabin, one record written and NOT anchored, then parked
 * on the hog while it spins — the power goes while the machine has a write it
 * has not settled and a core that is busy. `verify NAME` after the cut says
 * whether the write survived. A park that is refused, or a hog that ends
 * before the power goes, leaves nothing to cut beside it: that is a FAIL,
 * said after the PENDING it overturns. */
QuillVerdict QuillSettle(const char *const *args)
{
    const char *name = args[0];
    uint32_t    gen  = 0;
    int hog = proc_exec_gen("quilltest hog", NULL, &gen);
    if (hog <= 0) {
        printf("[QUILL] settle: FAIL — the hog would not start (answer %d)\n", hog);
        return QUILL_FAIL;
    }

    uint64_t ms = 0;
    time_uptime_ms(&ms);
    uint32_t    seq = (uint32_t)ms ? (uint32_t)ms : 1u;
    QuillRecord rec = QuillRecordOf(seq);
    int     fid = QuillCreate(name);
    int64_t w   = fid > 0 ? fwrite((uint32_t)fid, 0, &rec, sizeof(rec)) : 0;
    if (fid <= 0 || w != (int64_t)sizeof(rec)) {
        int krc = proc_finish((uint32_t)hog, gen);
        printf("[QUILL] settle: FAIL — %s could not be written beside hog pid %d (create %d, "
               "write %lld; the hog ended: %d)\n", name, hog, fid, (long long)w, krc);
        return QUILL_FAIL;
    }
    printf("[QUILL] settle: PENDING — wrote %s fid=%u seq=%u bytes=%u beside hog pid %d, "
           "not anchored; parked on the hog until the power goes, then quilltest verify %s\n",
           name, (uint32_t)fid, seq, (uint32_t)sizeof(rec), hog, name);

    int32_t code = 0;
    int     grc  = process_gone((uint32_t)hog, gen, &code);
    if (grc != 0) {
        int krc = proc_finish((uint32_t)hog, gen);
        printf("[QUILL] settle: FAIL — the park on hog pid %d was refused (gone %d), so nothing "
               "waits for the power beside it; the hog ended: %d\n", hog, grc, krc);
        return QUILL_FAIL;
    }
    printf("[QUILL] settle: FAIL — the hog (pid %d) ended with exit %d before the power went, so "
           "the write was not left beside a busy core\n", hog, code);
    return QUILL_FAIL;
}

/* PROGRAM run to its end in a cabin of its own while a hog spins in another,
 * and how long that took — the design's "anchor under the hog answers":
 * `quilltest hogged anchor_test` asks whether what an anchor needs from the
 * kernel is given on a machine whose only core a program will not let go
 * of. The shell runs one program at a time, so the hog and PROGRAM are both
 * started from here. A PROGRAM that is never answered leaves this parked,
 * and the silence is the finding. */
QuillVerdict QuillHogged(const char *const *args)
{
    const char *line    = args[0];
    uint32_t    hog_gen = 0, gen = 0;
    int hog = proc_exec_gen("quilltest hog", NULL, &hog_gen);
    if (hog <= 0) {
        printf("[QUILL] hogged: FAIL — the hog would not start (answer %d)\n", hog);
        return QUILL_FAIL;
    }

    uint64_t from = 0, to = 0;
    time_uptime_ms(&from);
    int     pid  = proc_exec_gen(line, NULL, &gen);
    int32_t code = -1;
    int     grc  = pid > 0 ? process_gone((uint32_t)pid, gen, &code) : pid;
    time_uptime_ms(&to);
    int krc = proc_finish((uint32_t)hog, hog_gen);

    if (pid <= 0 || grc != 0) {
        printf("[QUILL] hogged: FAIL — '%s' could not be run beside hog pid %d (start %d, "
               "gone %d; the hog ended: %d)\n", line, hog, pid, grc, krc);
        return QUILL_FAIL;
    }
    printf("[QUILL] hogged: %s — '%s' (pid %d) ended with exit %d after %llu ms while hog "
           "pid %d spun (the hog ended: %d)\n", code == 0 ? "PASS" : "FAIL", line, pid,
           code, (unsigned long long)(to - from), hog, krc);
    return code == 0 ? QUILL_PASS : QUILL_FAIL;
}

/* What `settle` or `churn` left in NAME, read back after the machine came up
 * again. Reads only. */
QuillVerdict QuillVerify(const char *const *args)
{
    const char *name = args[0];
    uint32_t ids[4];
    int n = QuillFindEverywhere(name, ids, 4);
    if (n != 1) {
        printf("[QUILL] verify: FAIL — the volume has %d file(s) called %s where one was "
               "written\n", n, name);
        return QUILL_FAIL;
    }
    QuillRecord rec;
    memset(&rec, 0, sizeof(rec));
    uint64_t size = QuillSize(ids[0]);
    int64_t  r    = fread(ids[0], 0, &rec, sizeof(rec));
    if (r == (int64_t)sizeof(rec) && size == sizeof(rec) && QuillRecordIsWhole(&rec)) {
        printf("[QUILL] verify: PASS — %s fid=%u seq=%u bytes=%llu: magic 0x%x and checksum "
               "0x%x read back as written\n", name, ids[0], rec.seq,
               (unsigned long long)size, rec.magic, rec.checksum);
        return QUILL_PASS;
    }
    printf("[QUILL] verify: FAIL — %s fid=%u holds %llu bytes and a read of %u answered %lld: "
           "magic 0x%x seq=%u checksum 0x%x where 0x%x was due\n", name, ids[0],
           (unsigned long long)size, (uint32_t)sizeof(rec), (long long)r, rec.magic, rec.seq,
           rec.checksum, QuillChecksum(rec.magic, rec.seq));
    return QUILL_FAIL;
}

/* NAME carried on — the file of that name, and the seq after the record it
 * holds — or laid down, at seq 1: its file id, or <= 0 with `mode`'s FAIL
 * said. */
static int QuillCarryOn(const char *mode, const char *name, uint32_t *seq)
{
    uint32_t ids[1];
    int n = QuillFindEverywhere(name, ids, 1);
    if (n < 0) {
        printf("[QUILL] %s: FAIL — could not look for %s (answer %d), so there is no telling "
               "whether to carry a file on or lay one down\n", mode, name, n);
        return n;
    }
    int fid;
    *seq = 1;
    if (n > 0) {
        fid  = (int)ids[0];
        *seq = QuillReadSeq(ids[0]) + 1;
    } else {
        fid = create_everywhere(name, NULL);
    }
    if (fid <= 0)
        printf("[QUILL] %s: FAIL — %s could not be found or created (answers %d, %d)\n",
               mode, name, n, fid);
    return fid;
}

/* A writer that never stops: NAME's record rewritten with the next seq, over
 * and over, never anchored. Carries on from the record NAME already holds, so
 * `race` can lay the file down first and hand it over. */
QuillVerdict QuillChurn(const char *const *args)
{
    const char *name = args[0];
    uint32_t    seq  = 1;
    int         fid  = QuillCarryOn("churn", name, &seq);
    if (fid <= 0)
        return QUILL_FAIL;
    printf("[QUILL] churn: %s fid=%u starts at seq=%u — it writes until the machine stops\n",
           name, (uint32_t)fid, seq);
    for (;; seq++) {
        QuillRecord rec = QuillRecordOf(seq);
        int64_t     w   = fwrite((uint32_t)fid, 0, &rec, sizeof(rec));
        if (w != (int64_t)sizeof(rec)) {
            printf("[QUILL] churn: FAIL — writing seq=%u of %s fid=%u answered %lld\n",
                   seq, name, (uint32_t)fid, (long long)w);
            return QUILL_FAIL;
        }
        if (seq % QUILL_CHURN_PROGRESS == 0)
            printf("[QUILL] churn: %s fid=%u wrote seq=%u\n", name, (uint32_t)fid, seq);
    }
}

/* A writer that keeps what it writes: NAME's record rewritten with the next
 * seq and anchored, again and again, each anchor said once it answered. The
 * halt's stand (logcheck haltcycle) presses the power button while a cycle
 * settles one of them, and the record the last line named must be there
 * after the reboot. Carries on from the record NAME holds, as churn does;
 * never returns while the medium takes its writes and its anchors. */
QuillVerdict QuillKeeper(const char *const *args)
{
    const char *name = args[0];
    uint32_t    seq  = 1;
    int         fid  = QuillCarryOn("keeper", name, &seq);
    if (fid <= 0)
        return QUILL_FAIL;
    printf("[QUILL] keeper: %s fid=%u starts at seq=%u — it writes and anchors until the "
           "machine stops\n", name, (uint32_t)fid, seq);
    for (;; seq++) {
        QuillRecord rec = QuillRecordOf(seq);
        int64_t     w   = fwrite((uint32_t)fid, 0, &rec, sizeof(rec));
        int         arc = w == (int64_t)sizeof(rec) ? anchor((uint32_t)fid) : 0;
        if (w != (int64_t)sizeof(rec) || arc != 0) {
            printf("[QUILL] keeper: FAIL — seq=%u of %s fid=%u: the write answered %lld, the "
                   "anchor %d\n", seq, name, (uint32_t)fid, (long long)w, arc);
            return QUILL_FAIL;
        }
        printf("[QUILL] keeper: %s fid=%u anchored seq=%u\n", name, (uint32_t)fid, seq);
    }
}

/* How many WROTE events of the churn's are waiting on NAME's stem tag;
 * taken off the ring as they are counted, so it never fills while the
 * anchors run. */
static uint32_t QuillRaceHeard(TouchTag tag, uint32_t churn)
{
    uint32_t heard = 0;
    Touch    t;
    while (touch_try_pop_tag(tag, &t))
        if (t.source_pid == churn)
            heard++;
    return heard;
}

/* What race's watch on its churn saw. `gone` is set before it is said on
 * NAME's stem tag, so whoever wakes on that word finds it set. */
typedef struct {
    uint32_t     pid, gen;           /* the churn */
    TouchTagPair pair;               /* NAME's stem tag, which race listens on */
    int          grc;                /* process_gone's own answer */
    int32_t      code;               /* how the churn ended */
    uint32_t     gone;
} QuillChurnWatch;

/* The churn's end, asked of the kernel as a state — an incarnation already
 * gone answers at once, so there is nothing to miss — and said on the tag
 * race waits on for the churn's first write. A churn that ends before it
 * writes ends that wait, instead of leaving race parked for good with no
 * verdict. */
static void QuillRaceWatch(void *arg)
{
    QuillChurnWatch *w = arg;
    w->grc = process_gone(w->pid, w->gen, &w->code);
    __atomic_store_n(&w->gone, 1u, __ATOMIC_RELEASE);
    touch_send(w->pair, &w->code, sizeof(w->code), 0);
}

/* The watch let go of: taken down while it still waits (its pid is held for
 * it, joinable, so the kill can land on nobody else), and released. */
static void QuillRaceUnwatch(uint32_t watcher, const QuillChurnWatch *watch)
{
    if (!__atomic_load_n(&watch->gone, __ATOMIC_ACQUIRE))
        proc_kill(watcher);
    strand_release(watcher);
}

/* ROUNDS anchors of the whole volume while a churn writes NAME in another
 * cabin — an anchor racing a writer for the pool's dirty mark. The anchors
 * start on the churn's first write, heard as the WROTE event on NAME's stem
 * tag, never on a guess at how long a spawn takes, and the churn's writes
 * heard while they run say the race really ran. A churn that ends first is
 * heard too, from a strand parked on its end. The churn is left writing. */
QuillVerdict QuillRace(const char *const *args)
{
    const char *name   = args[0];
    uint32_t    rounds = is_number(args[1]) ? to_uint(args[1]) : 0;
    if (rounds == 0) {
        printf("[QUILL] race: FAIL — ROUNDS is a count above 0, and '%s' is not (0 anchors run)\n",
               args[1]);
        return QUILL_FAIL;
    }
    /* The churn is handed NAME as one quoted word of its command line, so a
     * NAME with a blank reaches it whole; one with a quote in it cannot. */
    char   line[64];
    size_t nl = strlen(name);
    if (strchr(name, '"') || 16 + 1 + nl + 1 >= sizeof(line)) {
        printf("[QUILL] race: FAIL — %s cannot be handed to the churn as one quoted word "
               "(%u bytes, room for %u, and no '\"' in it); 0 anchors run\n", name,
               (unsigned)nl, (unsigned)(sizeof(line) - 19));
        return QUILL_FAIL;
    }
    memcpy(line, "quilltest churn \"", 17);
    memcpy(line + 17, name, nl);
    memcpy(line + 17 + nl, "\"", 2);

    QuillRecord rec = QuillRecordOf(1);
    int     fid = QuillCreate(name);
    int64_t w   = fid > 0 ? fwrite((uint32_t)fid, 0, &rec, sizeof(rec)) : 0;
    char    stem[64];
    QuillStem(name, stem, sizeof(stem));
    TouchTagPair pair = touch_intern(stem);
    TouchTag     tag  = touch_pair_choose(pair);
    if (fid <= 0 || w != (int64_t)sizeof(rec) || tag == TOUCH_TAG_INVALID ||
        touch_claim(tag, TOUCH_REST, 0, 0) != 0) {
        printf("[QUILL] race: FAIL — %s could not be laid down and listened to (create %d, "
               "write %lld, tag %u); 0 anchors run\n", name, fid, (long long)w, (uint32_t)tag);
        return QUILL_FAIL;
    }

    uint32_t gen   = 0;
    int      churn = proc_exec_gen(line, NULL, &gen);
    static QuillChurnWatch watch;
    watch = (QuillChurnWatch){ .pid = (uint32_t)churn, .gen = gen, .pair = pair };
    uint32_t watcher = churn > 0 ? strand_spawn_joinable(QuillRaceWatch, &watch) : 0;
    /* The churn's own write, not the echo of the one above: the WROTE event
     * carries the writer's pid. */
    int   hrc = 0;
    Touch first;
    first.source_pid = 0;
    if (watcher) {
        do {
            hrc = touch_await(tag, &first, 0);
        } while (hrc == 0 && first.source_pid != (uint32_t)churn &&
                 !__atomic_load_n(&watch.gone, __ATOMIC_ACQUIRE));
    }
    if (!watcher || hrc != 0 || first.source_pid != (uint32_t)churn) {
        bool gone = watcher && __atomic_load_n(&watch.gone, __ATOMIC_ACQUIRE);
        int  krc  = churn > 0 && !gone ? proc_finish((uint32_t)churn, gen) : 0;
        if (watcher)
            QuillRaceUnwatch(watcher, &watch);
        touch_release(tag);
        if (churn <= 0)
            printf("[QUILL] race: FAIL — the churn of %s would not start (answer %d); 0 anchors "
                   "run\n", name, churn);
        else if (gone)
            printf("[QUILL] race: FAIL — the churn of %s (pid %d) ended with exit %d before its "
                   "first write (gone %d); 0 anchors run\n", name, churn, watch.code, watch.grc);
        else
            printf("[QUILL] race: FAIL — the churn of %s (pid %d) could not be %s (answer %d), "
                   "so it was ended: %d; 0 anchors run\n", name, churn,
                   watcher ? "listened to" : "watched", watcher ? hrc : 0, krc);
        return QUILL_FAIL;
    }

    uint32_t anchored = 0, first_bad = 0, landed = 0;
    int      bad_rc = 0;
    for (uint32_t i = 0; i < rounds; i++) {
        int rc = anchor(0);
        if (rc == 0) {
            anchored++;
        } else if (!first_bad) {
            first_bad = i + 1;
            bad_rc = rc;
        }
        landed += QuillRaceHeard(tag, (uint32_t)churn);
    }
    QuillRaceUnwatch(watcher, &watch);
    touch_release(tag);
    if (anchored == rounds && landed > 0) {
        printf("[QUILL] race: PASS — anchored %u of %u while churn pid %d landed %u write(s) "
               "on %s fid=%u\n", anchored, rounds, churn, landed, name, (uint32_t)fid);
        return QUILL_PASS;
    }
    printf("[QUILL] race: FAIL — anchored %u of %u (anchor %u answered %d) while churn pid %d "
           "landed %u write(s) on %s fid=%u\n", anchored, rounds, first_bad, bad_rc, churn,
           landed, name, (uint32_t)fid);
    return QUILL_FAIL;
}

/* =========================================================================
 *  crowd — files that each add a record to the registry and a row to the table
 * ========================================================================= */

/* A count or a number as the shell gave it: digits only, at most ten, and no
 * more than a uint32_t holds. to_uint reads "-5" as 0 and 4294967296 as 0 as
 * well, and a case that took either for what it was given would make files
 * nobody asked for. */
bool QuillNumber(const char *word, uint32_t *out)
{
    size_t   len = word ? strlen(word) : 0;
    uint64_t v   = 0;
    if (len == 0 || len > 10)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (word[i] < '0' || word[i] > '9')
            return false;
        v = v * 10 + (uint64_t)(word[i] - '0');
    }
    if (v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* "crowd" and the number in ten digits: every name one length whatever the
 * number, so the tag a name gives its file is a registry record of one size
 * the host can count on — tools/powercut.sh chaingrow brings a volume to the
 * edge of a registry block with them. */
static void QuillCrowdName(uint32_t number, char out[QUILL_CROWD_NAME])
{
    memcpy(out, "crowd", 5);
    for (int d = 14; d >= 5; d--) {
        out[d] = (char)('0' + number % 10);
        number /= 10;
    }
    out[15] = '\0';
}

/* The tag a crowd name gives its file is the name itself (it has no dot), so
 * before the file is made no other may wear it: a file of this very name — an
 * earlier crowd's — is deleted, and one of any other name is the case's FAIL,
 * because the tag would then not be the new file's own. Returns 0 once none
 * wears it, *deleted counting the files of the name that went; 1 with *other
 * the id of a file of another name that wears it; or the answer of the query,
 * file_info or delete that stopped it. */
static int QuillCrowdClear(const char *name, uint32_t *deleted, uint32_t *other)
{
    uint32_t *ids = NULL;
    int n  = query_all_everywhere(name, &ids);
    int rc = n < 0 ? n : 0;
    for (int i = 0; i < n && rc == 0; i++) {
        file_info_t info;
        rc = file_info(ids[i], &info);
        if (rc == 0 && strcmp(info.filename, name) != 0) {
            *other = ids[i];
            rc = 1;
        } else if (rc == 0) {
            rc = delete(ids[i]);
            *deleted += rc == 0;
        }
    }
    free(ids);
    return rc;
}

/* N files crowdNNNNNNNNNN numbered from FROM, each wearing one tag no other
 * file on the volume wears — its name — so each adds one record to the tag
 * registry, flushed as the file is made (tagfs.c, "The name is a tag"), and
 * one row to the file table, which the anchor after the last one writes. A
 * name an earlier crowd made is made again, its old file deleted first: that
 * spends a file id and adds no record, and the verdict counts those apart. */
QuillVerdict QuillCrowd(const char *const *args)
{
    uint32_t count = 0, from = 0;
    if (!QuillNumber(args[0], &count) || count == 0 ||
        (args[1] && !QuillNumber(args[1], &from)) || count - 1 > UINT32_MAX - from) {
        printf("[QUILL] crowd: FAIL — N is a count above 0 and FROM a number, FROM + N - 1 at "
               "most %u; '%s' and '%s' are not (0 files made)\n", UINT32_MAX, args[0],
               args[1] ? args[1] : "0");
        return QUILL_FAIL;
    }

    char     name[QUILL_CROWD_NAME], first[QUILL_CROWD_NAME];
    uint32_t first_fid = 0, last_fid = 0, again = 0;
    uint64_t t0 = 0, t1 = 0;
    time_uptime_ms(&t0);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t deleted = 0, other = 0;
        QuillCrowdName(from + i, name);
        int crc = QuillCrowdClear(name, &deleted, &other);
        int fid = crc == 0 ? create_everywhere(name, NULL) : 0;
        if (fid <= 0) {
            int arc = anchor(0);
            if (crc == 1)
                printf("[QUILL] crowd: FAIL — at index %u of %u, %s: fid %u wears its tag under "
                       "another name, so the tag would not be the new file's own; %u made before "
                       "it (anchor %d)\n", i, count, name, other, i, arc);
            else
                printf("[QUILL] crowd: FAIL — at index %u of %u, %s could not be made (%s "
                       "answered %d); %u made before it (anchor %d)\n", i, count, name,
                       crc ? "clearing its name" : "create", crc ? crc : fid, i, arc);
            return QUILL_FAIL;
        }
        again += deleted > 0;
        if (i == 0) {
            memcpy(first, name, sizeof(first));
            first_fid = (uint32_t)fid;
        }
        last_fid = (uint32_t)fid;
        if ((i + 1) % QUILL_CROWD_PROGRESS == 0 && i + 1 < count)
            printf("[QUILL] crowd: %u of %u made, the last %s fid=%u\n", i + 1, count, name,
                   last_fid);
    }
    int arc = anchor(0);
    time_uptime_ms(&t1);
    if (arc != 0) {
        printf("[QUILL] crowd: FAIL — %u files %s..%s were made (fids %u..%u) and the anchor "
               "after them answered %d\n", count, first, name, first_fid, last_fid, arc);
        return QUILL_FAIL;
    }
    printf("[QUILL] crowd: PASS — made %u files %s..%s, fids %u..%u, each wearing a tag no other "
           "file wears (its name); %u of the names made again over a file of their own; "
           "anchored, %llu ms in all\n", count, first, name, first_fid, last_fid, again,
           (unsigned long long)(t1 - t0));
    return QUILL_PASS;
}
