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
#include "tagfs_volume.h"

/*
 * The Settle cycle (sewing.h): one stitch on the book's ribbon.
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
 *                room for refused and the one the book held forgotten — the
 *                freers it carried named by its seq,
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


void StitchMountReset(TagFSVolume* vol)
{
    __atomic_store_n(&vol->sewing.book_refused, 0u, __ATOMIC_RELEASE);
}

bool StitchBookRefused(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->sewing.book_refused, __ATOMIC_ACQUIRE) != 0;
}

bool StitchMarksVerdict(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->sewing.marking, __ATOMIC_ACQUIRE) == (uint32_t)amp_get_core_index() + 1u;
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
static void StitchCloseForRoom(TagFSVolume* vol, StitchSnap* s, bool awaits)
{
    if (awaits && s->claimed == 0 && s->jots.count == 0 && !s->use_owed && s->coined_lo == s->coined_hi &&
        !s->retires && RibbonCloseEarly(vol))
        RibbonNextGiven(vol);
}

/* The sectors the stitch TAKE took can take at the most: its claims — the
 * Labeleds of the mark the verdict's files bear among them — the Undones
 * of the mount's verdict, the Coineds of the tags given ids since the last
 * stitch settled — `coined` of them, `text` bytes of keys and values, every
 * one in its range — the word owed, and what every stitch carries. */
static uint64_t StitchFlight(TagFSVolume* vol, const StitchSnap* s, uint32_t coined, uint64_t text)
{
    uint64_t bytes = STITCH_HEAD_MOST + STITCH_CLASP_MOST + s->claimed + ErrataBytes(vol) +
                     (uint64_t)coined * STITCH_COINED_MOST + text;
    if (s->use_owed)
        bytes += STITCH_USE_MOST + (s->use ? s->use->bytes : 0u);
    return RibbonStitchOf(vol, bytes);
}

static void StitchTake(TagFSVolume* vol, StitchSnap* s)
{
    TagFSState* fs = &vol->state;
    uint32_t    coined = 0;
    uint64_t    text   = 0;
#ifdef CONFIG_VOLUME_TESTS
    SettleStandRingsHere(vol, true);
#endif
    SettleWaitersRunBegins(vol);
    uint32_t demands = CadenceRunBegins(vol);
    bool     room    = s->why == SETTLE_FOR_ROOM || (demands & SETTLE_DEMAND_ROOM);
    TagFSLockTake(&fs->lock);
    s->target    = InkedGen(vol);
    s->data_need = InkDataNeed(vol);
    s->cycle     = SettledCycles(vol) + 1u;
    s->flags     = (s->why == SETTLE_FOR_HALT && HelmOwed(vol) == 0 ? STITCH_CLEAN : 0u) |
                   (RibbonCursorLied(vol) ? STITCH_MEDIUM_LIED : 0u);
    s->use_owed  = UseMemoryStitchTake(vol, &s->use, &s->use_ticket);
    s->claimed   = RibbonClaimTaken(vol);
    bool awaits  = room && FallowAwaitsQuire(vol);
    DocketTakeLocked(vol, &s->jots);
    /* After the target: every tag a record up to it wears was given before
     * its Ink, so it is below the range's end; and what the registry holds
     * after the range holds every Coined in it. The registry's lock is a
     * leaf, taken under the state lock (lock_rank.h). */
    TagRegistryCoinedRange(fs->registry, &s->coined_lo, &s->coined_hi);
    uint32_t retiring = 0, retired = 0;
    s->retires = TagRegistryRetiringNext(fs->registry, &retiring, s->coined_lo, s->target, &retired);
    TagRegistryCoinedHeld(fs->registry, &coined, &text);
    s->flight = StitchFlight(vol, s, coined, text);
    RibbonFlightBegins(vol, s->flight);
    TagFSLockDrop(&fs->lock);
    StitchCloseForRoom(vol, s, awaits);
    FallowGiveRoomHold(vol, s->cycle);
    /* The toll's D is the last settled cycle's less the flush of its own data
     * (§16.5 п.1), so a flush paid never spares the next cycle its own; the
     * cadence's D — the Lull, the medium's share, the small writer's measure —
     * is the whole of that cycle's time on the medium, the flush with it. */
    uint64_t d = SettleLastDUs(vol);
    s->d_us    = d > vol->sewing.settled_flush_us ? d - vol->sewing.settled_flush_us : 0;
    ReadPaceTake(&s->pace);
    s->made_ns = rtc_get_unix64() * 1000000000ull;
    SETTLE_CUT_CYCLE(s->target);
#ifdef CONFIG_VOLUME_TESTS
    SettleStandRingsHere(vol, false);
#endif
}

/*
 * ‼ THE MARK GOES DOWN WITH THE VERDICT (§16.5 п.6, M-10). Each file the
 * mount's verdict rolls back is marked so in its record (TAGFS_FILE_UNDONE) by
 * an ordinary Ink before the snapshot of the cycle that lays the verdict down —
 * so the one stitch carries both, and no mount finds the Undone without the
 * mark of a file that lives. Every cycle until one settles carries the verdict (a
 * stitch that settled without it would leave the next mount to replay what
 * it undoes, H-1), so it is the first cycle that begins — asked at the door's
 * opening (tagfs.c, ErrataAsk) — that marks them, and a cycle refused before
 * the last of them marks the rest in the next. Off the heap's reserve, which
 * is the steps' own: a refusal of the moment — the heap (SETTLE_PARK_HEAP), the
 * ribbon's room (tried a window on), a volume taking no change now — refuses
 * the cycle, never laying the verdict without them (errata.h, ErrataMarked). A
 * stopping machine's halt refuses every change not begun; these are its own
 * cycle's, made before its snapshot, and pass (StitchMarksVerdict).
 */
static error_t StitchMarked(TagFSVolume* vol, StitchSnap* s)
{
    if (!ErrataHeld(vol))
        return OK;
    KHeapScope was = KHeapScopeEnter(KHEAP_CHARGE_KERNEL, 0);
    __atomic_store_n(&vol->sewing.marking, (uint32_t)amp_get_core_index() + 1u, __ATOMIC_RELEASE);
    error_t    rc  = ErrataMarked(vol);
    __atomic_store_n(&vol->sewing.marking, 0u, __ATOMIC_RELEASE);
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
    UseText* use;          /* the word written, when it differs from what the book holds; NULL: none */
    bool     use_written;
    uint32_t coined;       /* the Coineds of them */
    uint32_t jot_tags;     /* the most tags one file's two records wear */
    uint64_t records;
    uint64_t bytes;
    uint64_t body;
    uint64_t runs;         /* the runs of fresh data the files' records name, */
    uint64_t fresh;        /* and their blocks: what a mount's check reads */
} StitchLoad;

/*
 * The Coineds a stitch lays, one after another: every id of its range as it
 * is — a tag retired by an Ink up to the target as RETIRED — and then, of the
 * ids the book has already, every one that was retired by such an Ink
 * (TagRegistryRetiringNext). An id the registry gave no tag lies in no
 * Coined. The same walk measures the stitch and lays it, so the two cannot
 * differ.
 */
typedef struct {
    uint32_t id;       /* the next id of the range */
    uint32_t at;       /* the next place of the retirements the registry owes */
} StitchCoinedWalk;

static bool StitchCoinedNext(TagFSVolume* vol, const StitchSnap* s, StitchCoinedWalk* walk, uint32_t* id, TallyTag* t)
{
    TagRegistry* reg = vol->state.registry;
    TagRolled    rolled;
    if (walk->id == 0)
        walk->id = s->coined_lo;
    while (walk->id < s->coined_hi) {
        uint32_t next = walk->id++;
        if (TagRoll(reg, next, next + 1u, &rolled)) {
            *id = next;
            *t  = TagRolledBookForm(&rolled, s->target);
            return true;
        }
    }
    if (!TagRegistryRetiringNext(reg, &walk->at, s->coined_lo, s->target, id))
        return false;
    *t = (TallyTag){ .flags = TALLY_TAG_RETIRED };
    return true;
}

/* The records measured: the Undones of the mount's verdict while it is held,
 * every tag given an id since the last stitch settled, and the word owed — or
 * none, the book's forgotten — when the book does not hold it already (§7: the
 * same text is written never again), and the files' — ERR_NO_MEMORY for no
 * room for how their runs changed. */
static error_t StitchMeasure(TagFSVolume* vol, StitchSnap* s, StitchLoad* l)
{
    memset(l, 0, sizeof(*l));
    l->bytes   = STITCH_HEAD_MOST + STITCH_CLASP_MOST + ErrataBytes(vol);
    l->body    = STITCH_HEAD_MOST > STITCH_CLASP_MOST ? STITCH_HEAD_MOST : STITCH_CLASP_MOST;
    l->body    = ErrataBodyMost(vol) > l->body ? ErrataBodyMost(vol) : l->body;
    l->records = ErrataUndones(vol);
    StitchCoinedWalk walk = { 0 };
    uint32_t         id;
    TallyTag         t;
    while (StitchCoinedNext(vol, s, &walk, &id, &t)) {
        uint64_t body = STITCH_COINED_MOST - STITCH_RECORD_HEAD_MOST + TallyTagCarried(&t);
        l->coined++;
        l->records++;
        l->bytes += STITCH_RECORD_HEAD_MOST + body;
        l->body   = body > l->body ? body : l->body;
    }
    UseText* word = s->use_owed ? s->use : NULL;
    if (s->use_owed || s->use_forgot) {
        UseText* memory = tagfs_recall_use_context(vol, NULL);
        l->use_written  = !UseTextSame(memory, word);
        UseTextDrop(memory);
    }
    if (l->use_written) {
        uint64_t body = STITCH_USE_MOST - STITCH_RECORD_HEAD_MOST + (word ? word->bytes : 0u);
        l->use     = word;
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
static error_t StitchStream(TagFSVolume* vol, StitchSnap* s, const StitchLoad* l, uint8_t* out, uint8_t* body, uint32_t* starts,
                            uint32_t* nstarts, uint32_t* clasp, uint32_t* len)
{
    const RibbonCursor* c = RibbonCursorNow(vol);
    bool                came = c->came.blocks && (c->off == 0 || c->refused);
    StitchHead          h = { .flags = s->flags, .prev = c->last ? s->seq - c->last : 0, .prev_clasp = c->clasp,
                              .tally = s->seq - c->tally, .made_ns = s->made_ns, .next_block = c->next.block,
                              .next_blocks = c->next.blocks, .from_block = came ? c->came.block : 0,
                              .from_blocks = came ? c->came.blocks : 0 };
    uint32_t n = 0, coined = 0, at = RibbonRecordPut(out, RIBBON_STITCH_HEAD, 0, body, StitchHeadBodyPut(body, &h));
    starts[n++] = 0;
    at = ErrataLay(vol, out, at, s->seq, body, starts, &n);
    StitchCoinedWalk walk = { 0 };
    uint32_t         id;
    TallyTag         t;
    while (coined < l->coined && StitchCoinedNext(vol, s, &walk, &id, &t)) {
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
    error_t rc = DocketLay(vol, &s->jots, s->seq, l->jot_tags, out, &at, body, starts, &n, &s->fresh);
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
static void StitchTryKept(TagFSVolume* vol, StitchSnap* s, bool past)
{
    if (!s->laid || s->step < STITCH_STEP_STITCH)
        return;
    s->laid->at   = s->at;
    s->laid->hop  = s->hop;
    s->laid->of   = s->of;
    s->laid->seq  = s->seq;
    s->laid->past = past;
    RibbonTryLaid(vol, s->laid);
    s->laid = NULL;
    StitchSectorsLetGo(s);
}

/* The stitch measured, given its place and its seq, laid and sealed — the
 * heap's, never a frame's; *word whether it carries a UseContext. A word of
 * text is the one thing no claim held room for; an empty one — the word none,
 * or the book's forgotten — gives up what the book held, as a delete does, and
 * takes the room what frees takes (RibbonRoom's `promised`). */
static error_t StitchLay(TagFSVolume* vol, StitchSnap* s, bool* word)
{
    StitchLoad l;
    StitchSectorsLetGo(s);
    error_t   measured = StitchMeasure(vol, s, &l);
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
        s->seq = RibbonSeqNext(vol);
        rc     = StitchStream(vol, s, &l, stream, body, starts, &nstarts, &s->clasp, &len);
    }
    if (rc == OK) {
        s->useful = len;
        s->of     = (uint32_t)RibbonStitchOf(vol, len);
        StitchHeldToFlight(s);
        rc    = RibbonRoom(vol, s->of, !l.use_written || !l.use || !l.use->bytes, &s->at, &s->hop);
    }
    if (rc == OK && !(s->laid = RibbonTryMake(s->of)))
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        s->sectors = s->laid->sectors;
        uint64_t left = s->at.quire.blocks * TAGFS_BLOCK_SECTORS - s->at.off;
        RibbonStitchSeal(s->sectors, stream, len, starts, nstarts, s->seq, RibbonPlaceVlba(&s->at), left,
                         s->hop.block * TAGFS_BLOCK_SECTORS, s->of, vol->state.uuid);
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
 * P21) — AND THE WORD THE BOOK HELD IS FORGOTTEN WITH IT (the owner's rule,
 * 09-08: better to forget than to remember a word the person no longer says).
 * The person's word is the one thing a stitch carries that no claim held room
 * for; refused room, it is the word that goes — the stitch is laid again
 * without it, with a new seq, an empty UseContext in its place, and the files'
 * changes and tags it carries land — and once that stitch settles the word is
 * answered as refused and the book holds none (UseMemoryStitchRefused): the
 * one who said it hears ERR_TAGFS_NO_SPACE, and no mount after brings the old
 * word back. An empty UseContext gives up what the book held, as a delete
 * does, and takes the room kept for what frees; only where not even it fits
 * — a stitch an atom longer for it, the rest of the Quire spent to the atom
 * by what was claimed — is the stitch laid without either, the old word kept
 * on the medium and in memory alike, said. Refused with no word aboard, the
 * cycle is: ERR_TAGFS_NO_SPACE, said by the ribbon.
 */
static error_t StitchBuild(TagFSVolume* vol, StitchSnap* s)
{
    bool    word = false;
    error_t rc   = StitchLay(vol, s, &word);
    if (rc != ERR_TAGFS_NO_SPACE || !word)
        return rc;
    uint64_t bytes = s->use ? s->use->bytes : 0u;
    if (s->use_owed && bytes) {
        kprintf("[TagFS] the word the person said, %llu byte(s), finds no room on the ribbon: the stitch is laid "
                "without it, and the volume forgets the word it held rather than remember one the person no "
                "longer says (ERR_TAGFS_NO_SPACE to the one who said it)\n", (unsigned long long)bytes);
        s->use_owed   = false;
        s->use_out    = true;
        s->use_forgot = true;
        rc = StitchLay(vol, s, &word);
        if (rc != ERR_TAGFS_NO_SPACE || !word)
            return rc;
    }
    kprintf("[TagFS] the word the person said, %llu byte(s), finds no room on the ribbon, nor does the volume's "
            "forgetting the word it holds: the stitch is laid without either, and the volume keeps that word "
            "(ERR_TAGFS_NO_SPACE to the one who said it)\n", (unsigned long long)bytes);
    s->use_owed   = false;
    s->use_out    = true;
    s->use_forgot = false;
    return StitchLay(vol, s, &word);
}

// ----------------------------------------------------------------------------
// STITCH
// ----------------------------------------------------------------------------

/* The sector of a stitch its last atom of the ribbon begins at: a stitch ends
 * on one (RibbonStitchOf), and one begun inside a flash page, past an end a
 * writer of a smaller atom left, may be shorter than a page — all of it then. */
static uint64_t StitchLastAtom(TagFSVolume* vol, const StitchSnap* s)
{
    uint32_t atom = RibbonCursorNow(vol)->atom;
    return s->of > atom ? s->of - atom : 0;
}

/* The window of the stitch written next: from sector `written` on, as many as
 * the seat takes in one command, inside one Quire — and the last atom a write
 * of its own, so the cut that stops before it tears a stitch of more than one
 * atom (SETTLECUT `stitch`). Its first sector's address in *vlba. */
static uint32_t StitchWindow(TagFSVolume* vol, const StitchSnap* s, uint64_t* vlba)
{
    uint64_t left  = s->at.quire.blocks * TAGFS_BLOCK_SECTORS - s->at.off;
    uint64_t last  = StitchLastAtom(vol, s);
    uint64_t n     = s->of - s->written;
    uint64_t most  = BoardroomSeatRun(tagfs_get_seat(vol));
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
static error_t StitchWrite(TagFSVolume* vol, StitchSnap* s, bool* done)
{
    uint64_t vlba = 0;
    uint32_t n    = StitchWindow(vol, s, &vlba);
    if (s->written == 0 && ErrataHeld(vol))
        SETTLE_CUT_AT(SETTLE_CUT_POINT_UNDONE);
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
    if (s->written == 0 && EngrossCarriedBy(s->target))
        SETTLE_CUT_AT(SETTLE_CUT_POINT_ENGROSS);
#endif
    if (s->written == StitchLastAtom(vol, s))
        SETTLE_CUT_AT(SETTLE_CUT_POINT_STITCH);
    int rc = tagfs_volume_write(vol, vlba, n, s->sectors + (uint64_t)s->written * BOOK_SECTOR_BYTES);
    if (rc != 0)
        return rc == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY
             : rc == (int)ERR_TAGFS_READ_ONLY  ? ERR_TAGFS_READ_ONLY : ERR_WRITE_FAILED;
    s->written += n;
    *done = s->written == s->of;
    return OK;
}

/* No Quire takes the write (M-6): the book takes none from here, said with the
 * refusals in a row and `nowhere`, why no reader would take a retry up. */
static error_t StitchNowhere(TagFSVolume* vol, const StitchSnap* s, const char* nowhere)
{
    const RibbonCursor* c = RibbonCursorNow(vol);
    __atomic_store_n(&vol->sewing.book_refused, 1u, __ATOMIC_RELEASE);
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
static error_t StitchRefused(TagFSVolume* vol, StitchSnap* s, error_t rc)
{
    TagFSWriteRefused(vol);
    if (rc == ERR_NO_MEMORY || rc == ERR_DEVICE_NOT_READY || TagFSMediumLeft(vol) || TagFSReadOnly(vol))
        return rc;
    const char* nowhere = RibbonRetry(vol, &s->at, &s->hop, s->of, false);
    StitchTryKept(vol, s, nowhere == NULL);
    if (nowhere)
        return StitchNowhere(vol, s, nowhere);
    if (RibbonCursorNow(vol)->refused - RibbonCursorNow(vol)->cut == 1u)
        kprintf("[TagFS] the medium refused stitch %llu at %s (error %u): the cycle goes on with a new seq in the "
                "next_quire, block %llu, at its sector %llu (M-6)\n", (unsigned long long)s->seq, s->stage,
                (unsigned)rc, (unsigned long long)RibbonCursorNow(vol)->quire.block,
                (unsigned long long)RibbonCursorNow(vol)->off);
    RibbonNextGiven(vol);
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
static void StitchStandLeft(TagFSVolume* vol, const StitchSnap* s)
{
    uint32_t armed = __atomic_load_n(&s_stand_leaves, __ATOMIC_ACQUIRE);
    uint32_t at    = armed - 1u < s->of ? armed - 1u : s->of;
    if (armed && s->written >= at && __atomic_exchange_n(&s_stand_leaves, 0u, __ATOMIC_ACQ_REL))
        TagFSStandMediumLeaves(vol);
}
#define STITCH_STAND_LEFT(vol, s) StitchStandLeft(vol, s)
#else
#define STITCH_STAND_LEFT(vol, s) ((void)(vol), (void)(s))
#endif

// ----------------------------------------------------------------------------
// The cycle
// ----------------------------------------------------------------------------

void StitchRunBegin(TagFSVolume* vol, SettleWhy why)
{
    StitchSectorsLetGo(&vol->sewing.stitch);
    UseTextDrop(vol->sewing.stitch.use);
    memset(&vol->sewing.stitch, 0, sizeof(vol->sewing.stitch));
    vol->sewing.stitch.why  = why;
    vol->sewing.stitch.step = STITCH_STEP_TAKE;
}

error_t StitchRunStep(TagFSVolume* vol, bool* done)
{
    StitchSnap* s    = &vol->sewing.stitch;
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
        RibbonNextGiven(vol);
        rc = StitchMarked(vol, s);
        if (rc != OK)
            break;
        StitchTake(vol, s);
        s->step = STITCH_STEP_BUILD;
        break;
    case STITCH_STEP_BUILD:
        s->stage = "the stitch's making";
#ifdef CONFIG_VOLUME_TESTS
        if ((rc = SettleStandRefusalHere(OK)) != OK)
            break;
#endif
        rc       = StitchBuild(vol, s);
        if (rc == OK)
            s->step = s->toll ? STITCH_STEP_FIRST_FLUSH : STITCH_STEP_STITCH;
        else if (rc == ERR_TAGFS_READ_ONLY)
            rc = StitchNowhere(vol, s, "the attempts refused leave the next_quire the last stitch sewn names no room "
                                  "for the retry");
        break;
    case STITCH_STEP_FIRST_FLUSH: {
        uint64_t t0 = SettleNowUs();
        s->stage    = "the flush of the data the stitch names";
        rc           = VolumeFlushFor(vol, s->data_need);
        s->flush_us += SettleNowUs() - t0;
        if (rc == OK)
            s->step = STITCH_STEP_STITCH;
        break;
    }
    case STITCH_STEP_STITCH:
        STITCH_STAND_LEFT(vol, s);
        s->stage = "the stitch";
        rc       = StitchWrite(vol, s, &last);
        if (rc == OK && last)
            s->step = STITCH_STEP_LAST_FLUSH;
        break;
    case STITCH_STEP_LAST_FLUSH:
        STITCH_STAND_LEFT(vol, s);
        if (!s->toll && s->fresh)
            SETTLE_CUT_AT(SETTLE_CUT_POINT_SPARED);
        SETTLE_CUT_AT(SETTLE_CUT_POINT_FLUSH);
        s->stage = "the flush after the stitch";
        rc       = VolumeFlushFor(vol, VolumeFlushNeed(vol));
        if (rc == OK)
            s->step = STITCH_STEP_DONE;
        break;
    case STITCH_STEP_DONE:
        break;
    }
    if (rc != OK && s->step >= STITCH_STEP_FIRST_FLUSH)
        rc = StitchRefused(vol, s, rc);
    *done = rc != OK || s->step == STITCH_STEP_DONE;
    return rc;
}

bool StitchHoldStill(TagFSVolume* vol, uint64_t* written)
{
    for (;;) {
        uint64_t cover   = 0;
        error_t  refused = OK;
        uint64_t ticket  = ScribeAsk(&vol->settle.scribe);
        switch (ScribeWait(&vol->settle.scribe, ticket, &cover, &refused)) {
        case SCRIBE_MINE:
            *written = __atomic_load_n(&vol->settle.scribe.written, __ATOMIC_ACQUIRE);
            return true;
        case SCRIBE_SELF:
            if (!SettleRunBetweenStepsHere(vol))
                return false;
            while (SettleRunBetweenStepsHere(vol))
                SettleRunStepHere(vol);
            break;
        default:
            break;      /* a pass covered the ask: the hold is asked again */
        }
    }
}

void StitchStillLetGo(TagFSVolume* vol, uint64_t written)
{
    ScribeGive(&vol->settle.scribe, written, OK);
}

uint64_t StitchRunTarget(TagFSVolume* vol)
{
    return vol->sewing.stitch.target;
}

const char* StitchRunStage(TagFSVolume* vol)
{
    return vol->sewing.stitch.stage ? vol->sewing.stitch.stage : "the stitch's snapshot";
}

/* The tags the settled stitch retired of those the book had, into DogEar —
 * before the registry lets them go from what it owes. */
static void StitchRetiredDogEared(TagFSVolume* vol, const StitchSnap* s)
{
    TagRegistry* reg = vol->state.registry;
    uint32_t     at  = 0, id;
    while (s->retires && TagRegistryRetiringNext(reg, &at, s->coined_lo, s->target, &id))
        DogEarPut(vol, TALLY_TAGS, (TallyKey){ id, 0 }, (TallyKey){ id + 1u, 0 });
}

/* What the settled stitch changed of the book's trees, into DogEar (dog_ear.h):
 * every file it carried a record of, the tags it coined and retired, the Use
 * Context when it carried one or forgot the book's — what the next Tally lays
 * anew of the state it leaves. */
static void StitchDogEared(TagFSVolume* vol, const StitchSnap* s)
{
    for (const Jot* j = s->jots.first; j; j = j->next[0])
        if (j->carried)
            DogEarFileChanged(vol, j->fid, j->was, j->now, s->target);
    if (s->coined_hi > s->coined_lo)
        DogEarPut(vol, TALLY_TAGS, (TallyKey){ s->coined_lo, 0 }, (TallyKey){ s->coined_hi, 0 });
    if (s->use_owed || s->use_forgot)
        DogEarPut(vol, TALLY_USE, (TallyKey){ 0, 0 }, (TallyKey){ 1, 0 });
}

/* DONE: the ribbon's cursor past the stitch — the room it leaves published
 * while the flight still stands, so a claim is judged by too little room —
 * then, under the state lock, the target settled, the word the stitch carried
 * — or the one the book held already — the volume's, or the word it left out
 * refused and the book's forgotten where it said so, the freers it carried
 * named by its seq, the Quires it left told to
 * Fallow and what ripened given back; then, with no lock, the tags it laid
 * down the book's, the files it carried given its seq, and a next_quire asked
 * ahead of need where it went on into one. Refused, the Docket it took is
 * given back under what came since, its claims with it. The flight ends
 * either way. */
void StitchRunEnd(TagFSVolume* vol, error_t rc)
{
    StitchSnap* s     = &vol->sewing.stitch;
    TagFSState* fs    = &vol->state;
    UseText*    gone  = NULL;
    Jot*        spent = NULL;
    FallowNode* nodes = NULL;
    if (rc != OK) {
        TagFSLockTake(&fs->lock);
        DocketBackLocked(vol, &s->jots, &spent);
        RibbonClaimBack(vol, s->claimed);
        TagFSLockDrop(&fs->lock);
        JotsLetGo(spent);
        /* A volume that takes no write from here ends the Tally under way:
         * nothing of it is written past the refusal, its ground goes back. */
        if (TagFSReadOnly(vol) || TagFSMediumLeft(vol))
            BinderStopped(vol, rc);
        StitchTryKept(vol, s, false);
    } else {
        uint64_t left = RibbonSewn(vol, &s->at, &s->hop, s->of, s->seq, s->clasp, s->clasp_at, s->flags);
        TagFSLockTake(&fs->lock);
        SettleSettledLocked(vol, s->target, s->cycle, (s->flags & STITCH_CLEAN) != 0);
        if (s->use_owed) {
            gone   = UseMemoryStitchSettled(vol, s->use, s->use_ticket);
            s->use = NULL;
        } else if (s->use_out) {
            gone = UseMemoryStitchRefused(vol, s->use_ticket, s->use_forgot);
        }
        FallowStitchSettled(vol, s->target, s->seq, s->cycle);
        FallowQuireLeft(vol, left);
        /* A slot's rest is let go in the hold its ground is given back in:
         * no measure between sees it twice, or the reserve without it. */
        bool     held = BinderHoldsGround(vol);
        uint64_t ripe = held ? 0 : FallowRipen(vol, s->cycle, &nodes);
        if (!held)
            BinderRestRipens(vol, FallowSpineRipeFrom(vol));
        SettleGivenLocked(vol, ripe);
        TagFSLockDrop(&fs->lock);
        FallowNodesFree(nodes);
        GlossTrim(vol);
        BirthsTrim(vol);
        StitchRetiredDogEared(vol, s);
        TagRegistryCoinedUpTo(fs->registry, s->coined_hi, s->target);
        uint64_t fid_most = 0;
        error_t  births   = DocketBirths(vol, &s->jots, s->seq, &fid_most);
        StitchDogEared(vol, s);
        CommonholdSettled(vol, s->target);   /* past DogEar: the blocks the stitch's Inks left with no owner, gone */
        LooseLeavesSettled(vol, &s->jots);
        BinderStitchSettled(vol, s->seq, s->useful, fid_most, births);
        EndpaperGrew(vol, &s->jots, s->seq);
        DocketSettled(&s->jots, s->seq);
        ErrataSettled(vol, s->seq);
        vol->sewing.settled_flush_us = s->flush_us;
        RibbonNextGiven(vol);
    }
    /* Past the Coineds the stitch laid and the next_quire asked: the room a
     * claim is judged by is the cursor's own from here (ribbon.c). */
    RibbonFlightEnds(vol);
    UseTextDrop(gone);
    UseTextDrop(s->use);
    s->use = NULL;
    StitchSectorsLetGo(s);
}
