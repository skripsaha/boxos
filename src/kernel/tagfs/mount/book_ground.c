#include "book_mount.h"
#include "ground_census.h"

/*
 * The census of a format-v2 volume (book_mount.h; spec_v2 §4.6): no bitmap
 * says which blocks are taken — every owner names its ground, and what none
 * names is free. The owners are the kernel's one list (ground_owner.h, those
 * of format v2), the same volume-read's census asks of the same image
 * (tools/volumeread/bookcensus.py, from the generated BOOK_OWNERS):
 *
 *   front     the volume before its data run, the Spine's slots aside
 *   spine     every slot of the ring, whatever it holds
 *   tally     every node of every Tally a slot that holds names, both copies —
 *             a rotten one too: its parent names those blocks
 *   ribbon    the Quire the ribbon goes on in from each such slot, and the
 *             next_quire it names where one may be named
 *   file      every DATA run of every live file (tally_leaves.c)
 *   boot      the BOOT run, beside the kernel file it is (not exclusive)
 *   far_deed  the far copy of the Deed, the volume's last block
 *   held      what a rotten range or a locked file may own and no name that
 *             reads does — given to no one until an explicit act lets it go
 *
 * The only finding is a block two exclusive owners name (GroundCensusBook).
 * The ground the ribbon let go, lying fallow, is a ribbon's to name — this
 * kernel mounts no book with anything sewn past its Tally, so none lies so.
 */

/* Ground of the volume `count` blocks long from `start`, claimed by `owner` in
 * runs a claim counts. */
static void BookClaim(GroundOwner owner, uint64_t start, uint64_t count, uint64_t who)
{
    while (count) {
        uint32_t n = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
        GroundClaim(owner, start, n, who);
        start += n;
        count -= n;
    }
}

error_t BookGroundNode(void* ctx, const TallyPlace* at)
{
    (void)ctx;
    for (uint32_t k = 0; k < at->ptr.copies && k < TALLY_COPIES_TRIED; k++)
        BookClaim(GROUND_TALLY, at->ptr.block[k], at->ptr.sheets, 0);
    return OK;
}

void BookGroundHoldRun(uint64_t start, uint64_t count)
{
    uint64_t lo = g_book.frame.data_block, hi = g_book.data_end;
    uint64_t end = start + count < start ? UINT64_MAX : start + count;
    if (start < lo)
        start = lo;
    if (end > hi)
        end = hi;
    if (start < end)
        BookClaim(GROUND_HELD, start, end - start, 0);
}

/* The Quires a slot's ribbon goes on in (stitch.py Ribbon): its Quire when its
 * `at` lies in it, and the next_quire it names when that lies in the data
 * run, on an atom and — for the slot stood on, `taken` — clear of the book
 * and the files. */
static void BookGroundRibbon(const SpineSlot* s, bool taken)
{
    const SpineRibbon* r    = &s->ribbon;
    uint64_t           grid = g_book.atom > TAGFS_BLOCK_SIZE ? g_book.atom / TAGFS_BLOCK_SIZE : 1u;
    if (!r->quire_blocks || r->at < r->quire_block || r->at - r->quire_block >= r->quire_blocks)
        return;
    BookClaim(GROUND_RIBBON, r->quire_block, r->quire_blocks, 0);
    if (!r->next_blocks || (r->next_block == r->quire_block && r->next_blocks == r->quire_blocks))
        return;
    if (r->next_block < g_book.frame.data_block || r->next_block + r->next_blocks > g_book.data_end ||
        r->next_block % grid)
        return;
    if (taken && (GroundClaimedBy(GROUND_TALLY, r->next_block, r->next_blocks) ||
                  GroundClaimedBy(GROUND_FILE, r->next_block, r->next_blocks)))
        return;
    BookClaim(GROUND_RIBBON, r->next_block, r->next_blocks, 0);
}

/* A node of a slot not stood on that neither copy of holds — the root a slot
 * passed over was refused for, or one below a root of any older slot, or a
 * child whose pointer does not read. Its blocks are claimed already, by name
 * (BookGroundNode, before the read): the slot names them. What lies under it
 * no name that reads gives, and such a slot is read for its ground alone
 * (§4.5 п.4) — its rot is not the mount's to say, nor to lock anything by
 * (bookmount.py _Older). */
static error_t BookGroundOlderRot(void* ctx, const TallyPlace* at, const char* why)
{
    (void)ctx;
    (void)at;
    (void)why;
    return OK;
}

/* The slots not stood on: their Tallies' nodes, read for the ground, leaves
 * not opened, nothing said; and their ribbons' Quires. Any refusal — the
 * medium's, memory's — ends the census: ground a slot names and the census
 * could not see would read as no one's. */
static error_t BookGroundOlder(const char** what)
{
    TallySink sink = { .ctx = NULL, .item = NULL, .rotten = BookGroundOlderRot, .node = BookGroundNode,
                       .said = false };
    for (uint32_t i = 0; i < g_book.nslots; i++) {
        const SpineSlot* s = &g_book.slots[i];
        if (s == g_book.stood)
            continue;
        for (uint32_t r = 0; r < s->roots; r++) {
            error_t rc = TallyRead(s->root[r].tree, &s->root[r], false, &sink);
            if (rc != OK) {
                *what = rc == ERR_NO_MEMORY ? "there is no memory to read the Tally of a slot not stood on"
                                            : "its medium would not give up the Tally of a slot not stood on, "
                                              "whose ground the census must see";
                return rc;
            }
        }
        BookGroundRibbon(s, false);
    }
    return OK;
}

/* The front of the volume, less the Spine's slots, and the slots themselves. */
static void BookGroundFront(void)
{
    const VolumeFrame* f  = &g_book.frame;
    uint64_t           at = 0;
    for (uint32_t i = 0; i < f->spine_slots; i++) {
        uint64_t slot = f->spine_block + (uint64_t)i * f->spine_stride;
        if (slot > at)
            BookClaim(GROUND_FRONT, at, slot - at, 0);
        BookClaim(GROUND_SPINE, slot, 1, 0);
        at = slot + 1u;
    }
    if (f->data_block > at)
        BookClaim(GROUND_FRONT, at, f->data_block - at, 0);
    BookClaim(GROUND_FAR_DEED, g_book.data_end, 1, 0);
}

/* The BOOT run the Deed names, as it names it — beside the file the kernel
 * is, never in its place: not an exclusive owner (ground_owner.h). Which
 * live file it lies under is volume-read's to say (bookcensus.py _Boot); the
 * census here only keeps the loaders' blocks from reading as no one's. */
static void BookGroundBoot(void)
{
    const VolumeBoot* boot = &tagfs_get_state()->boot;
    if (boot->kernel_blocks)
        BookClaim(GROUND_BOOT, boot->kernel_block, boot->kernel_blocks, 0);
}

/* Every digested block, and every range a rotten leaf would have digested,
 * held — when a FILES range rots, a block no name owns may be a locked
 * file's (§16.1 п.5). */
static void BookHeldDigested(void* ctx, uint64_t start, uint64_t count)
{
    (void)ctx;
    BookGroundHoldRun(start, count);
}

static void BookHeldRotten(void* ctx, uint64_t start, uint64_t count, bool to_end)
{
    (void)ctx;
    BookGroundHoldRun(start, to_end ? (start < g_book.data_end ? g_book.data_end - start : 0) : count);
}

error_t BookGroundCensus(const char** what)
{
    BookGroundRibbon(g_book.stood, true);
    error_t rc = BookGroundOlder(what);
    if (rc != OK)
        return rc;
    BookGroundFront();
    BookGroundBoot();
    if (g_book.files_rot && (rc = TallyDigestsEach(BookHeldDigested, BookHeldRotten, NULL)) != OK) {
        *what = rc == ERR_NO_MEMORY ? "there is no memory to read the DIGESTS a rotten FILES range is held by"
                                    : "its medium would not give up the DIGESTS a rotten FILES range is held by";
        return rc;
    }
    rc = GroundCensusBook(&g_book.said.held);
    GroundCensusEnd();
    if (rc != OK) {
        *what = "there is no memory to hold its census";
        return rc;
    }
    if (g_book.said.held)
        kprintf("[TagFS] %llu block(s) are held: no name that reads owns them, and a rotten range or a "
                "locked file may — they are given to no one until an explicit act lets them go\n",
                (unsigned long long)g_book.said.held);
    return OK;
}
