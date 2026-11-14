#include "sewing.h"
#include "../ink/commonhold.h"
#include "binder.h"
#include "../ink/ink.h"
#include "../ink/docket.h"
#include "../ink/fallow.h"
#include "../helm/helm.h"
#include "../scribe/scribe.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../settle_cut/settle_cut.h"
#include "../integrity/gloss.h"
#include "../mount/errata.h"
#include "loose_leaves.h"
#include "../helm/engross.h"
#include "../mount/away.h"
#include "amp.h"
#include "dog_ear.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "boardroom.h"
#include "kheap.h"
#include "read_pace.h"
#include "crypto.h"

/*
 * The Settle cycle of a format-v2 volume (sewing.h): one stitch on its ribbon.
 *
 *   TAKE         S0: under the state lock, memory only — the target (every
 *                Ink so far) and the Docket with it (docket.h), the ribbon's
 *                bytes their Inks claimed, the cycle's flags, the Use Context
 *                owed, the tags given ids since the last stitch settled, and
 *                the most the stitch can take, the room claims are judged by
 *                lying past it (RibbonFlightBegins); the next_quire asked for
 *                first, with no lock held, when the ribbon has none; the
 *                Quire closed early when room is asked for and Fallow waits
 *                for it to be left; the pages Fallow's gives may want held;
 *                and what its toll is judged by: the last cycle's D less the
 *                flush of its data, and the pace the medium's reads were seen
 *                to keep (StitchToll)
 *   BUILD        no lock: the stream — StitchHead naming the last stitch by
 *                its seq and its Clasp, the Undones of the mount's verdict,
 *                the Coineds, the UseContext, a record of each file the
 *                Docket names by fid, the Clasp summing the blocks of fresh
 *                data they name — its toll, its place on the ribbon, its seq,
 *                and its sectors sealed
 *   FIRST_FLUSH  only for a stitch whose toll is the flush of its data: the
 *                flush that keeps every block the Inks it carries wrote,
 *                before any atom of the stitch that names them
 *   STITCH       its atoms, a window the seat takes at a time; a stitch that
 *                paid its toll, or names no fresh data to flush ahead of it,
 *                says DATA_FLUSHED
 *   LAST_FLUSH   S5: the cycle's one flush that must be kept
 *   DONE         S6: the cursor past it; under the state lock, the target
 *                settled — and the Use Context with it, or the word it had no
 *                room for refused — the freers it carried named by its seq,
 *                the Quires it left told to Fallow and what ripened given
 *                back; then the tags it laid down the book's, the files it
 *                carried given its seq, and the flight over; then its waiters
 *                are answered (settle.c). A cycle refused gives the Docket it
 *                took back, under what came since, and its claims with it
 *
 * ‼ ON THE MEDIUM: THE DATA, [THE FLUSH], THE STITCH, THE FLUSH (§3.2, §3.3).
 * A stitch names blocks of fresh data that were written before its TAKE, and
 * a medium may keep its writes in any order until a flush is kept: a stitch
 * that landed with the data it names not landed would name blocks that hold
 * nothing of the file. Either the stitch is begun only once the flush of that
 * data is kept, and says so (DATA_FLUSHED) — the mount need not read those
 * blocks back to believe it — or it is not, and the mount reads every one of
 * them back against the digest its record carries, rolling a file whose data
 * did not land back by its splice (mount/ribbon_verify.c). Which, the toll
 * decides (StitchToll): the flush where the check would cost more than a
 * cycle does.
 *
 * ‼ A WRITE THE MEDIUM REFUSES IS TAKEN AGAIN WHERE NOTHING OF IT LIES AND
 * THE MOUNT'S READER WALKS (M-6, §3.2, §4.5 п.5 (б), §16.4 п.4). Any refusal
 * once the stitch's writing began — the flush of its data, an atom, the last
 * flush — sends the cycle
 * back to BUILD with a new seq in the next_quire the last stitch sewn names:
 * at its start, the Quire it stood in closed early, or on the atom past a
 * refused attempt lying there — so the attempt that may have landed in part
 * is never written over, nor written again on a block that refused it. Where
 * no reader would take a retry up (ribbon.c, RibbonRetry) — no next_quire
 * named, or one the refusals have run to its end — the book takes no write:
 * the volume may only be read, said with the refusals in a row and why, every
 * waiter told so, never answered OK, and no cycle runs on it again
 * (settle.c). A medium that left or takes no write is not the book's to go
 * on (away.h).
 */

typedef enum {
    STITCH_STEP_TAKE = 0,
    STITCH_STEP_BUILD,
    STITCH_STEP_FIRST_FLUSH,
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
    bool        use_out;      /* the word had no room on the ribbon: the stitch is laid without it */
    DocketTaken jots;         /* the files' changes up to the target, by fid */
    uint64_t    claimed;      /* the ribbon's bytes their Inks claimed, taken (RibbonClaimTaken) */
    uint64_t    flight;       /* the sectors TAKE counted it in flight at the most (StitchFlight) */
    uint64_t    data_need;    /* the flush that keeps every block the Inks up to the target wrote */
    uint64_t    d_us;         /* the toll's measures, taken at S0: the last cycle's D, */
    ReadPaceSeen pace;        /* and the pace the medium's reads were seen to keep */
    bool        toll;         /* the stitch pays the flush of its data before its atoms */
    uint64_t    fresh;        /* the blocks of fresh data the stitch names (Clasp.fresh) */
    uint64_t    flush_us;     /* what the flush of its data took the medium: no part of D (§16.5 п.1) */
    uint64_t    seq;          /* this attempt's */
    uint32_t    clasp;
    uint32_t    clasp_at;     /* where its Clasp lies in its stream: its crc is of every byte before */
    RibbonTry*  laid;         /* the attempt, kept past its refusal or its cut (ribbon_left.c) */
    uint8_t*    sectors;      /* the attempt sealed: `of` sectors, laid's */
    uint32_t    of;
    uint32_t    useful;       /* the bytes of its records, StitchHead to Clasp (§4.2) */
    RibbonPlace at;           /* where it lies, and the Quire it runs on into */
    RibbonQuire hop;
    uint32_t    written;      /* its sectors on the medium so far */
    const char* stage;
} StitchSnap;

static StitchSnap s_stitch;

/* What the flush of its data took the medium in the last cycle that settled —
 * the cycle whose busy time the cadence keeps as D (cadence.c, s_d_us): the
 * Settle's holder's alone, written as that cycle ends and read as the next
 * one's S0 takes the toll's D. */
static uint64_t s_settled_flush_us;

/* No Quire took the write: the book takes none until the volume is mounted
 * again. */
static volatile uint8_t s_book_refused;

/* A part of format v1's cycle asked of a book, said once a mount. */
static volatile uint8_t s_v1_said;

void StitchMountReset(void)
{
    __atomic_store_n(&s_book_refused, 0u, __ATOMIC_RELEASE);
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

/* The core putting the verdict's tags on, + 1; 0 while none is. */
static volatile uint32_t s_marking;

bool StitchMarksVerdict(void)
{
    return __atomic_load_n(&s_marking, __ATOMIC_ACQUIRE) == (uint32_t)amp_get_core_index() + 1u;
}

// ----------------------------------------------------------------------------
// TAKE
// ----------------------------------------------------------------------------

/*
 * ‼ ROOM ASKED FOR CLOSES THE QUIRE EARLY, WITH A STITCH THAT CARRIES NOTHING
 * (В1, §3.4). Ground a stitch gave up comes back once the ribbon has left that
 * stitch's Quire; a cycle that answers a want of room while Fallow waits for
 * that begins at the start of the next_quire instead (RibbonCloseEarly). Only
 * a cycle that carries nothing does: the next_quire alone may have no
 * next_quire of its own to give yet, and its rest takes what frees and nothing
 * more (§3.6) — a cycle with changes lays them where the ribbon stands, and the
 * asker's next round closes the Quire.
 */
static void StitchCloseForRoom(StitchSnap* s, bool awaits)
{
    if (awaits && s->claimed == 0 && s->jots.count == 0 && !s->use_owed && s->coined_lo == s->coined_hi &&
        RibbonCloseEarly())
        RibbonNextGiven();
}

/* The sectors the stitch TAKE took can take at the most: its claims — the
 * Labeleds of the mark the verdict's files bear among them — the Undones
 * of the mount's verdict, the Coineds of the tags given ids since the last
 * stitch settled — `coined` of them, `text` bytes of keys and values, every
 * one in its range — the word owed, and what every stitch carries. */
static uint64_t StitchFlight(const StitchSnap* s, uint32_t coined, uint64_t text)
{
    uint64_t bytes = STITCH_HEAD_MOST + STITCH_CLASP_MOST + s->claimed + ErrataBytes() +
                     (uint64_t)coined * STITCH_COINED_MOST + text;
    if (s->use_owed)
        bytes += STITCH_USE_MOST + (s->use ? s->use->bytes : 0u);
    return RibbonStitchOf(bytes);
}

static void StitchTake(StitchSnap* s)
{
    TagFSState* fs = tagfs_get_state();
    uint32_t    coined = 0;
    uint64_t    text   = 0;
    SettleWaitersRunBegins();
    uint32_t demands = CadenceRunBegins();
    bool     room    = s->why == SETTLE_FOR_ROOM || (demands & SETTLE_DEMAND_ROOM);
    TagFSLockTake(&fs->lock);
    s->target    = InkedGen();
    s->data_need = InkDataNeed();
    s->cycle     = SettledCycles() + 1u;
    s->flags     = (s->why == SETTLE_FOR_HALT && HelmOwed() == 0 ? STITCH_CLEAN : 0u) |
                   (RibbonCursorLied() ? STITCH_MEDIUM_LIED : 0u);
    s->use_owed  = UseMemoryStitchTake(&s->use, &s->use_ticket);
    s->claimed   = RibbonClaimTaken();
    bool awaits  = room && FallowAwaitsQuire();
    DocketTakeLocked(&s->jots);
    /* After the target: every tag a record up to it wears was given before
     * its Ink, so it is below the range's end; and what the registry holds
     * after the range holds every Coined in it. The registry's lock is a
     * leaf, taken under the state lock (lock_rank.h). */
    TagRegistryCoinedRange(fs->registry, &s->coined_lo, &s->coined_hi);
    TagRegistryCoinedHeld(fs->registry, &coined, &text);
    s->flight = StitchFlight(s, coined, text);
    RibbonFlightBegins(s->flight);
    TagFSLockDrop(&fs->lock);
    StitchCloseForRoom(s, awaits);
    FallowGiveRoomHold(s->cycle);
    /* The toll's D is the last settled cycle's less the flush of its own data
     * (§16.5 п.1), so a flush paid never spares the next cycle its own; the
     * cadence's D — the Lull, the medium's share, the small writer's measure —
     * is the whole of that cycle's time on the medium, the flush with it. */
    uint64_t d = SettleLastDUs();
    s->d_us    = d > s_settled_flush_us ? d - s_settled_flush_us : 0;
    ReadPaceTake(&s->pace);
    s->made_ns = rtc_get_unix64() * 1000000000ull;
    SETTLE_CUT_CYCLE(s->target);
}

/*
 * ‼ THE MARK GOES DOWN WITH THE VERDICT (§16.5 п.6, M-10). Each file the
 * mount's verdict rolls back is marked so in its record (TAGFS_FILE_UNDONE) by
 * an ordinary Ink before the snapshot of the cycle that lays the verdict down —
 * so the one stitch carries both, and no mount finds the Undone without the
 * mark of a file that lives. Every cycle until one settles carries the verdict (a
 * stitch that settled without it would leave the next mount to replay what
 * it undoes, H-1), so it is the first cycle that begins — asked at the door's
 * opening (mount.c, ErrataAsk) — that marks them, and a cycle refused before
 * the last of them marks the rest in the next. Off the heap's reserve, which
 * is the steps' own: a refusal of the moment — the heap (SETTLE_PARK_HEAP), the
 * ribbon's room (tried a window on), a volume taking no change now — refuses
 * the cycle, never laying the verdict without them (errata.h, ErrataMarked). A
 * stopping machine's halt refuses every change not begun; these are its own
 * cycle's, made before its snapshot, and pass (StitchMarksVerdict).
 */
static error_t StitchMarked(StitchSnap* s)
{
    if (!ErrataHeld())
        return OK;
    KHeapScope was = KHeapScopeEnter(KHEAP_CHARGE_KERNEL, 0);
    __atomic_store_n(&s_marking, (uint32_t)amp_get_core_index() + 1u, __ATOMIC_RELEASE);
    error_t    rc  = ErrataMarked();
    __atomic_store_n(&s_marking, 0u, __ATOMIC_RELEASE);
    KHeapScopeLeave(was);
    if (rc != OK)
        s->stage = "the mark the mount's verdict owes its files";
    return rc;
}

// ----------------------------------------------------------------------------
// BUILD
// ----------------------------------------------------------------------------

/* What the stitch carries, measured: its records, the bytes they take at the
 * most and the body of the longest. */
typedef struct {
    UseText* use;          /* the word written, when it differs from what the book holds */
    bool     use_written;
    uint32_t coined;       /* the Coineds of them */
    uint32_t jot_tags;     /* the most tags one file's two records wear */
    uint64_t records;
    uint64_t bytes;
    uint64_t body;
    uint64_t runs;         /* the runs of fresh data the files' records name, */
    uint64_t fresh;        /* and their blocks: what a mount's check reads */
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

/* The records measured: the Undones of the mount's verdict while it is held,
 * every tag given an id since the last stitch settled, and the word owed when
 * the book does not hold it already (§7: the same text is written never
 * again), and the files' — ERR_NO_MEMORY for no room for how their runs
 * changed. */
static error_t StitchMeasure(StitchSnap* s, StitchLoad* l)
{
    memset(l, 0, sizeof(*l));
    l->bytes   = STITCH_HEAD_MOST + STITCH_CLASP_MOST + ErrataBytes();
    l->body    = STITCH_HEAD_MOST > STITCH_CLASP_MOST ? STITCH_HEAD_MOST : STITCH_CLASP_MOST;
    l->body    = ErrataBodyMost() > l->body ? ErrataBodyMost() : l->body;
    l->records = ErrataUndones();
    for (uint32_t id = s->coined_lo; id < s->coined_hi; id++) {
        TallyTag t;
        if (!StitchCoinedTag(id, &t))
            continue;
        uint64_t body = STITCH_COINED_MOST - STITCH_RECORD_HEAD_MOST + t.key_len + t.value_len;
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
        uint64_t body = STITCH_USE_MOST - STITCH_RECORD_HEAD_MOST + (s->use ? s->use->bytes : 0u);
        l->use     = s->use;
        l->records++;
        l->bytes  += STITCH_RECORD_HEAD_MOST + body;
        l->body    = body > l->body ? body : l->body;
    }
    uint64_t records = 0, bytes = 0, body = 0;
    error_t  rc      = DocketMeasure(&s->jots, &records, &bytes, &body, &l->jot_tags, &l->runs, &l->fresh);
    l->records += records;
    l->bytes   += bytes;
    l->body     = body > l->body ? body : l->body;
    return rc;
}

/*
 * ‼ THE FLUSH OF THE DATA IS PAID BY WHAT ITS CHECK WOULD COST (§3.3, M-13,
 * §16.5 п.1-3). A stitch that names fresh data and is begun before its flush
 * leaves the mount to read every block of it back: its runs, each a seek, and
 * its bytes at the pace the medium's reads keep — the worst S0 saw since the
 * last S0 (read_pace.h). Where that costs more than D — the last cycle that
 * settled, less what the flush of its own data took, so a flush paid never
 * spares the next one its own — the cycle pays the flush first, and the mount
 * believes the stitch; where it does not, the mount's check stays within a
 * cycle's cost however the medium seeks. With no pace known, and for the
 * halt's CLEAN stitch, the flush is paid: the last stitch of a machine
 * stopping is checked by nobody before the next mount. A stitch naming no
 * fresh data has nothing to flush ahead of it, nor anything to check. Taken
 * once at S0 and judged in each BUILD, so a retry (M-6) is judged by the same
 * measures.
 */
static bool StitchToll(const StitchSnap* s, uint64_t runs, uint64_t fresh)
{
    if (!fresh)
        return false;
#ifdef CONFIG_TAGFS_TOLL_SPARES
    /* A kernel built TOLL=spare (Makefile): every flush spared but the
     * halt's, said once, so a PowerCut chain's record holds a cut that kept
     * a stitch and lost data it names whatever pace its host keeps. */
    static bool said;
    if (!said && !(s->flags & STITCH_CLEAN)) {
        said = true;
        kprintf("[TagFS] TOLL=spare: a stitch that names fresh data is begun before the flush of that data, as "
                "every one but the halt's is on this kernel\n");
    }
    return (s->flags & STITCH_CLEAN) != 0;
#endif
    if ((s->flags & STITCH_CLEAN) || !s->pace.rate)
        return true;
    if (fresh > UINT64_MAX / ((uint64_t)TAGFS_BLOCK_SIZE * 1000000ull))
        return true;
    uint64_t read_us = fresh * TAGFS_BLOCK_SIZE * 1000000ull / s->pace.rate;
    if (read_us > s->d_us)
        return true;
    return s->pace.seek_us && runs > (s->d_us - read_us) / s->pace.seek_us;
}

/* The fresh blocks the stitch lays are not the ones its toll was judged by: a
 * defect of the measure, said; the flush of its data is paid all the same,
 * so the stitch's word for it — whatever it says — never outruns the medium. */
static void StitchFreshUnmeasured(StitchSnap* s, uint64_t measured)
{
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: stitch %llu lays %llu fresh block(s) and its toll was judged by %llu",
          (unsigned long long)s->seq, (unsigned long long)s->fresh, (unsigned long long)measured);
#endif
    kprintf("[TagFS] DEFECT: stitch %llu lays %llu fresh block(s) and its toll was judged by %llu — the flush of "
            "its data is paid before it all the same\n", (unsigned long long)s->seq,
            (unsigned long long)s->fresh, (unsigned long long)measured);
    s->toll = true;
}

/*
 * ‼ THE VERDICT GOES FIRST, AND WITH EVERY ATTEMPT (§2.8, §16.5 п.5). The
 * mount's reader takes an Undone only before every other record of its
 * stitch — one after a Coined would leave the stitch unapplied — so the
 * Undones of the mount's verdict follow the StitchHead straight away, one a
 * stitch undone, and each names the stitch it undoes by its distance from
 * this attempt's own seq: a retry (M-6) takes a new seq, and lays them again
 * reckoned from it.
 *
 * The stream laid into `out`: StitchHead, the records `l` measured, Clasp;
 * `starts` each record's offset, *nstarts how many — never more than the
 * records measured and the two around them, which `starts` holds. Its length
 * in *len, and the Clasp's sum in *clasp; ERR_NO_MEMORY for the room the
 * files' tags are put in order in. */
static error_t StitchStream(StitchSnap* s, const StitchLoad* l, uint8_t* out, uint8_t* body, uint32_t* starts,
                            uint32_t* nstarts, uint32_t* clasp, uint32_t* len)
{
    const RibbonCursor* c = RibbonCursorNow();
    bool                came = c->came.blocks && (c->off == 0 || c->refused);
    StitchHead          h = { .flags = s->flags, .prev = c->last ? s->seq - c->last : 0, .prev_clasp = c->clasp,
                              .tally = s->seq - c->tally, .made_ns = s->made_ns, .next_block = c->next.block,
                              .next_blocks = c->next.blocks, .from_block = came ? c->came.block : 0,
                              .from_blocks = came ? c->came.blocks : 0 };
    uint32_t n = 0, coined = 0, at = RibbonRecordPut(out, RIBBON_STITCH_HEAD, 0, body, StitchHeadBodyPut(body, &h));
    starts[n++] = 0;
    at = ErrataLay(out, at, s->seq, body, starts, &n);
    for (uint32_t id = s->coined_lo; id < s->coined_hi && coined < l->coined; id++) {
        TallyTag t;
        if (!StitchCoinedTag(id, &t))
            continue;
        starts[n++] = at;
        at += RibbonRecordPut(out + at, RIBBON_COINED, 0, body, RibbonCoinedBodyPut(body, id, &t));
        coined++;
    }
    if (l->use_written) {
        starts[n++] = at;
        at += RibbonRecordPut(out + at, RIBBON_USE_CONTEXT, 0, body,
                              TallyUsePut(body, l->use ? (const uint8_t*)l->use->text : NULL,
                                          l->use ? l->use->bytes : 0u));
    }
    s->fresh   = 0;
    error_t rc = DocketLay(&s->jots, s->seq, l->jot_tags, out, &at, body, starts, &n, &s->fresh);
    if (rc != OK)
        return rc;
    if (s->fresh != l->fresh)
        StitchFreshUnmeasured(s, l->fresh);
    Clasp clasped = { .records = n - 1u, .fresh = s->fresh, .crc = KCrc32c(out, at) };
    *clasp      = clasped.crc;
    s->clasp_at = at;
    starts[n++] = at;
    *nstarts    = n;
    *len        = at + RibbonRecordPut(out + at, RIBBON_CLASP, 0, body, ClaspBodyPut(body, &clasped));
    return OK;
}

static void StitchSectorsLetGo(StitchSnap* s)
{
    kfree(s->laid);
    s->laid    = NULL;
    s->sectors = NULL;
    s->of      = 0;
    s->written = 0;
}

/* The stitch held to the room TAKE counted it in flight: the claims of its
 * Inks and what every stitch carries hold every record it lays (P21) — the
 * room the next changes are granted is judged by that count, and a stitch
 * past it lays where none was kept for it. */
static void StitchHeldToFlight(const StitchSnap* s)
{
    if (s->of <= s->flight)
        return;
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: stitch %llu lays %u sector(s), past the %llu its TAKE counted in flight",
          (unsigned long long)s->seq, s->of, (unsigned long long)s->flight);
#endif
    kprintf("[TagFS] DEFECT: stitch %llu lays %u sector(s), past the %llu its TAKE counted in flight — the claims "
            "of its Inks fell short of what it carries\n", (unsigned long long)s->seq, s->of,
            (unsigned long long)s->flight);
}

/* The attempt whose writing began kept as laid where it lies — refused by the
 * medium, or cut short by the medium leaving — whatever of it landed being
 * this kernel's own to a return that finds it (ribbon_left.c); `past` when the
 * ribbon has gone on past it already (RibbonRetry). An attempt refused before
 * a sector of it was written is nothing on the medium. */
static void StitchTryKept(StitchSnap* s, bool past)
{
    if (!s->laid || s->step < STITCH_STEP_STITCH)
        return;
    s->laid->at   = s->at;
    s->laid->hop  = s->hop;
    s->laid->of   = s->of;
    s->laid->seq  = s->seq;
    s->laid->past = past;
    RibbonTryLaid(s->laid);
    s->laid = NULL;
    StitchSectorsLetGo(s);
}

/* The stitch measured, given its place and its seq, laid and sealed — the
 * heap's, never a frame's; *word whether it carries the person's word. */
static error_t StitchLay(StitchSnap* s, bool* word)
{
    StitchLoad l;
    StitchSectorsLetGo(s);
    error_t   measured = StitchMeasure(s, &l);
    *word = l.use_written;
    s->toll = StitchToll(s, l.runs, l.fresh);
    /* Judged again by each attempt with its toll, and kept in s->flags, so the
     * word its StitchHead carries (StitchStream) is the one handed on as it
     * settles (RibbonSewn: the stand's mirror of the cursor, sewn_flags; a
     * kernel without VOLUME_TESTS keeps none). The other readers of s->flags
     * ask STITCH_CLEAN alone (StitchToll, SettleSettledLocked). */
    s->flags = (s->flags & ~(uint64_t)STITCH_DATA_FLUSHED) | (s->toll || !l.fresh ? STITCH_DATA_FLUSHED : 0u);
    uint8_t*  stream = kmalloc(l.bytes);
    uint8_t*  body   = kmalloc(l.body);
    uint32_t* starts = kmalloc(sizeof(uint32_t) * (l.records + 2u));
    error_t   rc     = measured != OK ? measured : stream && body && starts ? OK : ERR_NO_MEMORY;
    uint32_t  len    = 0, nstarts = 0;
    if (rc == OK) {
        s->seq = RibbonSeqNext();
        rc     = StitchStream(s, &l, stream, body, starts, &nstarts, &s->clasp, &len);
    }
    if (rc == OK) {
        s->useful = len;
        s->of     = (uint32_t)RibbonStitchOf(len);
        StitchHeldToFlight(s);
        rc    = RibbonRoom(s->of, !l.use_written, &s->at, &s->hop);
    }
    if (rc == OK && !(s->laid = RibbonTryMake(s->of)))
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        s->sectors = s->laid->sectors;
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

/*
 * ‼ A WORD WITH NO ROOM IS LEFT OUT, NEVER WHAT WAS CLAIMED WITH IT (§3.6,
 * P21). The person's word is the one thing a stitch carries that no claim held
 * room for; refused room, it is the word that goes — the stitch is laid again
 * without it, with a new seq, and the files' changes and tags it carries land
 * — and once that stitch settles the word is answered as refused
 * (UseMemoryStitchRefused): the one who said it hears ERR_TAGFS_NO_SPACE, and
 * the volume keeps the word it holds. Refused with no word aboard, the cycle
 * is: ERR_TAGFS_NO_SPACE, said by the ribbon.
 */
static error_t StitchBuild(StitchSnap* s)
{
    bool    word = false;
    error_t rc   = StitchLay(s, &word);
    if (rc != ERR_TAGFS_NO_SPACE || !word)
        return rc;
    kprintf("[TagFS] the word the person said, %llu byte(s), finds no room on the ribbon: the stitch is laid "
            "without it, and the volume keeps the word it holds (ERR_TAGFS_NO_SPACE to the one who said it)\n",
            (unsigned long long)(s->use ? s->use->bytes : 0u));
    s->use_owed = false;
    s->use_out  = true;
    return StitchLay(s, &word);
}

// ----------------------------------------------------------------------------
// STITCH
// ----------------------------------------------------------------------------

/* The sector of a stitch its last atom of the ribbon begins at: a stitch ends
 * on one (RibbonStitchOf), and one begun inside a flash page, past an end a
 * writer of a smaller atom left, may be shorter than a page — all of it then. */
static uint64_t StitchLastAtom(const StitchSnap* s)
{
    uint32_t atom = RibbonCursorNow()->atom;
    return s->of > atom ? s->of - atom : 0;
}

/* The window of the stitch written next: from sector `written` on, as many as
 * the seat takes in one command, inside one Quire — and the last atom a write
 * of its own, so the cut that stops before it tears a stitch of more than one
 * atom (SETTLECUT `stitch`). Its first sector's address in *vlba. */
static uint32_t StitchWindow(const StitchSnap* s, uint64_t* vlba)
{
    uint64_t left  = s->at.quire.blocks * TAGFS_BLOCK_SECTORS - s->at.off;
    uint64_t last  = StitchLastAtom(s);
    uint64_t n     = s->of - s->written;
    uint64_t most  = BoardroomSeatRun(tagfs_get_seat());
    if (s->written < left) {
        *vlba = RibbonPlaceVlba(&s->at) + s->written;
        n     = n < left - s->written ? n : left - s->written;
    } else {
        *vlba = s->hop.block * TAGFS_BLOCK_SECTORS + (s->written - left);
    }
    if (s->written + n > last && s->written < last)
        n = last - s->written;
    return (uint32_t)(n < most ? n : most);
}

/* One window of the stitch to the medium: *done once its last sector is. */
static error_t StitchWrite(StitchSnap* s, bool* done)
{
    uint64_t vlba = 0;
    uint32_t n    = StitchWindow(s, &vlba);
    if (s->written == 0 && ErrataHeld())
        SETTLE_CUT_AT(SETTLE_CUT_POINT_UNDONE);
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
    if (s->written == 0 && EngrossCarriedBy(s->target))
        SETTLE_CUT_AT(SETTLE_CUT_POINT_ENGROSS);
#endif
    if (s->written == StitchLastAtom(s))
        SETTLE_CUT_AT(SETTLE_CUT_POINT_STITCH);
    int rc = tagfs_volume_write(vlba, n, s->sectors + (uint64_t)s->written * BOOK_SECTOR_BYTES);
    if (rc != 0)
        return rc == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY
             : rc == (int)ERR_TAGFS_READ_ONLY  ? ERR_TAGFS_READ_ONLY : ERR_WRITE_FAILED;
    s->written += n;
    *done = s->written == s->of;
    return OK;
}

/* No Quire takes the write (M-6): the book takes none from here, said with the
 * refusals in a row and `nowhere`, why no reader would take a retry up. */
static error_t StitchNowhere(const StitchSnap* s, const char* nowhere)
{
    const RibbonCursor* c = RibbonCursorNow();
    __atomic_store_n(&s_book_refused, 1u, __ATOMIC_RELEASE);
    kprintf("[TagFS] the book takes no write: %u refusal(s) in a row%s, the last of stitch %llu at %s — %s, so none "
            "is written: the volume may only be read from here (ERR_TAGFS_READ_ONLY)\n", c->refused - c->cut,
            c->cut ? " past an attempt its medium leaving cut short" : "", (unsigned long long)s->seq, s->stage,
            nowhere);
    SettleKnock();
    return ERR_TAGFS_READ_ONLY;
}

/* A refusal once the stitch's writing began (M-6): OK, and the cycle back at
 * BUILD with the ribbon gone on in the next_quire the last stitch sewn names;
 * or the answer the cycle ends with — the medium's own when it left or takes
 * no write, and ERR_TAGFS_READ_ONLY where no reader would take a retry up. */
static error_t StitchRefused(StitchSnap* s, error_t rc)
{
    TagFSWriteRefused();
    if (rc == ERR_NO_MEMORY || rc == ERR_DEVICE_NOT_READY || TagFSMediumLeft() || TagFSReadOnly())
        return rc;
    const char* nowhere = RibbonRetry(&s->at, &s->hop, s->of, false);
    StitchTryKept(s, nowhere == NULL);
    if (nowhere)
        return StitchNowhere(s, nowhere);
    if (RibbonCursorNow()->refused - RibbonCursorNow()->cut == 1u)
        kprintf("[TagFS] the medium refused stitch %llu at %s (error %u): the cycle goes on with a new seq in the "
                "next_quire, block %llu, at its sector %llu (M-6)\n", (unsigned long long)s->seq, s->stage,
                (unsigned)rc, (unsigned long long)RibbonCursorNow()->quire.block,
                (unsigned long long)RibbonCursorNow()->off);
    RibbonNextGiven();
    s->step = STITCH_STEP_BUILD;
    return OK;
}

#ifdef CONFIG_VOLUME_TESTS
/* The stand's departure in the middle of a stitch: the sectors written past
 * which the medium leaves, plus one; 0 unarmed. */
static volatile uint32_t s_stand_leaves;

void StitchStandLeaves(uint32_t sectors)
{
    __atomic_store_n(&s_stand_leaves, sectors == UINT32_MAX ? UINT32_MAX : sectors + 1u, __ATOMIC_RELEASE);
}

/* The medium leaves here, once, where the stand armed it to: before the next
 * window of the stitch once `sectors` of it are written, or before its last
 * flush once all of them are. */
static void StitchStandLeft(const StitchSnap* s)
{
    uint32_t armed = __atomic_load_n(&s_stand_leaves, __ATOMIC_ACQUIRE);
    uint32_t at    = armed - 1u < s->of ? armed - 1u : s->of;
    if (armed && s->written >= at && __atomic_exchange_n(&s_stand_leaves, 0u, __ATOMIC_ACQ_REL))
        TagFSStandMediumLeaves();
}
#define STITCH_STAND_LEFT(s) StitchStandLeft(s)
#else
#define STITCH_STAND_LEFT(s) ((void)(s))
#endif

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
        /* The next_quire first: the verdict's Labeleds claim room the cursor
         * judges past it (RibbonRoomOf), and a cursor the mount left with none
         * — gone on at the start of the next_quire named (RibbonGoesOnAt), its
         * own ask refused (RibbonNextGivenAtMount) — would see every claim but
         * a freeing one refused, the cycle with them, and the next cycle's the
         * same, were it given only past them. */
        RibbonNextGiven();
        rc = StitchMarked(s);
        if (rc != OK)
            break;
        StitchTake(s);
        s->step = STITCH_STEP_BUILD;
        break;
    case STITCH_STEP_BUILD:
        s->stage = "the stitch's making";
#ifdef CONFIG_VOLUME_TESTS
        if ((rc = SettleStandRefusalHere(OK)) != OK)
            break;
#endif
        rc       = StitchBuild(s);
        if (rc == OK)
            s->step = s->toll ? STITCH_STEP_FIRST_FLUSH : STITCH_STEP_STITCH;
        else if (rc == ERR_TAGFS_READ_ONLY)
            rc = StitchNowhere(s, "the attempts refused leave the next_quire the last stitch sewn names no room "
                                  "for the retry");
        break;
    case STITCH_STEP_FIRST_FLUSH: {
        uint64_t t0 = SettleNowUs();
        s->stage    = "the flush of the data the stitch names";
        rc           = VolumeFlushFor(s->data_need);
        s->flush_us += SettleNowUs() - t0;
        if (rc == OK)
            s->step = STITCH_STEP_STITCH;
        break;
    }
    case STITCH_STEP_STITCH:
        STITCH_STAND_LEFT(s);
        s->stage = "the stitch";
        rc       = StitchWrite(s, &last);
        if (rc == OK && last)
            s->step = STITCH_STEP_LAST_FLUSH;
        break;
    case STITCH_STEP_LAST_FLUSH:
        STITCH_STAND_LEFT(s);
        if (!s->toll && s->fresh)
            SETTLE_CUT_AT(SETTLE_CUT_POINT_SPARED);
        SETTLE_CUT_AT(SETTLE_CUT_POINT_FLUSH);
        s->stage = "the flush after the stitch";
        rc       = VolumeFlushFor(VolumeFlushNeed());
        if (rc == OK)
            s->step = STITCH_STEP_DONE;
        break;
    case STITCH_STEP_DONE:
        break;
    }
    if (rc != OK && s->step >= STITCH_STEP_FIRST_FLUSH)
        rc = StitchRefused(s, rc);
    *done = rc != OK || s->step == STITCH_STEP_DONE;
    return rc;
}

bool StitchHoldStill(uint64_t* written)
{
    for (;;) {
        uint64_t cover   = 0;
        error_t  refused = OK;
        uint64_t ticket  = ScribeAsk(&g_settle_scribe);
        switch (ScribeWait(&g_settle_scribe, ticket, &cover, &refused)) {
        case SCRIBE_MINE:
            *written = __atomic_load_n(&g_settle_scribe.written, __ATOMIC_ACQUIRE);
            return true;
        case SCRIBE_SELF:
            if (!SettleRunBetweenStepsHere())
                return false;
            while (SettleRunBetweenStepsHere())
                SettleRunStepHere();
            break;
        default:
            break;      /* a pass covered the ask: the hold is asked again */
        }
    }
}

void StitchStillLetGo(uint64_t written)
{
    ScribeGive(&g_settle_scribe, written, OK);
}

uint64_t StitchRunTarget(void)
{
    return s_stitch.target;
}

const char* StitchRunStage(void)
{
    return s_stitch.stage ? s_stitch.stage : "the stitch's snapshot";
}

/* What the settled stitch changed of the book's trees, into DogEar (dog_ear.h):
 * every file it carried a record of, the tags it coined, the Use Context when
 * it carried one — what the next Tally lays anew of the state it leaves. */
static void StitchDogEared(const StitchSnap* s)
{
    for (const Jot* j = s->jots.first; j; j = j->next[0])
        if (j->carried)
            DogEarFileChanged(j->fid, j->was, j->now, s->target);
    if (s->coined_hi > s->coined_lo)
        DogEarPut(TALLY_TAGS, (TallyKey){ s->coined_lo, 0 }, (TallyKey){ s->coined_hi, 0 });
    if (s->use_owed)
        DogEarPut(TALLY_USE, (TallyKey){ 0, 0 }, (TallyKey){ 1, 0 });
}

/* DONE: the ribbon's cursor past the stitch — the room it leaves published
 * while the flight still stands, so a claim is judged by too little room —
 * then, under the state lock, the target settled, the word the stitch carried
 * — or the one the book held already — the volume's, or the word it left out
 * refused, the freers it carried named by its seq, the Quires it left told to
 * Fallow and what ripened given back; then, with no lock, the tags it laid
 * down the book's, the files it carried given its seq, and a next_quire asked
 * ahead of need where it went on into one. Refused, the Docket it took is
 * given back under what came since, its claims with it. The flight ends
 * either way. */
void StitchRunEnd(error_t rc)
{
    StitchSnap* s     = &s_stitch;
    TagFSState* fs    = tagfs_get_state();
    UseText*    gone  = NULL;
    Jot*        spent = NULL;
    FallowNode* nodes = NULL;
    if (rc != OK) {
        TagFSLockTake(&fs->lock);
        DocketBackLocked(&s->jots, &spent);
        RibbonClaimBack(s->claimed);
        TagFSLockDrop(&fs->lock);
        JotsLetGo(spent);
        /* A volume that takes no write from here ends the Tally under way:
         * nothing of it is written past the refusal, its ground goes back. */
        if (TagFSReadOnly() || TagFSMediumLeft())
            BinderStopped(rc);
        StitchTryKept(s, false);
    } else {
        uint64_t left = RibbonSewn(&s->at, &s->hop, s->of, s->seq, s->clasp, s->clasp_at, s->flags);
        TagFSLockTake(&fs->lock);
        SettleSettledLocked(s->target, s->cycle, (s->flags & STITCH_CLEAN) != 0);
        if (s->use_owed) {
            gone   = UseMemoryStitchSettled(s->use, s->use_ticket);
            s->use = NULL;
        } else if (s->use_out) {
            UseMemoryStitchRefused(s->use_ticket);
        }
        FallowStitchSettled(s->target, s->seq, s->cycle);
        FallowQuireLeft(left);
        /* A slot's rest is let go in the hold its ground is given back in:
         * no measure between sees it twice, or the reserve without it. */
        bool     held = BinderHoldsGround();
        uint64_t ripe = held ? 0 : FallowRipen(0, s->cycle, &nodes);
        if (!held)
            BinderRestRipens(FallowSpineRipeFrom());
        SettleGivenLocked(ripe);
        TagFSLockDrop(&fs->lock);
        FallowNodesFree(nodes);
        GlossTrim();
        BirthsTrim();
        TagRegistryCoinedUpTo(fs->registry, s->coined_hi);
        uint64_t fid_most = 0;
        error_t  births   = DocketBirths(&s->jots, s->seq, &fid_most);
        StitchDogEared(s);
        CommonholdSettled(s->target);   /* past DogEar: the blocks the stitch's Inks left with no owner, gone */
        LooseLeavesSettled(&s->jots);
        BinderStitchSettled(s->seq, s->useful, fid_most, births);
        EndpaperGrew(&s->jots, s->seq);
        DocketSettled(&s->jots, s->seq);
        ErrataSettled(s->seq);
        s_settled_flush_us = s->flush_us;
        RibbonNextGiven();
    }
    /* Past the Coineds the stitch laid and the next_quire asked: the room a
     * claim is judged by is the cursor's own from here (ribbon.c). */
    RibbonFlightEnds();
    UseTextDrop(gone);
    UseTextDrop(s->use);
    s->use = NULL;
    StitchSectorsLetGo(s);
}
