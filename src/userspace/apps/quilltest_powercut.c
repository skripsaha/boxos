/*
 * quilltest_powercut.c — the modes of quilltest the power-cut stand drives.
 *
 * tools/powercut.sh records every write the machine makes, pulls the power
 * at every flush and at every subset of the writes between two, and boots
 * what is left. These modes give it something to cut: a write left
 * unanchored while one core is busy (hog, settle), a writer that never
 * stops (churn), anchors racing that writer (race), and the read that says
 * afterwards whether a record survived (verify). Each says what it did in
 * quilltest's four words, with the numbers the judge needs to compute the
 * bytes a surviving file must hold. See quilltest.c for the rest.
 */

#include "box/print.h"
#include "box/file.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/system.h"
#include "box/time.h"
#include "box/touch.h"
#include "box/error.h"
#include "box/core/cabin.h"
#include "quilltest.h"

#define QUILL_CHURN_PROGRESS 100u

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
 * on the hog for good — the power goes while the machine has a write it has
 * not settled and a core that is busy. `verify NAME` after the cut says
 * whether the write survived. */
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
    int64_t w   = fid > 0 ? QuillWrite((uint32_t)fid, 0, &rec, sizeof(rec)) : 0;
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
    printf("[QUILL] settle: the hog (pid %d) was ended from outside (gone %d, exit %d)\n",
           hog, grc, code);
    return QUILL_PENDING;
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

/* A writer that never stops: NAME's record rewritten with the next seq, over
 * and over, never anchored. Carries on from the record NAME already holds, so
 * `race` can lay the file down first and hand it over. */
QuillVerdict QuillChurn(const char *const *args)
{
    const char *name = args[0];
    uint32_t ids[1];
    uint32_t seq = 1;
    int n = QuillFindEverywhere(name, ids, 1);
    int fid;
    if (n > 0) {
        fid = (int)ids[0];
        seq = QuillReadSeq(ids[0]) + 1;
    } else {
        fid = create_everywhere(name, NULL);
    }
    if (fid <= 0) {
        printf("[QUILL] churn: FAIL — %s could not be found or created (answers %d, %d)\n",
               name, n, fid);
        return QUILL_FAIL;
    }
    printf("[QUILL] churn: %s fid=%u starts at seq=%u — it writes until the machine stops\n",
           name, (uint32_t)fid, seq);
    for (;; seq++) {
        QuillRecord rec = QuillRecordOf(seq);
        int64_t     w   = QuillWrite((uint32_t)fid, 0, &rec, sizeof(rec));
        if (w != (int64_t)sizeof(rec)) {
            printf("[QUILL] churn: FAIL — writing seq=%u of %s fid=%u answered %lld\n",
                   seq, name, (uint32_t)fid, (long long)w);
            return QUILL_FAIL;
        }
        if (seq % QUILL_CHURN_PROGRESS == 0)
            printf("[QUILL] churn: %s fid=%u wrote seq=%u\n", name, (uint32_t)fid, seq);
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

/* ROUNDS anchors of the whole volume while a churn writes NAME in another
 * cabin — an anchor racing a writer for the pool's dirty mark. The anchors
 * start on the churn's first write, heard as the WROTE event on NAME's stem
 * tag, never on a guess at how long a spawn takes, and the churn's writes
 * heard while they run say the race really ran. The churn is left writing. */
QuillVerdict QuillRace(const char *const *args)
{
    const char *name   = args[0];
    uint32_t    rounds = is_number(args[1]) ? to_uint(args[1]) : 0;
    if (rounds == 0) {
        printf("[QUILL] race: FAIL — ROUNDS is a count above 0, and '%s' is not (0 anchors run)\n",
               args[1]);
        return QUILL_FAIL;
    }
    QuillRecord rec = QuillRecordOf(1);
    int     fid = QuillCreate(name);
    int64_t w   = fid > 0 ? QuillWrite((uint32_t)fid, 0, &rec, sizeof(rec)) : 0;
    char    stem[64];
    QuillStem(name, stem, sizeof(stem));
    TouchTag tag = touch_pair_choose(touch_intern(stem));
    if (fid <= 0 || w != (int64_t)sizeof(rec) || tag == TOUCH_TAG_INVALID ||
        touch_claim(tag, TOUCH_REST, 0, 0) != 0) {
        printf("[QUILL] race: FAIL — %s could not be laid down and listened to (create %d, "
               "write %lld, tag %u)\n", name, fid, (long long)w, (uint32_t)tag);
        return QUILL_FAIL;
    }

    char line[96];
    size_t nl = strlen(name);
    memcpy(line, "quilltest churn ", 16);
    memcpy(line + 16, name, nl + 1);
    uint32_t gen = 0;
    int churn = nl < sizeof(line) - 16 ? proc_exec_gen(line, NULL, &gen) : -(int)ERR_INVALID_ARGUMENT;
    /* The churn's own write, not the echo of the one above: the WROTE event
     * carries the writer's pid. */
    int hrc = churn;
    if (churn > 0) {
        Touch first;
        do {
            hrc = touch_await(tag, &first, 0);
        } while (hrc == 0 && first.source_pid != (uint32_t)churn);
    }
    if (churn <= 0 || hrc != 0) {
        touch_release(tag);
        printf("[QUILL] race: FAIL — the churn of %s did not start writing (spawn %d, heard %d)\n",
               name, churn, hrc);
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
