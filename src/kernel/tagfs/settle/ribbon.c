#include "sewing.h"
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
 * ribbon ended (RibbonCursorMount) and moved by the Settle's holder alone — a
 * stitch settled, a write refused. Nothing else changes it, so it needs no
 * lock: the stand reads it between two cycles, on the runner's core.
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

/* The bytes claimed by changes no stitch has taken, and those held for tags
 * about to be given ids (RibbonReserve): under the state lock. */
static uint64_t          s_claimed;
static uint64_t          s_reserved;

/* The sectors the stitch in flight takes at the most; 0 with none. Set under
 * the state lock at TAKE, cleared by the Settle's holder at the cycle's end. */
static volatile uint64_t s_flight;

/* A stitch with no word aboard that the rest and the next_quire do not hold,
 * said once a mount. */
static volatile uint8_t  s_long_said;

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
    if (flight && flight < left) {
        left -= flight;
    } else if (flight) {
        uint64_t past = flight - left;
        left = next > past ? next - past : 0;
        next = 0;
    }
    *any   = next && left + next > 2u * c->atom ? left + next - 2u * c->atom : 0;
    *frees = left > c->atom ? left - c->atom : 0;
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

error_t RibbonCursorMount(const RibbonWalk* w, const char** glass)
{
    RibbonCursor* c = &s_cursor;
    memset(c, 0, sizeof(*c));
    c->atom = g_book.atom / BOOK_SECTOR_BYTES;
    if (!w->has_end) {
        *glass = "its ribbon ends where the walk cannot say what comes next — no writer goes on from there";
        return ERR_TAGFS_READ_ONLY;
    }
    if (!w->clasp_known) {
        *glass = "the Clasp of its last stitch does not read — no stitch can name it";
        return ERR_TAGFS_READ_ONLY;
    }
    c->quire = w->end.quire;
    c->off   = (w->end.off + c->atom - 1u) / c->atom * c->atom;
    c->next  = RibbonQuireSame(&w->end.quire, &w->quire) ? w->next : (RibbonQuire){ 0, 0 };
    c->came  = RibbonQuireSame(&w->end.quire, &w->quire) ? (RibbonQuire){ 0, 0 } : w->quire;
    if (c->off >= c->quire.blocks * TAGFS_BLOCK_SECTORS) {
        if (!c->next.blocks) {
            *glass = "its ribbon fills its last Quire, and names no next_quire to go on in";
            return ERR_TAGFS_READ_ONLY;
        }
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
    RibbonRoomPublish();
    return OK;
}

void RibbonCursorLetGo(void)
{
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
#endif

uint64_t RibbonSeqNext(void)
{
    uint64_t seen = BookSeqSeen();
    s_cursor.given = (s_cursor.given > seen ? s_cursor.given : seen) + 1u;
    BookSeqGiven(s_cursor.given);
    return s_cursor.given;
}

/*
 * ‼ THE NEXT QUIRE IS GIVEN AHEAD OF NEED (§3.6): the first stitch of each
 * Quire of the ribbon asks for the one after it, and every stitch after it
 * asks again while none was given — so that a Quire's end, an early close or
 * a refused write always has somewhere to go on, named in every StitchHead
 * from then on (M-5). It is a whole Quire on an atom of the data run, looked
 * for past the Quire the ribbon stands in, and never one the ground a whole
 * Tally needs would go to (EndpaperKeeps): with no Tally written by this
 * kernel, a Quire of the ribbon never comes back, and the reserve is what is
 * left to the writer of one.
 */
void RibbonNextGiven(void)
{
    RibbonCursor* c = &s_cursor;
    uint64_t      start = 0;
    if (!c->up || c->next.blocks)
        return;
    c->taken = BlockMapTakeQuire(c->quire_blocks, c->atom / TAGFS_BLOCK_SECTORS ? c->atom / TAGFS_BLOCK_SECTORS : 1u,
                                 EndpaperKeeps(c->quire_blocks), g_book.frame.data_block,
                                 c->quire.block + c->quire.blocks, &start);
    if (c->taken == OK)
        c->next = (RibbonQuire){ start, c->quire_blocks };
    else if (c->taken == ERR_TAGFS_NO_SPACE)
        __atomic_store_n(&s_refused_free, __atomic_load_n(&tagfs_get_state()->ledger.free_blocks, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
    RibbonRoomPublish();
}

/* The ribbon run out, said once a mount — by the Settle's holder or by the
 * first change refused room: no next_quire, and none can be given. */
static void RibbonOutSaid(void)
{
    RibbonCursor* c = &s_cursor;
    if (__atomic_exchange_n(&c->out, true, __ATOMIC_ACQ_REL))
        return;
    kprintf("[TagFS] the ribbon has run out with no Tally to take it in: %llu Quire(s) taken for it, %llu "
            "block(s) free and %llu kept for a whole Tally — this kernel writes no Tally, so no Quire of the "
            "ribbon comes back; files, names, tags and words are refused from here (ERR_TAGFS_NO_SPACE), and "
            "the rest of its Quire is kept for what was claimed before and what gives ground up\n",
            (unsigned long long)__atomic_load_n(&c->quires, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&tagfs_get_state()->ledger.free_blocks, __ATOMIC_ACQUIRE),
            (unsigned long long)EndpaperKeeps(c->quire_blocks));
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
 * glass with every stitch sewn after it unread. A word the person said is the
 * one thing a stitch carries that no claim held room for (`promised` false):
 * refused here, it is left out of the stitch (sewing.c).
 */
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
            return OK;
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
        return OK;
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
                    uint64_t flags)
{
    RibbonCursor* c     = &s_cursor;
    uint64_t      left  = at->quire.blocks * TAGFS_BLOCK_SECTORS - at->off;
    RibbonQuire   last  = of <= left ? at->quire : *hop;
    uint64_t      bound = RibbonQuireSame(&at->quire, &c->ended) ? c->ended_from : seq;
    c->last    = seq;
    c->clasp   = clasp;
    c->refused = 0;
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
 * in as the one it came from (ribbon_collate.c, RibbonFurther; stitch.py
 * alike). So the retry goes to that next_quire: at its start, or, where the
 * attempt refused lies in it, on the atom past the attempt's `of` — however
 * much of it the medium took — with a new seq, naming the Quire the last
 * stitch ends in. And not at all when no next_quire is named past it (there
 * was nothing to take), or when the attempts refused have run it to its end:
 * a retry anywhere else is a stitch the waiters were told is kept and the next
 * mount does not read.
 */
const char* RibbonRetry(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of)
{
    RibbonCursor* c = &s_cursor;
    RibbonPlace   end;
    uint64_t      off = 0;
    c->refused++;
    if (!c->named.blocks)
        return "the last stitch sewn names no next_quire, where alone a reader takes a retry up";
    if (RibbonPlaceOn(at, of, hop, &end) && RibbonQuireSame(&end.quire, &c->named))
        off = end.off;
    else if (!RibbonQuireSame(&at->quire, &c->ended))
        return "the attempts refused run the next_quire the last stitch sewn names to its end";
    if (!RibbonQuireSame(&c->quire, &c->named)) {
        c->next = (RibbonQuire){ 0, 0 };
        __atomic_add_fetch(&c->quires, 1u, __ATOMIC_RELAXED);
    }
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
 * settling frees what they held at the most, while a next_quire may still be
 * given, the ground having grown since the last ask was refused, or while
 * ground rests in a Quire the ribbon has left for no more than its lag. Else
 * nothing a cycle does makes room: ERR_TAGFS_NO_SPACE, at once.
 */
static error_t RibbonRoomFor(uint64_t bytes, bool frees)
{
    uint64_t room = __atomic_load_n(frees ? &s_room_frees : &s_room_any, __ATOMIC_ACQUIRE);
    uint64_t want = bytes ? bytes : STITCH_RECORD_HEAD_MOST;
    uint64_t held = RibbonHeld();
    if (room >= want && held <= room - want)
        return OK;
    if (s_claimed || __atomic_load_n(&s_flight, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_room_grows, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&tagfs_get_state()->ledger.free_blocks, __ATOMIC_ACQUIRE) >
            __atomic_load_n(&s_refused_free, __ATOMIC_ACQUIRE) ||
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

void RibbonClaimSay(error_t rc)
{
    if (rc == ERR_TAGFS_NO_SPACE)
        RibbonOutSaid();
}
