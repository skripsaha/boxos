#include "sewing.h"
#include "../ink/ink.h"
#include "../helm/helm.h"
#include "../scribe/scribe.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../settle_cut/settle_cut.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "boardroom.h"
#include "crypto.h"

/*
 * The Settle cycle of a format-v2 volume (sewing.h): one stitch on its ribbon.
 *
 *   TAKE         S0: under the state lock, memory only — the target (every
 *                Ink so far), the cycle's flags, the Use Context owed; the
 *                tags given ids since the last stitch settled; the
 *                next_quire asked for first, with no lock held, when the
 *                ribbon has none
 *   BUILD        no lock: the stream — StitchHead naming the last stitch by
 *                its seq and its Clasp, the Coineds, the UseContext, the Clasp
 *                — its place on the ribbon, its seq, and its sectors sealed
 *   STITCH       its atoms, a window the seat takes at a time — no FLUSH#1
 *                before them: that is only for a stitch that names fresh data
 *                (§16.2 п.7), and no stitch of this kernel names any; each
 *                says DATA_FLUSHED, with nothing to flush ahead of it
 *   LAST_FLUSH   S5: the cycle's one flush that must be kept
 *   DONE         S6: under the state lock, the target settled — and the Use
 *                Context with it — the tags it laid down the book's, the
 *                cursor past it; then its waiters are answered (settle.c)
 *
 * ‼ A WRITE THE MEDIUM REFUSES IS TAKEN AGAIN WHERE NOTHING OF IT LIES AND
 * THE MOUNT'S READER LOOKS (M-6, §3.2, §4.5 п.5 (б)). Any refusal once the
 * stitch's writing began — an atom, the last flush — sends the cycle back to
 * BUILD with a new seq at the start of the next_quire the last stitch sewn
 * names, the Quire it stood in closed early: the attempt that may have landed
 * in part is never written over, nor written again on a block that refused it.
 * Where no reader would take a retry up (ribbon.c, RibbonRetry) — a retry
 * refused in its turn among them — the book takes no write: the volume may
 * only be read, said with the refusals in a row and why, every waiter told
 * so, never answered OK, and no cycle runs on it again (settle.c). A medium
 * that left or takes no write is not the book's to go on (away.h).
 */

typedef enum {
    STITCH_STEP_TAKE = 0,
    STITCH_STEP_BUILD,
    STITCH_STEP_STITCH,
    STITCH_STEP_LAST_FLUSH,
    STITCH_STEP_DONE,
} StitchStep;

typedef struct {
    SettleWhy   why;
    StitchStep  step;
    uint64_t    target;       /* every Ink up to here */
    uint64_t    cycle;
    uint64_t    flags;        /* StitchHead's */
    uint64_t    made_ns;
    uint32_t    coined_lo;    /* the tags laid down as Coineds: [lo, hi) */
    uint32_t    coined_hi;
    bool        use_owed;     /* a word the person said is owed the medium */
    UseText*    use;          /* that word, held; NULL says none */
    uint64_t    use_ticket;
    uint64_t    seq;          /* this attempt's */
    uint32_t    clasp;
    uint8_t*    sectors;      /* the attempt sealed: `of` sectors */
    uint32_t    of;
    RibbonPlace at;           /* where it lies, and the Quire it runs on into */
    RibbonQuire hop;
    uint32_t    written;      /* its sectors on the medium so far */
    bool        ran_on;       /* a write of it was sent into `hop` */
    const char* stage;
} StitchSnap;

static StitchSnap s_stitch;

/* No Quire took the write: the book takes none until the volume is mounted
 * again. And the refusal of a change to a file, said once a mount. */
static volatile uint8_t s_book_refused;
static volatile uint8_t s_files_said;

/* A part of format v1's cycle asked of a book, said once a mount. */
static volatile uint8_t s_v1_said;

void StitchMountReset(void)
{
    __atomic_store_n(&s_book_refused, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_files_said, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_v1_said, 0u, __ATOMIC_RELEASE);
}

bool SettleV1OnBook(const char* what)
{
    if (tagfs_get_state()->format != DEED_FORMAT_V2)
        return false;
#if CONFIG_STRICT
    panic("[Settle] DEFECT: %s, a part of format v1's cycle, was asked of a format-v2 volume", what);
#endif
    if (!__atomic_exchange_n(&s_v1_said, 1u, __ATOMIC_ACQ_REL))
        kprintf("[Settle] DEFECT: %s, a part of format v1's cycle, was asked of a format-v2 volume — it does "
                "nothing there\n", what);
    return true;
}

bool StitchBookRefused(void)
{
    return __atomic_load_n(&s_book_refused, __ATOMIC_ACQUIRE) != 0;
}

error_t StitchFilesRefused(void)
{
    if (!__atomic_exchange_n(&s_files_said, 1u, __ATOMIC_ACQ_REL))
        kprintf("[TagFS] this kernel writes the names of tags and the volume's memory into a format-v2 book, and "
                "not yet a change to a file: a file is not made, named, tagged, given up or written here "
                "(ERR_TAGFS_READ_ONLY)\n");
    return ERR_TAGFS_READ_ONLY;
}

// ----------------------------------------------------------------------------
// TAKE
// ----------------------------------------------------------------------------

static void StitchTake(StitchSnap* s)
{
    TagFSState* fs = tagfs_get_state();
    RibbonNextGiven();
    SettleWaitersRunBegins();
    (void)CadenceRunBegins();
    TagFSLockTake(&fs->lock);
    s->target    = InkedGen();
    s->cycle     = SettledCycles() + 1u;
    s->flags     = STITCH_DATA_FLUSHED | (s->why == SETTLE_FOR_HALT && HelmOwed() == 0 ? STITCH_CLEAN : 0u) |
                   (RibbonCursorLied() ? STITCH_MEDIUM_LIED : 0u);
    s->use_owed  = UseMemoryStitchTake(&s->use, &s->use_ticket);
    TagFSLockDrop(&fs->lock);
    /* After the target: every tag a record up to it wears was given before
     * its Ink, so it is below the range's end. */
    TagRegistryCoinedRange(fs->registry, &s->coined_lo, &s->coined_hi);
    s->made_ns = rtc_get_unix64() * 1000000000ull;
    SETTLE_CUT_CYCLE(s->target);
}

// ----------------------------------------------------------------------------
// BUILD
// ----------------------------------------------------------------------------

/* The bytes a record's tag and length take at the most, a StitchHead's whole
 * record and a Clasp's — what the stream is measured by before it is laid. */
#define STITCH_RECORD_HEAD_MOST  15u
#define STITCH_HEAD_MOST        (STITCH_RECORD_HEAD_MOST + 9u * 10u + 4u)
#define STITCH_CLASP_MOST       (STITCH_RECORD_HEAD_MOST + 2u * 10u + 4u)

/* What the stitch carries, measured: its records, the bytes they take at the
 * most and the body of the longest. */
typedef struct {
    UseText* use;          /* the word written, when it differs from what the book holds */
    bool     use_written;
    uint32_t coined;       /* the Coineds of them */
    uint64_t records;
    uint64_t bytes;
    uint64_t body;
} StitchLoad;

/* Tag `id` as a Coined carries it: false when the registry gave no tag that id. */
static bool StitchCoinedTag(uint32_t id, TallyTag* t)
{
    TagSpelling sp;
    if (!TagRegistrySpelling(tagfs_get_state()->registry, id, &sp, NULL))
        return false;
    *t = (TallyTag){ .flags = sp.value ? TALLY_TAG_HAS_VALUE : 0u, .key = (const uint8_t*)sp.key,
                     .key_len = sp.key_len, .value = (const uint8_t*)sp.value, .value_len = sp.value_len };
    return true;
}

/* The records measured: every tag given an id since the last stitch settled,
 * and the word owed when the book does not hold it already (§7: the same text
 * is written never again). */
static void StitchMeasure(const StitchSnap* s, StitchLoad* l)
{
    memset(l, 0, sizeof(*l));
    l->bytes = STITCH_HEAD_MOST + STITCH_CLASP_MOST;
    l->body  = STITCH_HEAD_MOST > STITCH_CLASP_MOST ? STITCH_HEAD_MOST : STITCH_CLASP_MOST;
    for (uint32_t id = s->coined_lo; id < s->coined_hi; id++) {
        TallyTag t;
        if (!StitchCoinedTag(id, &t))
            continue;
        uint64_t body = 3u * 10u + t.key_len + t.value_len;
        l->coined++;
        l->records++;
        l->bytes += STITCH_RECORD_HEAD_MOST + body;
        l->body   = body > l->body ? body : l->body;
    }
    if (s->use_owed) {
        UseText* memory = tagfs_recall_use_context(NULL);
        l->use_written  = !UseTextSame(memory, s->use);
        UseTextDrop(memory);
    }
    if (l->use_written) {
        uint64_t body = 10u + (s->use ? s->use->bytes : 0u);
        l->use     = s->use;
        l->records++;
        l->bytes  += STITCH_RECORD_HEAD_MOST + body;
        l->body    = body > l->body ? body : l->body;
    }
}

/* The stream laid into `out`: StitchHead, the records `l` measured, Clasp;
 * `starts` each record's offset, *nstarts how many — never more than the
 * records measured and the two around them, which `starts` holds. Its length,
 * and the Clasp's sum in *clasp. */
static uint32_t StitchStream(StitchSnap* s, const StitchLoad* l, uint8_t* out, uint8_t* body, uint32_t* starts,
                             uint32_t* nstarts, uint32_t* clasp)
{
    const RibbonCursor* c = RibbonCursorNow();
    bool                came = c->off == 0 && c->came.blocks;
    StitchHead          h = { .flags = s->flags, .prev = c->last ? s->seq - c->last : 0, .prev_clasp = c->clasp,
                              .tally = s->seq - c->tally, .made_ns = s->made_ns, .next_block = c->next.block,
                              .next_blocks = c->next.blocks, .from_block = came ? c->came.block : 0,
                              .from_blocks = came ? c->came.blocks : 0 };
    uint32_t n = 0, at = RibbonRecordPut(out, RIBBON_STITCH_HEAD, 0, body, StitchHeadBodyPut(body, &h));
    starts[n++] = 0;
    for (uint32_t id = s->coined_lo; id < s->coined_hi && n <= l->coined; id++) {
        TallyTag t;
        if (!StitchCoinedTag(id, &t))
            continue;
        starts[n++] = at;
        at += RibbonRecordPut(out + at, RIBBON_COINED, 0, body, RibbonCoinedBodyPut(body, id, &t));
    }
    if (l->use_written) {
        starts[n++] = at;
        at += RibbonRecordPut(out + at, RIBBON_USE_CONTEXT, 0, body,
                              TallyUsePut(body, l->use ? (const uint8_t*)l->use->text : NULL,
                                          l->use ? l->use->bytes : 0u));
    }
    Clasp clasped = { .records = n - 1u, .fresh = 0, .crc = KCrc32c(out, at) };
    *clasp      = clasped.crc;
    starts[n++] = at;
    *nstarts    = n;
    return at + RibbonRecordPut(out + at, RIBBON_CLASP, 0, body, ClaspBodyPut(body, &clasped));
}

static void StitchSectorsLetGo(StitchSnap* s)
{
    kfree(s->sectors);
    s->sectors = NULL;
    s->of      = 0;
    s->written = 0;
    s->ran_on  = false;
}

/*
 * BUILD: the stitch measured, given its place and its seq, laid and sealed —
 * the heap's, never a frame's. ERR_NO_MEMORY parks the cycle for the
 * machine's pages; ERR_TAGFS_NO_SPACE is the ribbon's own refusal (RibbonRoom),
 * said there.
 */
static error_t StitchBuild(StitchSnap* s)
{
    StitchLoad l;
    StitchSectorsLetGo(s);
    StitchMeasure(s, &l);
    uint8_t*  stream = kmalloc(l.bytes);
    uint8_t*  body   = kmalloc(l.body);
    uint32_t* starts = kmalloc(sizeof(uint32_t) * (l.records + 2u));
    error_t   rc     = stream && body && starts ? OK : ERR_NO_MEMORY;
    uint32_t  len    = 0, nstarts = 0;
    if (rc == OK) {
        s->seq = RibbonSeqNext();
        len    = StitchStream(s, &l, stream, body, starts, &nstarts, &s->clasp);
        s->of  = (uint32_t)RibbonStitchSectors(len, RibbonCursorNow()->atom);
        rc     = RibbonRoom(s->of, nstarts == 2u, &s->at, &s->hop);
    }
    if (rc == OK && !(s->sectors = kmalloc((uint64_t)s->of * BOOK_SECTOR_BYTES)))
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        uint64_t left = s->at.quire.blocks * TAGFS_BLOCK_SECTORS - s->at.off;
        RibbonStitchSeal(s->sectors, stream, len, starts, nstarts, s->seq, RibbonPlaceVlba(&s->at), left,
                         s->hop.block * TAGFS_BLOCK_SECTORS, s->of, tagfs_get_state()->uuid);
    } else {
        s->of = 0;
    }
    kfree(stream);
    kfree(body);
    kfree(starts);
    return rc;
}

// ----------------------------------------------------------------------------
// STITCH
// ----------------------------------------------------------------------------

/* The window of the stitch written next: from sector `written` on, as many as
 * the seat takes in one command, inside one Quire — and the last atom a write
 * of its own, so the cut that stops before it tears a stitch of more than one
 * atom (SETTLECUT `stitch`). Its first sector's address in *vlba. */
static uint32_t StitchWindow(const StitchSnap* s, uint64_t* vlba)
{
    uint64_t left  = s->at.quire.blocks * TAGFS_BLOCK_SECTORS - s->at.off;
    uint32_t atom  = RibbonCursorNow()->atom;
    uint64_t n     = s->of - s->written;
    uint64_t most  = BoardroomSeatRun(tagfs_get_seat());
    if (s->written < left) {
        *vlba = RibbonPlaceVlba(&s->at) + s->written;
        n     = n < left - s->written ? n : left - s->written;
    } else {
        *vlba = s->hop.block * TAGFS_BLOCK_SECTORS + (s->written - left);
    }
    if (s->written + n > s->of - atom && s->written < s->of - atom)
        n = s->of - atom - s->written;
    return (uint32_t)(n < most ? n : most);
}

/* One window of the stitch to the medium: *done once its last sector is. */
static error_t StitchWrite(StitchSnap* s, bool* done)
{
    uint64_t vlba = 0;
    uint32_t n    = StitchWindow(s, &vlba);
    if (s->written == s->of - RibbonCursorNow()->atom)
        SETTLE_CUT_AT(SETTLE_CUT_POINT_STITCH);
    s->ran_on = s->ran_on || s->written >= s->at.quire.blocks * TAGFS_BLOCK_SECTORS - s->at.off;
    int rc = tagfs_volume_write(vlba, n, s->sectors + (uint64_t)s->written * BOOK_SECTOR_BYTES);
    if (rc != 0)
        return rc == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY
             : rc == (int)ERR_TAGFS_READ_ONLY  ? ERR_TAGFS_READ_ONLY : ERR_WRITE_FAILED;
    s->written += n;
    *done = s->written == s->of;
    return OK;
}

/* A refusal once the stitch's writing began (M-6): OK, and the cycle back at
 * BUILD with the ribbon gone on to the next_quire the last stitch sewn names;
 * or the answer the cycle ends with — the medium's own when it left or takes
 * no write, and ERR_TAGFS_READ_ONLY where no reader would take a retry up. */
static error_t StitchRefused(StitchSnap* s, error_t rc)
{
    TagFSWriteRefused();
    if (rc == ERR_NO_MEMORY || rc == ERR_DEVICE_NOT_READY || TagFSMediumLeft() || TagFSReadOnly())
        return rc;
    const char* nowhere = RibbonRetry(s->ran_on);
    if (nowhere) {
        __atomic_store_n(&s_book_refused, 1u, __ATOMIC_RELEASE);
        kprintf("[TagFS] the book takes no write: %u refusal(s) in a row, the last of stitch %llu at %s — %s, "
                "the one place a mount's reader takes a retry up, so none is written: the volume may only be "
                "read from here (ERR_TAGFS_READ_ONLY)\n", RibbonCursorNow()->refused, (unsigned long long)s->seq,
                s->stage, nowhere);
        SettleKnock();
        return ERR_TAGFS_READ_ONLY;
    }
    if (RibbonCursorNow()->refused == 1u)
        kprintf("[TagFS] the medium refused stitch %llu at %s (error %u): the cycle goes on with a new seq at the "
                "start of the next_quire, block %llu, the Quire it stood in closed (M-6)\n",
                (unsigned long long)s->seq, s->stage, (unsigned)rc,
                (unsigned long long)RibbonCursorNow()->quire.block);
    RibbonNextGiven();
    s->step = STITCH_STEP_BUILD;
    return OK;
}

// ----------------------------------------------------------------------------
// The cycle
// ----------------------------------------------------------------------------

void StitchRunBegin(SettleWhy why)
{
    StitchSectorsLetGo(&s_stitch);
    UseTextDrop(s_stitch.use);
    memset(&s_stitch, 0, sizeof(s_stitch));
    s_stitch.why  = why;
    s_stitch.step = STITCH_STEP_TAKE;
}

error_t StitchRunStep(bool* done)
{
    StitchSnap* s    = &s_stitch;
    error_t     rc   = OK;
    bool        last = false;
    switch (s->step) {
    case STITCH_STEP_TAKE:
        StitchTake(s);
        s->step = STITCH_STEP_BUILD;
        break;
    case STITCH_STEP_BUILD:
        s->stage = "the stitch's making";
        rc       = StitchBuild(s);
        if (rc == OK)
            s->step = STITCH_STEP_STITCH;
        break;
    case STITCH_STEP_STITCH:
        s->stage = "the stitch";
        rc       = StitchWrite(s, &last);
        if (rc == OK && last)
            s->step = STITCH_STEP_LAST_FLUSH;
        break;
    case STITCH_STEP_LAST_FLUSH:
        SETTLE_CUT_AT(SETTLE_CUT_POINT_FLUSH);
        s->stage = "the flush after the stitch";
        rc       = VolumeFlushFor(VolumeFlushNeed());
        if (rc == OK)
            s->step = STITCH_STEP_DONE;
        break;
    case STITCH_STEP_DONE:
        break;
    }
    if (rc != OK && s->step >= STITCH_STEP_STITCH)
        rc = StitchRefused(s, rc);
    *done = rc != OK || s->step == STITCH_STEP_DONE;
    return rc;
}

uint64_t StitchRunTarget(void)
{
    return s_stitch.target;
}

const char* StitchRunStage(void)
{
    return s_stitch.stage ? s_stitch.stage : "the stitch's snapshot";
}

/* DONE, under the state lock: the target settled, and the word the stitch
 * carried — or the one the book held already — the volume's; then, with no
 * lock, the tags it laid down the book's and the ribbon's cursor past it. */
void StitchRunEnd(error_t rc)
{
    StitchSnap* s    = &s_stitch;
    UseText*    gone = NULL;
    if (rc == OK) {
        TagFSState* fs = tagfs_get_state();
        TagFSLockTake(&fs->lock);
        SettleSettledLocked(s->target, s->cycle, (s->flags & STITCH_CLEAN) != 0);
        if (s->use_owed) {
            gone   = UseMemoryStitchSettled(s->use, s->use_ticket);
            s->use = NULL;
        }
        TagFSLockDrop(&fs->lock);
        TagRegistryCoinedUpTo(fs->registry, s->coined_hi);
        RibbonSewn(&s->at, &s->hop, s->of, s->seq, s->clasp, s->flags);
    }
    UseTextDrop(gone);
    UseTextDrop(s->use);
    s->use = NULL;
    StitchSectorsLetGo(s);
}
