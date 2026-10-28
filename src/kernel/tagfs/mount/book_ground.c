#include "book_mount.h"
#include "ground_census.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "crypto.h"
#include "../settle/binder.h"

/*
 * The census of a format-v2 volume (book_mount.h; spec_v2 §4.6): no bitmap
 * says which blocks are taken — every owner names its ground, and what none
 * names is free. The owners are the kernel's one list (ground_owner.h, those
 * of format v2), the same volume-read's census asks of the same image
 * (tools/volumeread/bookcensus.py, from the generated BOOK_OWNERS):
 *
 *   front     the volume before its data run, the Spine's slots aside
 *   spine     every slot of the ring, whatever it holds
 *   tally     every node of every Tally a slot that holds names, every copy
 *             its pointer names — a rotten one too: its parent names those
 *             blocks
 *   ribbon    the Quires the ribbon of each such slot runs through, and
 *             the next_quire it names where one may be named (ribbon.h)
 *   file      every DATA run of every live file, as the Tally and the
 *             ribbon after it leave it (BookFilesLaid)
 *   boot      the BOOT run, beside the kernel file it is (not exclusive)
 *   far_deed  the far copy of the Deed, the volume's last block
 *   held      what a rotten range or a locked file may own and no name that
 *             reads does — given to no one until an explicit act lets it go
 *   fallow    what the ribbon let go and no other owner names (M-1)
 *
 * The only finding is a block two exclusive owners name (GroundCensusBook) —
 * a node and a Quire named by the slot that names them, so that one name two
 * slots give alike is one owner. What none names is free: the census lays
 * every claim into the volume's block map (block_map_derived.h). The ground
 * the ribbon let go lies fallow (M-1): neither a file's nor free, given to no
 * one until a writer's stitch settles past it — a block a mount reads as let
 * go may have come out of the medium's cache, and its owner's bytes may be
 * what a warm reboot still finds there.
 *
 * One owner of §4.6 this census does not see: a bookmark — the nodes of the
 * trees its element names and the runs under its Lien. This kernel reads no
 * bookmark (such a book stands under glass, book_mount.c BookTree), so where a
 * Tally of a slot that holds names one, the census names its two owners and
 * derives no map: a map of the rest would read free what the bookmark holds,
 * and the mount says the free count is not counted rather than give it high.
 */

/* Ground of the volume `count` blocks long from `start`, claimed by `owner` in
 * runs a claim counts — given by Spine slot `slot`, or by none (UINT32_MAX). */
static void BookClaimBy(GroundOwner owner, uint64_t start, uint64_t count, uint64_t who, uint32_t slot)
{
    while (count) {
        uint32_t n = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
        if (slot == UINT32_MAX)
            GroundClaim(owner, start, n, who);
        else
            GroundClaimBySlot(owner, start, n, who, slot);
        start += n;
        count -= n;
    }
}

static void BookClaim(GroundOwner owner, uint64_t start, uint64_t count, uint64_t who)
{
    BookClaimBy(owner, start, count, who, UINT32_MAX);
}

/* Every copy a node's pointer names (§16.2 п.17), as slot `slot` names it: a
 * node by its seq. */
static void BookGroundNodeOf(const TallyPlace* at, uint32_t slot)
{
    for (uint32_t k = 0; k < at->ptr.copies; k++) {
        BookClaimBy(GROUND_TALLY, TallyPtrBlock(&at->ptr, k), at->ptr.sheets, at->ptr.seq, slot);
        if (BinderSlotNode(slot, TallyPtrBlock(&at->ptr, k), at->ptr.sheets, at->ptr.seq) != OK)
            g_book.binder_short = true;
    }
    if (g_book.stood && slot == g_book.stood->index) {
        g_book.tally_blocks += at->ptr.sheets;
        EndpaperLaidNode(&g_book.laid, g_book.laid_tree, at->level, at->ptr.sheets,
                         g_book.atom > TAGFS_BLOCK_SIZE ? g_book.atom / TAGFS_BLOCK_SIZE : 1u,
                         at->has_lo ? at->lo.k1 : 0);
    }
}

error_t BookGroundNode(void* ctx, const TallyPlace* at)
{
    (void)ctx;
    BookGroundNodeOf(at, g_book.stood->index);
    return OK;
}

/* A node of a slot not stood on, `ctx` the slot. */
static error_t BookGroundOlderNode(void* ctx, const TallyPlace* at)
{
    BookGroundNodeOf(at, ((const SpineSlot*)ctx)->index);
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

/* The Quires a slot's ribbon runs through (ribbon.h — RibbonCollate for the
 * slot stood on, RibbonQuiresOf for another): its Quire when its `at` lies in
 * it, every Quire its stitches go on into, and the next_quire it names where
 * one may be named — as that slot names them. */
static void BookGroundRibbon(const SpineSlot* s, const RibbonWalk* w)
{
    for (uint32_t i = 0; i < w->nquires; i++) {
        BookClaimBy(GROUND_RIBBON, w->quires[i].block, w->quires[i].blocks, 0, s->index);
        if (BinderSlotRibbon(s->index, &w->quires[i]) != OK)
            g_book.binder_short = true;
    }
}

/* A node of a slot not stood on that neither copy of holds — the root a slot
 * passed over was refused for, or one below a root of any older slot, or a
 * child whose pointer does not read. Its blocks are claimed already, by name
 * (BookGroundOlderNode, before the read): the slot names them. What lies under it
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
 * not opened, nothing said; and their ribbons' Quires, walked by their
 * StitchHeads alone (§16.2 п.12, ribbon_heads.c — volume-read walks them
 * whole, §16.3 п.13, and the census holds the two to one answer). Any
 * refusal — the medium's, memory's — ends the census: ground a slot names
 * and the census could not see would read as no one's. */
static error_t BookGroundOlder(const char** what)
{
    for (uint32_t i = 0; i < g_book.nslots; i++) {
        const SpineSlot* s    = &g_book.slots[i];
        RibbonWalk*      w    = NULL;
        TallySink        sink = { .ctx = (void*)s, .item = NULL, .rotten = BookGroundOlderRot,
                                  .node = BookGroundOlderNode, .said = false };
        error_t          rc   = OK;
        if (s == g_book.stood)
            continue;
        for (uint32_t r = 0; r < s->roots && rc == OK; r++)
            rc = TallyRead(s->root[r].tree, &s->root[r], false, &sink);
        if (rc == OK && (rc = RibbonQuiresOf(s, &w)) == OK)
            BookGroundRibbon(s, w);
        RibbonWalkLetGo(w);
        if (rc != OK) {
            *what = rc == ERR_NO_MEMORY ? "there is no memory to read the Tally or the ribbon of a slot not stood on"
                                        : "its medium would not give up the Tally or the ribbon of a slot not stood "
                                          "on, whose ground the census must see";
            return rc;
        }
    }
    return OK;
}

/* The ground the ribbon let go, fallow (M-1): neither a file's nor free —
 * what of it no other owner names is the census's to cut (GroundCensusBook). */
static void BookGroundFallow(void)
{
    for (uint64_t i = 0; i < g_book.nfallow; i++) {
        uint64_t start = g_book.fallow[i].start, end = start + g_book.fallow[i].count;
        if (end > g_book.data_end + 1u)
            end = g_book.data_end + 1u;
        if (start < end)
            BookClaim(GROUND_FALLOW, start, end - start, 0);
    }
}

/* A run of the print (MirrorRuns): a hole, or DATA from `start`, `blocks`
 * long — the Strips that go on from one another are one. */
typedef struct __attribute__((packed)) {
    uint8_t  hole;
    uint64_t start;
    uint64_t blocks;
} BookPrintRun;

/*
 * One live file into the print of what memory holds (tools/volumeread/
 * bookcensus.py MirrorPrint): le64 fid · le64 size · le32 name bytes · the
 * name · le32 tags · le32 each tag, rising · each run as u8 hole · le64 start
 * · le64 blocks, the Strips that go on from one another one run · le32 runs.
 */
static uint32_t BookFilePrinted(uint32_t crc, const FileRecord* rec)
{
    struct __attribute__((packed)) {
        uint64_t fid;
        uint64_t size;
        uint32_t name;
    } head = { rec->file_id, rec->size, rec->name_len };
    uint32_t     ntags = rec->tag_count, nruns = 0;
    BookPrintRun run   = { 0, 0, 0 };
    StripWalk    walk;
    Strip        s;
    crc = KCrc32cContinue(crc, (const uint8_t*)&head, sizeof(head));
    crc = KCrc32cContinue(crc, (const uint8_t*)rec->name, rec->name_len);
    crc = KCrc32cContinue(crc, (const uint8_t*)&ntags, sizeof(ntags));
    crc = KCrc32cContinue(crc, (const uint8_t*)rec->tags, sizeof(uint32_t) * ntags);
    StripWalkBegin(rec, &walk);
    for (bool more = StripWalkNext(&walk, &s);; more = StripWalkNext(&walk, &s)) {
        uint8_t hole = more && s.kind == STRIP_HOLE;
        if (more && run.blocks && run.hole == hole && (hole || run.start + run.blocks == s.start)) {
            run.blocks += s.count;
            continue;
        }
        if (run.blocks) {
            crc = KCrc32cContinue(crc, (const uint8_t*)&run, sizeof(run));
            nruns++;
        }
        if (!more)
            break;
        run = (BookPrintRun){ hole, hole ? 0 : s.start, s.count };
    }
    return KCrc32cContinue(crc, (const uint8_t*)&nruns, sizeof(nruns));
}

/* Every live file's ground claimed, the print of them taken, and the file the
 * loader reads the kernel by named once, so a change of a book's file asks
 * the guard of its id and not of every run it has (ink.c, InkMayChange). */
void BookFilesLaid(void)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, ids[32];
    uint32_t    n, crc = 0;
    while ((n = FileMapRows(fs->files, from, ids, 32)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec = FileMapRecordHold(fs->files, ids[i], NULL);
            StripWalk   walk;
            Strip       run;
            if (!rec)
                continue;
            StripWalkBegin(rec, &walk);
            while (StripWalkNextGround(&walk, &run)) {
                GroundClaim(GROUND_FILE, run.start, run.count, ids[i]);
                if (!g_book.boot_fid && InkTouchesBoot(&run, 1))
                    g_book.boot_fid = ids[i];
            }
            for (uint32_t t = 0; t < rec->tag_count; t++)
                if ((uint64_t)rec->tags[t] + 1u > g_book.next_tag)
                    g_book.next_tag = (uint64_t)rec->tags[t] + 1u;
            crc = BookFilePrinted(crc, rec);
            FileRecordRelease(rec);
        }
        from = ids[n - 1] + 1;
    }
    g_book.files_print = crc;
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

/* Whether the Tally of a slot that holds names a bookmark: a BOOKMARKS or a
 * LIEN tree that is not empty (volume-read's census asks the same,
 * bookcensus.py BookmarksNamed). */
static bool BookBookmarked(void)
{
    for (uint32_t i = 0; i < g_book.nslots; i++)
        for (uint32_t r = 0; r < g_book.slots[i].roots; r++) {
            const SpineRoot* root = &g_book.slots[i].root[r];
            if ((root->tree == TALLY_BOOKMARKS || root->tree == TALLY_LIEN) && root->items)
                return true;
        }
    return false;
}

/* The ground the ribbon let go, as the census leaves it — what of it no other
 * owner names — kept for the mount's Fallow (ink.c, InkMountReset): a block of
 * it another owner names is that owner's, and never given back for the
 * ribbon's word. ERR_NO_MEMORY. */
static error_t BookFallowCut(const char** what)
{
    uint64_t n    = GroundClaimsOf(GROUND_FALLOW, NULL, 0);
    Strip*   runs = n ? kmalloc(sizeof(Strip) * n) : NULL;
    if (n && !runs) {
        *what = "there is no memory to keep the ground its ribbon let go";
        return ERR_NO_MEMORY;
    }
    (void)GroundClaimsOf(GROUND_FALLOW, runs, n);
    kfree(g_book.fallow);
    g_book.fallow    = runs;
    g_book.nfallow   = n;
    g_book.capfallow = n;
    return OK;
}

/* The ground the census holds — what a rotten range or a locked file may own
 * and no name that reads does — kept for the writer of a Tally, whose DIGESTS
 * names its digests again so it stays held (settle/binder.h). */
static void BookHeldKept(void)
{
    uint64_t n    = GroundClaimsOf(GROUND_HELD, NULL, 0);
    Strip*   runs = n ? kmalloc(sizeof(Strip) * n) : NULL;
    if (n && !runs) {
        g_book.binder_short = true;
        return;
    }
    (void)GroundClaimsOf(GROUND_HELD, runs, n);
    for (uint64_t i = 0; i < n; i++)
        if (BinderHeldRun(runs[i].start, runs[i].count) != OK)
            g_book.binder_short = true;
    kfree(runs);
}

error_t BookGroundCensus(const char** what)
{
    BookGroundRibbon(g_book.stood, g_book.ribbon);
    BookGroundFallow();
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
    g_book.bookmarked = BookBookmarked();
    rc = GroundCensusBook(&g_book.said.held, &g_book.said.fallow, !g_book.bookmarked, what);
    if (rc == OK)
        rc = BookFallowCut(what);
    if (rc == OK)
        BookHeldKept();
    GroundCensusEnd();
    if (rc != OK)
        return rc;
    if (g_book.said.fallow)
        kprintf("[TagFS] %llu block(s) the ribbon let go lie fallow: neither a file's nor free — given to no one "
                "until a stitch of this volume's writer settles past them (M-1, §3.4)\n",
                (unsigned long long)g_book.said.fallow);
    if (g_book.said.held)
        kprintf("[TagFS] %llu block(s) are held: no name that reads owns them, and a rotten range or a "
                "locked file may — they are given to no one until an explicit act lets them go\n",
                (unsigned long long)g_book.said.held);
    return OK;
}
