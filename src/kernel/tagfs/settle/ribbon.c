#include "sewing.h"
#include "binder.h"
#include "../mount/book_mount.h"
#include "../mount/book_unit.h"
#include "../block_map/block_map_derived.h"
#include "../ink/fallow.h"
#include "../tag_registry/tag_registry.h"

/*
 * Where the ribbon goes on (sewing.h): the writer's cursor past the last
 * stitch sewn on, its next_quire given ahead of need, a refused write taken on
 * in the next_quire the mount's reader walks, and the rest of a Quire with no
 * next_quire kept.
 *
 * The cursor is laid by the mount, with the door shut, where the walk of the
 * ribbon ended (RibbonCursorMount), given a next_quire there where it wants one
 * and the map has one (RibbonNextGivenAtMount), and moved by the Settle's
 * holder alone from the door's opening — a next_quire given, a stitch settled,
 * a write refused. Nothing else changes it, so it needs no lock: the stand
 * reads it between two cycles, on the runner's core.
 */

static RibbonCursor s_cursor;

/*
 * What a change of a book may claim of the ribbon (RibbonClaim), published by
 * the Settle's holder each time the cursor or the stitch in flight moves, and
 * by the mount, and read under the state lock by Ink: the bytes of records the
 * next stitch can carry past the one in flight, and of a stitch of freeing
 * records alone; whether the ribbon may still be given a next_quire, and the
 * free count when the last ask for one was refused. A move that shrinks the
 * room publishes it before what it took is let go, so a claim is judged
 * against too little room, never too much.
 */
static volatile uint64_t s_room_any;
static volatile uint64_t s_room_frees;
static volatile uint8_t  s_room_grows;
static volatile uint64_t s_refused_free;

/* The ribbon stands with no next_quire, its last ask refused past the reserve
 * — or answered with one found only on ground a Tally a mount may stand on
 * keeps from it (fallow.c, NodeBarred): published with the room. A change
 * short of room on the ribbon waited on a Tally for it, and no change that
 * does not free has been given room since (RibbonClaimSay, RibbonRoomFor):
 * the ribbon's shortfall is the volume's, not a change's, so any change given
 * room past it ends it. A change refused room on the ribbon, said until one
 * is given room (RibbonRefusedSaid). Any core, no lock; let go at mount. */
static volatile uint8_t  s_room_refused;
static volatile uint8_t  s_room_barred;
static volatile uint8_t  s_room_waited;
static volatile uint8_t  s_refused_said;

/* The slice of the last Tally's DONE that asked the next_quire again, and how
 * many in a row took that same stitch in (RibbonNextAgain). By the Settle's
 * holder. */
static uint64_t          s_again_slice;
static uint32_t          s_again_turns;

/* The bytes claimed by changes no stitch has taken, and those held for changes
 * in the making — a tag about to be given an id, a write's record while its
 * data moves (RibbonReserve): under the state lock. */
static uint64_t          s_claimed;
static uint64_t          s_reserved;

/* The sectors the stitch in flight takes at the most; 0 with none. Set under
 * the state lock at TAKE, cleared by the Settle's holder at the cycle's end. */
static volatile uint64_t s_flight;

/* A stitch with no word aboard that the rest and the next_quire do not hold,
 * said once a mount. */
static volatile uint8_t  s_long_said;

/* A stitch that ends inside a flash page, said once a mount (RibbonPlaced);
 * the last Quire begun off a page gone on into, said once, its block + 1
 * (RibbonEnteredSaid). */
static volatile uint8_t  s_inside_said;
static volatile uint64_t s_entered_said;

/* The medium's atom, sectors (§1.5): no write of the ribbon is smaller. */
static uint32_t RibbonMediumAtom(void)
{
    return g_book.atom / BOOK_SECTOR_BYTES;
}

/* Whether `q` is laid on atoms of `atom` sectors: its first block and its
 * length whole atoms. A Quire a writer of a smaller atom took may not be. */
static bool RibbonQuireOn(const RibbonQuire* q, uint32_t atom)
{
    uint64_t blocks = atom / TAGFS_BLOCK_SECTORS ? atom / TAGFS_BLOCK_SECTORS : 1u;
    return q->blocks && !(q->block % blocks) && !(q->blocks % blocks);
}

/* `sectors` of a stitch as the bytes of records it carries besides its
 * StitchHead and its Clasp. */
static uint64_t RibbonRoomBytes(uint64_t sectors)
{
    uint64_t bytes = sectors * BOOK_PAYLOAD_BYTES, fixed = STITCH_HEAD_MOST + STITCH_CLASP_MOST;
    return bytes > fixed ? bytes - fixed : 0;
}

/*
 * ‼ THE ROOM A CLAIM IS JUDGED BY LIES PAST THE STITCH IN FLIGHT (§16.2 п.8,
 * P21). A change claimed while a stitch is in flight rides the next one, which
 * begins where the one in flight ends — and where that one may run on into its
 * next_quire, in a Quire no next_quire is given for yet, where nothing but
 * what frees may go (RibbonRoom). So the room is the cursor's as it will stand
 * past `flight` sectors: with a next_quire, the rest and the next_quire less
 * the two atoms a stitch that runs on into it leaves; with none, a stitch of
 * freeing records only, an atom kept past it.
 */
void RibbonRoomOf(const RibbonCursor* c, uint64_t flight, uint64_t* any, uint64_t* frees)
{
    uint64_t left = c->up ? c->quire.blocks * TAGFS_BLOCK_SECTORS - c->off : 0;
    uint64_t next = c->up ? c->next.blocks * TAGFS_BLOCK_SECTORS : 0;
    uint64_t at   = c->quire.block * TAGFS_BLOCK_SECTORS + c->off, m = RibbonMediumAtom();
    /* A stitch ends on an atom of the ribbon (RibbonStitchEnds): begun off
     * one, or in a Quire a writer of a smaller atom laid off one, it may take
     * up to that atom less the medium's past its length — that much less is
     * claimed. On the grid it takes nothing more. */
    uint64_t slack = c->atom > m && ((at % c->atom) || !RibbonQuireOn(&c->quire, c->atom) ||
                                     (c->next.blocks && !RibbonQuireOn(&c->next, c->atom))) ? c->atom - m : 0;
    if (flight && flight < left) {
        left -= flight;
    } else if (flight) {
        uint64_t past = flight - left;
        left = next > past ? next - past : 0;
        next = 0;
    }
    *any   = next && left + next > 2u * c->atom + slack ? left + next - 2u * c->atom - slack : 0;
    *frees = left > c->atom + slack ? left - c->atom - slack : 0;
    if (*frees < *any)
        *frees = *any;
}

static void RibbonRoomPublish(void)
{
    const RibbonCursor* c      = &s_cursor;
    uint64_t            flight = __atomic_load_n(&s_flight, __ATOMIC_ACQUIRE);
    uint64_t            left   = c->up ? c->quire.blocks * TAGFS_BLOCK_SECTORS - c->off : 0;
    uint64_t            any = 0, frees = 0;
    RibbonRoomOf(c, flight, &any, &frees);
    __atomic_store_n(&s_room_any, RibbonRoomBytes(any), __ATOMIC_RELEASE);
    __atomic_store_n(&s_room_frees, RibbonRoomBytes(frees), __ATOMIC_RELEASE);
    __atomic_store_n(&s_room_grows, (uint8_t)(c->up && ((flight && flight >= left) || c->next.blocks ||
                                                        c->taken != ERR_TAGFS_NO_SPACE)), __ATOMIC_RELEASE);
    __atomic_store_n(&s_room_refused, (uint8_t)(c->up && !c->next.blocks && c->taken == ERR_TAGFS_NO_SPACE),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_room_barred, (uint8_t)(c->up && !c->next.blocks && c->taken == ERR_WOULD_BLOCK),
                     __ATOMIC_RELEASE);
}

/* A Quire's blocks for this volume: the grain it was laid out in (§2.6 — the
 * maker's Quire), a whole number of atoms; the Quire the ribbon stands in
 * when the volume names no grain. */
static uint64_t RibbonQuireBlocks(const RibbonQuire* now, uint32_t atom)
{
    uint64_t grain = tagfs_get_state()->geometry.grain_bytes / TAGFS_BLOCK_SIZE;
    uint64_t atoms = atom / TAGFS_BLOCK_SECTORS ? atom / TAGFS_BLOCK_SECTORS : 1u;
    if (grain == 0)
        grain = now->blocks;
    return (grain + atoms - 1u) / atoms * atoms;
}

/* The cursor gone on into `q` at its start, a Quire a writer of a smaller
 * atom took off an atom of the ribbon — the next_quire named before this
 * mount took one on it, where none could be taken or a retry is taken up
 * there alone (M-6) — said once a Quire: the stitch laid at its start shares
 * a flash page with what lies before it. */
static void RibbonEnteredSaid(const RibbonQuire* q)
{
    uint64_t pb = s_cursor.atom / TAGFS_BLOCK_SECTORS;
    if (!pb || !(q->block % pb) ||
        __atomic_exchange_n(&s_entered_said, q->block + 1u, __ATOMIC_ACQ_REL) == q->block + 1u)
        return;
    kprintf("[TagFS] the ribbon goes on into its Quire %llu+%llu, which a writer of a smaller atom took off a flash "
            "page of %u bytes: the stitch laid at its start shares a page with what lies before it, and ends on a "
            "page, every one after it begun on one (§16.6 п.2)\n", (unsigned long long)q->block,
            (unsigned long long)q->blocks, s_cursor.atom * BOOK_SECTOR_BYTES);
}

/*
 * Where the cursor laid at the walk's end goes on (volume_ribbon.h,
 * RibbonGoesOn): at the end, or at the start of the next_quire named — *hop —
 * said where the stitch laid there first shares a flash page; or nowhere, the
 * words for the glass. A next_quire named off the medium's atom no stitch is
 * written in (§1.5): the cursor forgets it, and the first stitch names one it
 * takes (RibbonNextGiven) — a retry, which the reader takes up there alone,
 * refused (RibbonRetry).
 */
static const char* RibbonGoesOnAt(RibbonCursor* c, bool* hop)
{
    uint32_t way = RibbonGoesOn(c->quire.block, c->quire.blocks, c->off, c->next.block, c->next.blocks,
                                RibbonMediumAtom(), c->atom);
    *hop = (way & RIBBON_ON_NEXT) != 0;
    if (!way)
        return "neither where its ribbon ends nor the next_quire it names lies on the medium's atom — a writer of a "
               "smaller atom laid them — and no write past its end would leave the settled sectors beside it whole "
               "(§1.5)";
    if (way == (RIBBON_ON_END | RIBBON_ON_SHARED))
        kprintf("[TagFS] the ribbon ends at sector %llu of its Quire %llu+%llu, inside a flash page of %u bytes — a "
                "writer of a smaller atom laid it — and names no next_quire laid on pages: the stitch this mount lays "
                "first shares that page with the last one settled, and ends on a page, every one after it begun on "
                "one (§16.2 п.11, §16.6 п.2)\n", (unsigned long long)c->off, (unsigned long long)c->quire.block,
                (unsigned long long)c->quire.blocks, c->atom * BOOK_SECTOR_BYTES);
    if (!*hop && c->next.blocks && !RibbonQuireOn(&c->next, RibbonMediumAtom())) {
        kprintf("[TagFS] the next_quire the ribbon names, %llu+%llu, lies off the medium's atom of %u bytes: no "
                "stitch is written in it, and the next stitch names one taken in its stead (§1.5)\n",
                (unsigned long long)c->next.block, (unsigned long long)c->next.blocks,
                RibbonMediumAtom() * BOOK_SECTOR_BYTES);
        c->next = (RibbonQuire){ 0, 0 };
    }
    return NULL;
}

error_t RibbonCursorMount(const RibbonWalk* w, const char** glass)
{
    RibbonCursor* c   = &s_cursor;
    bool          hop = false;
    memset(c, 0, sizeof(*c));
    __atomic_store_n(&s_entered_said, 0u, __ATOMIC_RELEASE);
    if (!w->has_end) {
        *glass = "its ribbon ends where the walk cannot say what comes next — no writer goes on from there";
        return ERR_TAGFS_READ_ONLY;
    }
    if (!w->clasp_known) {
        *glass = "the Clasp of its last stitch does not read — no stitch can name it";
        return ERR_TAGFS_READ_ONLY;
    }
    c->quire = w->end.quire;
    c->off   = w->end.off;
    c->next  = RibbonQuireSame(&w->end.quire, &w->quire) ? w->next : (RibbonQuire){ 0, 0 };
    c->came  = RibbonQuireSame(&w->end.quire, &w->quire) ? (RibbonQuire){ 0, 0 } : w->quire;
    c->atom  = g_book.ribbon_atom / BOOK_SECTOR_BYTES;
    if (c->off >= c->quire.blocks * TAGFS_BLOCK_SECTORS && !c->next.blocks) {
        *glass = "its ribbon fills its last Quire, and names no next_quire to go on in";
        return ERR_TAGFS_READ_ONLY;
    }
    if ((*glass = RibbonGoesOnAt(c, &hop)) != NULL)
        return ERR_TAGFS_READ_ONLY;
    if (hop) {
        RibbonEnteredSaid(&c->next);
        c->came  = c->quire;
        c->quire = c->next;
        c->next  = (RibbonQuire){ 0, 0 };
        c->off   = 0;
    }
    if (c->off)
        c->came = (RibbonQuire){ 0, 0 };
    /* What the walk holds past its last stitch, as rules (б) and (б′) read it
     * (ribbon_collate.c, RibbonFurther): the Quire it ends in, and the
     * next_quire it walks — the one its end lies at the start of included. */
    c->ended        = w->quire;
    c->ended_from   = w->seq;
    c->named        = w->next;
    c->last         = w->seq;
    c->clasp        = w->clasp;
    c->tally        = w->slot->head.tally_seq;
    c->given        = BookSeqSeen();
    c->quire_blocks = RibbonQuireBlocks(&c->quire, c->atom);
    c->quires       = w->nquires;
    c->lied         = w->lied;
    c->taken        = OK;
    c->up           = true;
    s_claimed       = 0;
    s_reserved      = 0;
    __atomic_store_n(&s_flight, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_refused_free, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_long_said, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_inside_said, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_room_waited, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_refused_said, 0u, __ATOMIC_RELEASE);
    RibbonLeftMount(w, c->lied, c->given);
    RibbonRoomPublish();
    return OK;
}

void RibbonCursorLetGo(void)
{
    RibbonLeftLetGo();
    memset(&s_cursor, 0, sizeof(s_cursor));
    s_claimed  = 0;
    s_reserved = 0;
    __atomic_store_n(&s_flight, 0, __ATOMIC_RELEASE);
    RibbonRoomPublish();
}

bool RibbonCursorLied(void)
{
    return s_cursor.lied;
}

const RibbonCursor* RibbonCursorNow(void)
{
    return &s_cursor;
}

#ifdef CONFIG_VOLUME_TESTS
void RibbonCursorStand(RibbonCursor* out)
{
    *out = s_cursor;
}

void RibbonClaimsStand(uint64_t* claimed, uint64_t* reserved)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    *claimed  = s_claimed;
    *reserved = s_reserved;
    TagFSLockDrop(&fs->lock);
}

static volatile bool s_stand_marks_refused;

void RibbonStandMarksRefused(void)
{
    __atomic_store_n(&s_stand_marks_refused, true, __ATOMIC_RELEASE);
}

bool RibbonStandMarksRefusing(void)
{
    return __atomic_exchange_n(&s_stand_marks_refused, false, __ATOMIC_ACQ_REL);
}
#endif

uint64_t RibbonSeqNext(void)
{
    uint64_t seen = BookSeqSeen();
    s_cursor.given = (s_cursor.given > seen ? s_cursor.given : seen) + 1u;
    BookSeqGiven(s_cursor.given);
    return s_cursor.given;
}

#ifdef CONFIG_VOLUME_TESTS
static volatile bool s_stand_next_refused;

bool RibbonStandNextRefused(bool armed)
{
    return __atomic_exchange_n(&s_stand_next_refused, armed, __ATOMIC_ACQ_REL);
}
#endif

/* Whether the cursor wants a next_quire: none named, or the one named off an
 * atom of the ribbon. */
static bool RibbonNextWanted(const RibbonCursor* c)
{
    return c->up && !(c->next.blocks && RibbonQuireOn(&c->next, c->atom));
}

/* A Quire of the map for the next_quire, named by the cursor: OK, or the
 * map's refusal. */
static error_t RibbonNextTake(RibbonCursor* c)
{
    uint64_t start = 0;
#ifdef CONFIG_VOLUME_TESTS
    if (__atomic_exchange_n(&s_stand_next_refused, false, __ATOMIC_ACQ_REL))
        return ERR_TAGFS_NO_SPACE;
#endif
    error_t rc = BlockMapTakeQuire(GROUND_RIBBON, c->quire_blocks,
                                   c->atom / TAGFS_BLOCK_SECTORS ? c->atom / TAGFS_BLOCK_SECTORS : 1u,
                                   EndpaperKeeps(c->quire_blocks), g_book.frame.data_block,
                                   c->quire.block + c->quire.blocks, &start);
    if (rc != OK)
        return rc;
    if (c->next.blocks)
        kprintf("[TagFS] the next_quire the ribbon names, %llu+%llu, a writer of a smaller atom took off a "
                "flash page of %u bytes: the ribbon names %llu+%llu, laid on pages, in its stead (§16.6 п.2)\n",
                (unsigned long long)c->next.block, (unsigned long long)c->next.blocks,
                c->atom * BOOK_SECTOR_BYTES, (unsigned long long)start, (unsigned long long)c->quire_blocks);
    c->next = (RibbonQuire){ start, c->quire_blocks };
    BinderRibbonGiven(&c->next);
    __atomic_store_n(&c->out, false, __ATOMIC_RELEASE);   /* the next run out is said again */
    return OK;
}

/*
 * ‼ THE NEXT QUIRE IS GIVEN AHEAD OF NEED (§3.6): the mount asks for one
 * where the cursor wants it, every cycle asks at its TAKE while none was
 * given — before anything of the cycle claims room past it — and every stitch
 * that settles asks again where the ribbon then stands with none — so that a Quire's end, an early close or a refused write always has somewhere
 * to go on, named in every StitchHead from then on (M-5). It is a whole Quire on an atom of the data run, looked
 * for past the Quire the ribbon stands in, and never one the ground the
 * Tallies need would go to (EndpaperKeeps — a whole Tally, and what the ring's
 * slots lack of one) nor a block promised to a Slip's tail (BlockMapTakeQuire),
 * nor ground a file of a Tally a mount may stand on lies on — given up past
 * it, its ground is data's, and a reader would refuse the step there
 * (fallow.c, NodeBarred): one taken there waits for the Tally past the delete
 * the refusal asks for. A Quire of the ribbon comes back only once the slots stop
 * naming it, which only a Tally written over one of them does. Laid on an
 * atom of the ribbon — a flash page on a removable medium (§16.6 п.2) — and
 * asked for again while the one named is not: a next_quire a writer of a
 * smaller atom took off a page is gone on into only while none on a page can
 * be taken, its ground held until a Tally lets the ribbon's Quires before
 * the one it stands in go, as every Quire a stitch named.
 */
static void RibbonNextAsk(bool ask)
{
    RibbonCursor* c = &s_cursor;
    if (!RibbonNextWanted(c))
        return;
    c->taken = RibbonNextTake(c);
    if (c->taken == ERR_WOULD_BLOCK && BinderNoTallies())
        c->taken = ERR_TAGFS_NO_SPACE;   /* no Tally of this mount lifts the bar (fallow.c, NodeBarred) */
    if (c->taken != OK && !c->next.blocks) {
        /* A Tally gives the ribbon ground — the slots' Quires, the bar lifted
         * — never the heap a refusal of memory wanted: it would want it too. */
        if (ask && (c->taken == ERR_TAGFS_NO_SPACE || c->taken == ERR_WOULD_BLOCK))
            SettleDemand(SETTLE_DEMAND_TALLY);
        if (c->taken == ERR_TAGFS_NO_SPACE)
            __atomic_store_n(&s_refused_free, AllotmentFreeUnpromised(), __ATOMIC_RELEASE);
    }
    RibbonRoomPublish();
}

void RibbonNextGiven(void)
{
    RibbonNextAsk(true);
}

void RibbonNextAgain(uint64_t slice)
{
    s_again_turns = slice == s_again_slice ? s_again_turns + 1u : 1u;
    s_again_slice = slice;
    if (!s_cursor.next.blocks)
        RibbonNextAsk(s_again_turns < g_binder.ring);
}

/*
 * The mount's own ask, before the door opens: a next_quire given where the map
 * has one, and a refusal left to the first cycle's ask — the cursor as the
 * mount laid it, its room still to grow (RibbonRoomFor: ERR_WOULD_BLOCK, and a
 * cycle on the asker's word). Taken here a refusal would shut the room before
 * any change could ask for that cycle, demand a Tally of a cadence the mount
 * resets after (SettleMountReset) and no settled cycle of this mount would let
 * begin (BinderOwnMayNot), and be judged against the promises of the mounting
 * before, whose epoch this one has not passed yet (AllotmentPromised) — a
 * Quire found only on ground a mount's Tally keeps from the ribbon
 * (ERR_WOULD_BLOCK, fallow.c NodeBarred) alike.
 */
void RibbonNextGivenAtMount(void)
{
    RibbonCursor* c = &s_cursor;
    if (RibbonNextWanted(c) && RibbonNextTake(c) == OK)
        RibbonRoomPublish();
}

void RibbonTallied(uint64_t tally, uint64_t taken)
{
    s_cursor.tally = tally;
    RibbonLeftTallied(taken);
}

/*
 * The ribbon run out under a stitch of the Settle's holder, said once until it
 * is given a next_quire again: no next_quire, and the stitch does not fit in
 * the rest of its Quire (RibbonRoom). The stitch is no change refused — a word
 * aboard is left out of it and the stitch laid again (sewing.c, StitchBuild),
 * a change short of room waits on the Tally that gives the ribbon its
 * next_quire while one may (RibbonClaimSay) and is refused with its own line
 * (RibbonRefusedSaid) — so this line says the ribbon's state, never the end of
 * a change's wait.
 */
static void RibbonOutSaid(void)
{
    RibbonCursor* c    = &s_cursor;
    const char*   none = BinderNoTallies();
    if (__atomic_exchange_n(&c->out, true, __ATOMIC_ACQ_REL))
        return;
    kprintf("[TagFS] the ribbon has run out: %llu Quire(s) taken for it, %llu block(s) free, %llu kept for "
            "the Tallies and %llu promised to the tails Slips lie over — %s%s; until then %s, and the rest of its "
            "Quire is kept for what was claimed before and what gives ground up\n",
            (unsigned long long)__atomic_load_n(&c->quires, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&tagfs_get_state()->free_blocks, __ATOMIC_ACQUIRE),
            (unsigned long long)EndpaperKeeps(c->quire_blocks), (unsigned long long)AllotmentPromised(),
            none ? "no Tally is written by this mount, so none of its Quires comes back before the volume is "
                   "mounted again: " : "a Tally is asked for to take its end in, and its Quires come back only once "
                   "the slots stop naming them",
            none ? none : "",
            none ? "files, names, tags and words are refused (ERR_TAGFS_NO_SPACE)"
                 : "a word the person says finds no room, and a file, a name or a tag waits on that Tally — "
                   "refused once the ring has turned or none can give");
}

/*
 * ‼ A STITCH THAT RUNS ON INTO ITS NEXT_QUIRE LEAVES TWO ATOMS OF IT (§2.7,
 * §3.6). Past it the ribbon stands in that Quire with no next_quire given,
 * where the rest is kept for what was claimed before — the room a claim was
 * judged by was held for it — and for a stitch that frees or takes nothing —
 * the halt's, a waiter's — and one atom more for the stitch that gives the
 * next_quire once ground comes back. One that filled the Quire whole would end
 * where no reader can say what comes next (RibbonPlaceOn: the next_quire it
 * would hop to is not named yet), and the mount would stand the volume under
 * glass with every stitch sewn after it unread. A word of text the person said
 * is the one thing a stitch carries that no claim held room for (`promised`
 * false): refused here, it is left out of the stitch, and the empty UseContext
 * laid in its place — the volume forgetting the word it held, which gives up
 * as a delete does — is promised (sewing.c).
 */
uint64_t RibbonStitchOf(uint64_t len)
{
    const RibbonCursor* c    = &s_cursor;
    uint64_t            left = c->quire.blocks * TAGFS_BLOCK_SECTORS - c->off;
    return RibbonStitchEnds(RibbonStitchSectors(len, RibbonMediumAtom()),
                            c->quire.block * TAGFS_BLOCK_SECTORS + c->off, left,
                            c->next.blocks ? c->next.block * TAGFS_BLOCK_SECTORS : 0, c->atom);
}

/* A stitch of `of` sectors given its place, `left` of its Quire past it: OK,
 * said where it runs on into a Quire begun off a flash page, or ends inside
 * one short of its Quire's end (volume_ribbon.h, RibbonStitchEnds) — the
 * stitch after it shares that page. */
static error_t RibbonPlaced(uint64_t of, uint64_t left)
{
    const RibbonCursor* c   = &s_cursor;
    uint64_t            end = c->quire.block * TAGFS_BLOCK_SECTORS + c->off + of;
    if (of > left)
        RibbonEnteredSaid(&c->next);
    else if (of < left && end % c->atom && !__atomic_exchange_n(&s_inside_said, 1u, __ATOMIC_ACQ_REL))
        kprintf("[TagFS] the ribbon's Quire %llu+%llu, which a writer of a smaller atom laid, ends inside a flash "
                "page of %u bytes and no next_quire is given: a stitch of %llu sector(s) ends inside a page short of "
                "it, and the stitch after it shares that page (§3.6, §16.6 п.2)\n",
                (unsigned long long)c->quire.block, (unsigned long long)c->quire.blocks,
                c->atom * BOOK_SECTOR_BYTES, (unsigned long long)of);
    return OK;
}

error_t RibbonRoom(uint64_t of, bool promised, RibbonPlace* at, RibbonQuire* hop)
{
    RibbonCursor* c    = &s_cursor;
    uint64_t      left = c->quire.blocks * TAGFS_BLOCK_SECTORS - c->off;
    at->quire = c->quire;
    at->off   = c->off;
    *hop      = c->next;
    if (c->next.blocks) {
        uint64_t most = left + c->next.blocks * TAGFS_BLOCK_SECTORS - 2u * c->atom;
        if (of <= left || of <= most)
            return RibbonPlaced(of, left);
        if (c->refused)
            return ERR_TAGFS_READ_ONLY;
        if (promised && !__atomic_exchange_n(&s_long_said, 1u, __ATOMIC_ACQ_REL))
            kprintf("[TagFS] a stitch of %llu sector(s) runs on past the rest of its Quire into the next one and "
                    "would leave less of it than the two atoms kept for a stitch that takes nothing and the one "
                    "that gives its next_quire (%llu sector(s) at the most) — it is not begun (§2.7, §3.6): what "
                    "it carries is refused, ERR_TAGFS_NO_SPACE\n", (unsigned long long)of, (unsigned long long)most);
        return ERR_TAGFS_NO_SPACE;
    }
    if (c->taken == ERR_NO_MEMORY)
        return ERR_NO_MEMORY;
    /* A retry (M-6) carries what the cycle already set out to write, in the
     * next_quire it went on into: taken while an atom is left past it, so a
     * refusal there is the medium's, said as one (sewing.c). One that does
     * not fit is no ribbon run out: the next_quire is spent on refusals. What
     * was claimed, and a stitch of freeing records alone, is what the rest is
     * kept for (§3.6): the ground it gives up is what a next_quire is taken
     * from again. */
    if ((promised || c->refused) && of + c->atom <= left)
        return RibbonPlaced(of, left);
    if (c->refused)
        return ERR_TAGFS_READ_ONLY;
    RibbonOutSaid();
    return ERR_TAGFS_NO_SPACE;
}

/* The stitch settled at `at`: the cursor past it — a stitch that ends on its
 * Quire's last sector goes on at the next Quire's first, RibbonRoom gave it
 * one — and what the mount's reader holds past it, as RibbonTake does
 * (ribbon_collate.c): the Quire its last sector lies in, the next_quire forgot
 * when that is a Quire the walk had not ended in, and the one it names taken
 * unless it is the Quire it ends in. */
uint64_t RibbonSewn(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, uint64_t seq, uint32_t clasp,
                    uint32_t clasp_at, uint64_t flags)
{
    RibbonCursor* c     = &s_cursor;
    uint64_t      left  = at->quire.blocks * TAGFS_BLOCK_SECTORS - at->off;
    RibbonQuire   last  = of <= left ? at->quire : *hop;
    uint64_t      bound = RibbonQuireSame(&at->quire, &c->ended) ? c->ended_from : seq;
    BookLeftMark  mark  = { .at = *at, .hop = *hop, .of = of, .seq = seq, .clasp = clasp, .clasp_at = clasp_at };
    c->last    = seq;
    c->clasp   = clasp;
    c->refused = 0;
    c->cut     = 0;
    RibbonLeftSewn(&mark, c->lied);
#ifdef CONFIG_VOLUME_TESTS
    c->sewn       = *at;
    c->sewn_hop   = *hop;
    c->sewn_of    = of;
    c->sewn_flags = flags;
#else
    (void)flags;
#endif
    if (!RibbonQuireSame(&last, &c->ended)) {
        c->ended      = last;
        c->ended_from = seq;
        c->named      = (RibbonQuire){ 0, 0 };
    }
    if (hop->blocks && !RibbonQuireSame(hop, &c->ended))
        c->named = *hop;
    if (of < left) {
        c->off = at->off + of;
    } else {
        c->came  = at->quire;
        c->quire = *hop;
        c->next  = (RibbonQuire){ 0, 0 };
        c->off   = of - left;
        __atomic_add_fetch(&c->quires, 1u, __ATOMIC_RELAXED);
    }
    if (c->off)
        c->came = (RibbonQuire){ 0, 0 };
    RibbonRoomPublish();
    return bound;
}

/*
 * ‼ A RETRY GOES WHERE THE MOUNT'S READER WALKS, NEVER OVER A TRY (M-6, §4.5
 * п.5 (б), §16.4 п.4). Past the last stitch it took, the reader looks at the
 * first atom after it — where the refused attempt lies, which is never written
 * over — and walks the next_quire that stitch names from its start, over every
 * unit lying there, to the first whole stitch naming it and the Quire it ends
 * in as the one it came from (ribbon_collate.c, RibbonFurther;
 * stitch_ribbon.py alike). So the retry goes to that next_quire: at its start,
 * or, where the attempt refused lies in it, on the atom past the attempt's
 * `of` — however much of it the medium took — with a new seq, naming the Quire
 * the last stitch ends in. And not at all when no next_quire is named past it
 * (there was nothing to take), or when the attempts refused have run it to its
 * end: a retry anywhere else is a stitch the waiters were told is kept and the
 * next mount does not read.
 */
const char* RibbonRetry(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, bool cut)
{
    RibbonCursor* c = &s_cursor;
    RibbonPlace   end;
    uint64_t      off = 0;
    c->refused++;
    c->cut += cut ? 1u : 0u;
    if (!c->named.blocks)
        return "the last stitch sewn names no next_quire, where alone a reader takes a retry up";
    if (!RibbonQuireOn(&c->named, RibbonMediumAtom()))
        return "the next_quire the last stitch sewn names, where alone a reader takes a retry up, lies off the "
               "medium's atom: a write there would rewrite the sectors beside it (§1.5)";
    if (RibbonPlaceOn(at, of, hop, &end) && RibbonQuireSame(&end.quire, &c->named))
        off = end.off;
    else if (!RibbonQuireSame(&at->quire, &c->ended))
        return "the attempts refused run the next_quire the last stitch sewn names to its end";
    if (!RibbonQuireSame(&c->quire, &c->named)) {
        c->next = (RibbonQuire){ 0, 0 };
        __atomic_add_fetch(&c->quires, 1u, __ATOMIC_RELAXED);
    }
    if (off == 0)
        RibbonEnteredSaid(&c->named);
    c->came  = c->ended;
    c->quire = c->named;
    c->off   = off;
    RibbonRoomPublish();
    return NULL;
}

/*
 * ‼ A QUIRE IS CLOSED EARLY ONLY WHERE THE MOUNT'S READER TAKES THE STITCH UP
 * (В1, §3.4, §4.5 п.5 (б)). Past the last stitch sewn, a reader that finds no
 * stitch on the atom after it walks the next_quire that stitch names from its
 * start to the first whole stitch naming it, and the Quire it ends in as the
 * one it came from — so the next stitch may begin there, at the next_quire's
 * first sector, leaving the rest of the Quire unwritten: the ribbon has left
 * it, and the ground its stitches gave up comes back with their lag
 * (FallowQuireLeft). Only where the last stitch names that next_quire and the
 * cursor stands in the Quire it ends in, with no retry under way.
 */
bool RibbonCloseEarly(void)
{
    RibbonCursor* c = &s_cursor;
    if (!c->up || c->refused || !c->named.blocks || !RibbonQuireSame(&c->quire, &c->ended) ||
        !RibbonQuireSame(&c->named, &c->next))
        return false;
    RibbonEnteredSaid(&c->named);
    c->came  = c->ended;
    c->quire = c->named;
    c->next  = (RibbonQuire){ 0, 0 };
    c->off   = 0;
    __atomic_add_fetch(&c->quires, 1u, __ATOMIC_RELAXED);
    RibbonRoomPublish();
    return true;
}

// ----------------------------------------------------------------------------
// A change's room on the ribbon (P21)
// ----------------------------------------------------------------------------

void RibbonFlightBegins(uint64_t sectors)
{
    __atomic_store_n(&s_flight, sectors, __ATOMIC_RELEASE);
    RibbonRoomPublish();
}

void RibbonFlightEnds(void)
{
    __atomic_store_n(&s_flight, 0, __ATOMIC_RELEASE);
    RibbonRoomPublish();
}

/* The bytes no stitch has taken: the claims, what is held for tags about to
 * be given ids, and the Coineds of the tags given ids since the last stitch
 * settled — counted whoever gave them (tag_registry.h, TagRegistryCoinedHeld). */
static uint64_t RibbonHeld(void)
{
    uint32_t coined = 0;
    uint64_t text   = 0;
    TagRegistryCoinedHeld(tagfs_get_state()->registry, &coined, &text);
    return s_claimed + s_reserved + (uint64_t)coined * STITCH_COINED_MOST + text;
}

/*
 * ‼ ROOM REFUSED IS WAITED FOR ONLY WHILE ROOM CAN COME (§3.6, RT-06/07). A
 * stitch takes every change inked before it, so a change is refused that would
 * leave the next stitch more than the ribbon holds; it waits — ERR_WOULD_BLOCK,
 * and a Chit on the cycle's end — while claims or a stitch are in flight whose
 * settling frees what they held at the most (a hold is a claim in the making:
 * its change claims no more than it, and the cycle after takes that), while a
 * next_quire may still be
 * given, the ground having grown since the last ask was refused — the free
 * ground past the blocks promised to Slips' tails, which a next_quire leaves
 * (AllotmentFreeUnpromised) — or while
 * ground rests in a Quire the ribbon has left for no more than its lag. Else
 * nothing a cycle does makes room: ERR_TAGFS_NO_SPACE, at once.
 */
static error_t RibbonRoomFor(uint64_t bytes, bool frees)
{
    uint64_t room = __atomic_load_n(frees ? &s_room_frees : &s_room_any, __ATOMIC_ACQUIRE);
    uint64_t want = bytes ? bytes : STITCH_RECORD_HEAD_MOST;
    uint64_t held = RibbonHeld();
    if (room >= want && held <= room - want) {
        /* Room for a change that does not free is what a change short of it
         * waited on a Tally for: the shortfall ended, the next has its turn of
         * the ring again. A freeing one is given room in the rest kept for it
         * while the shortfall stands (RibbonRoomOf), and ends nothing. */
        if (!frees && __atomic_load_n(&s_room_waited, __ATOMIC_ACQUIRE) &&
            __atomic_exchange_n(&s_room_waited, 0u, __ATOMIC_ACQ_REL))
            BinderRoomTaken();
        return OK;
    }
    if (s_claimed || s_reserved || __atomic_load_n(&s_flight, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_room_grows, __ATOMIC_ACQUIRE) ||
        AllotmentFreeUnpromised() > __atomic_load_n(&s_refused_free, __ATOMIC_ACQUIRE) ||
        FallowLeftBlocks())
        return ERR_WOULD_BLOCK;
    return ERR_TAGFS_NO_SPACE;
}

error_t RibbonClaim(uint64_t bytes, bool frees)
{
    error_t rc = RibbonRoomFor(bytes, frees);
    if (rc == OK)
        s_claimed += bytes;
    return rc;
}

error_t RibbonReserve(uint64_t bytes)
{
    error_t rc = RibbonRoomFor(bytes, false);
    if (rc == OK)
        s_reserved += bytes;
    return rc;
}

void RibbonReserveLetGo(uint64_t bytes)
{
    s_reserved = s_reserved > bytes ? s_reserved - bytes : 0;
}

error_t RibbonClaimHeld(uint64_t held, uint64_t bytes)
{
    if (bytes > held) {
        error_t rc = RibbonRoomFor(bytes - held, false);
        if (rc != OK)
            return rc;
    }
    s_reserved = s_reserved > held ? s_reserved - held : 0;
    s_claimed += bytes;
    return OK;
}

uint64_t RibbonClaimTaken(void)
{
    uint64_t taken = s_claimed;
    s_claimed      = 0;
    return taken;
}

void RibbonClaimBack(uint64_t bytes)
{
    s_claimed += bytes;
}

/*
 * A change refused room on the ribbon, said once until a change is given room
 * on it — as a write of data refused at the wall says it (allotment.c,
 * AllotmentBookShort), as its wait for a Tally ends: what was free, what the
 * book keeps — the Tallies' reserve and the Quire the ribbon asks past it, as
 * a write of data leaves both — the ground it holds itself, named
 * (BinderGroundHeld), and how the reserve was measured (BinderRoomMeasured),
 * so a write the ribbon ends is weighed by the kernel's word as one the data's
 * wall ends (logcheck fullvolume, roomback). Said by the change, never by the
 * Settle's stitch (RibbonOutSaid): its words are this change's, at the end of
 * this change's wait.
 */
static void RibbonRefusedSaid(void)
{
    const char* why = NULL;
    if (__atomic_exchange_n(&s_refused_said, 1u, __ATOMIC_ACQ_REL))
        return;
    const char* measured = BinderRoomMeasured(&why);
    kprintf("[TagFS] no room on the ribbon for a change of the volume: %llu free, the book keeps %llu for its "
            "ribbon and its Tally and holds %llu itself in its Tallies and its ribbon, and %llu are promised to the "
            "tails Slips lie over — the reserve measured %s%s%s (ERR_TAGFS_NO_SPACE)\n",
            (unsigned long long)__atomic_load_n(&tagfs_get_state()->free_blocks, __ATOMIC_ACQUIRE),
            (unsigned long long)(EndpaperKeeps(s_cursor.quire_blocks) + s_cursor.quire_blocks),
            (unsigned long long)BinderGroundHeld(), (unsigned long long)AllotmentPromised(), measured,
            why ? ": " : "", why ? why : "");
}

/*
 * ‼ A CHANGE SHORT OF ROOM ON THE RIBBON WAITS FOR THE TALLY THAT GIVES IT ONE
 * (§3.6, §16.6 п.8). With no next_quire every claim but a freeing one waits
 * (RibbonRoomOf), and while the next_quire is found only on ground a delete
 * past the newest Tally gave back, nothing but a Tally past the delete gives
 * one (fallow.c, NodeBarred). Waited out a cycle at a time, the cycles were a
 * stitch each with nothing a claim may lay, in the one Quire the ribbon stood
 * in, and a stitch of the Tally's steps between every two: the Quire's rest
 * was spent before the Tally ended, the stitch past it found no room, and its
 * waiters — the GiB written the moment a GiB was deleted among them — were
 * refused no-space half-way (fullvolume, roomback). So the change waits on that
 * Tally, as a write short of ground does (binder.h, BinderRibbonWait): its own
 * wait for room writes it, and the cadence gives it every turn. Refused past
 * the reserve, it waits as a write of data refused past it does — on the Tally
 * under way, or one that makes the measure exact or gives the ring's ground
 * back, whose end asks the next_quire again by the measure it makes
 * (RibbonNextAgain): at a full volume's wall the room a measure from above
 * refused — and is refused once the ring has turned or none can give, said as
 * its own refusal (RibbonRefusedSaid). A change that does not free given room
 * once one waited gives the next shortfall its turn of the ring again
 * (RibbonRoomFor).
 */
error_t RibbonClaimSay(error_t rc)
{
    bool barred  = rc == ERR_WOULD_BLOCK && __atomic_load_n(&s_room_barred, __ATOMIC_ACQUIRE);
    bool refused = rc == ERR_TAGFS_NO_SPACE && __atomic_load_n(&s_room_refused, __ATOMIC_ACQUIRE);
    if ((barred || refused) && BinderRibbonWait(barred)) {
        __atomic_store_n(&s_room_waited, 1u, __ATOMIC_RELEASE);
        return ERR_WOULD_BLOCK;
    }
    if (rc == OK && __atomic_load_n(&s_refused_said, __ATOMIC_ACQUIRE))
        __atomic_store_n(&s_refused_said, 0u, __ATOMIC_RELEASE);   /* the next refusal is said again */
    if (rc == ERR_TAGFS_NO_SPACE)
        RibbonRefusedSaid();
    return rc;
}
