#include "book_mount.h"
#include "../settle/binder.h"
#include "book_unit.h"
#include "away.h"
#include "ground_census.h"
#include "../block_map/block_map_derived.h"
#include "../integrity/gloss.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../settle/sewing.h"
#include "boardroom.h"
#include "crypto.h"

/*
 * A format-v2 volume's mount (book_mount.h): the ground, the Spine, the slot
 * stood on, the walk of its Tally, the census and the numbers given next.
 */

BookMount g_book;

/* The refusal the mount's line carries, made here when it names numbers. */
static char s_refusal[320];

// ----------------------------------------------------------------------------
// The ground (§1.2, §1.5)
// ----------------------------------------------------------------------------

/*
 * ‼ THE ATOM IS THE MEDIUM'S, AND WHERE ITS BLOCKS BEGIN IS TOO. A write of
 * this format is cut to A = the largest of 4096, the grid the volume was laid
 * out for and the physical block the medium says it is built from; and the
 * medium's blocks begin at L0, which IDENTIFY word 209 or READ CAPACITY(16)
 * says (BoardroomSeatGrid). Ground that does not begin on one of them makes
 * every write rewrite its neighbours — on flash, read, patch and write a
 * block for each — so a writer would not take it, and this kernel reads it
 * under glass (§1.5). QEMU says L0 = 0 and 512-byte blocks: the vector the
 * predicate is held to lives in the startup suite (book, ata_first_aligned).
 */
static void BookGroundAtom(uint8_t seat)
{
    uint32_t physical = 0;
    uint64_t first    = 0;
    uint64_t base     = tagfs_get_volume_base();
    uint32_t atom     = TAGFS_BLOCK_SIZE;
    char     words[224];

    BoardroomSeatGrid(seat, &physical, &first);
    if (tagfs_get_state()->geometry.physical_bytes > atom)
        atom = tagfs_get_state()->geometry.physical_bytes;
    if (physical > atom)
        atom = physical;
    g_book.atom = atom;
    if (BookGroundOnAtom(base, first, atom))
        return;
    ksnprintf(words, sizeof(words), "its ground begins at sector %llu, not on the medium's physical block "
              "of %u bytes, which begin at sector %llu: every write would rewrite its neighbours (§1.5) — "
              "repartition the medium", (unsigned long long)base, atom, (unsigned long long)first);
    TagFSUnderGlass(words);
}

error_t MountBookGround(uint8_t seat, const DeedCopy* head, uint64_t ground_sectors)
{
    TagFSState* fs    = tagfs_get_state();
    uint16_t    bytes = 0;
    const void* frame = DeedStamp(head, VOLUME_STAMP_FRAME, &bytes);
    char        words[160];

    MountBookLetGo();
    memset(&fs->layout, 0, sizeof(fs->layout));      /* format v1's alone: never filled here */
    memset(&fs->ledger, 0, sizeof(fs->ledger));      /* and its Ledger: only the numbers given next
                                                      * (BookBounds) and the free count its block
                                                      * map derives are kept in it — never a count
                                                      * of a volume mounted before */
    memcpy(&g_book.frame, frame, sizeof(VolumeFrame));
    g_book.data_end = head->head.tail_sector / VOLUME_BLOCK_SECTORS;
    g_book.ground   = ground_sectors;
    g_book.on_head  = head->head.role == VOLUME_DEED_ROLE_HEAD;
    fs->block_base  = 0;
    fs->block_end   = g_book.data_end;
    if (bytes > sizeof(VolumeFrame)) {
        ksnprintf(words, sizeof(words), "its FRAME is %u bytes, longer than the %u this reader knows — "
                  "written by a kernel that knows more (§1.2)", bytes, (unsigned)sizeof(VolumeFrame));
        TagFSUnderGlass(words);
    }
    BookGroundAtom(seat);
    return OK;
}

// ----------------------------------------------------------------------------
// The slot stood on (§1.4, §4.5 п.2)
// ----------------------------------------------------------------------------

/* Whether `tree` is one this reader reads as it is written — not one a reader
 * counts again from the rest (MAP, SHARED, or a root that says so). */
static bool BookTreeAuthoritative(const SpineRoot* r)
{
    return r->tree >= TALLY_FILES && r->tree <= TALLY_DIGESTS && !(r->flags & TALLY_ROOT_DERIVED);
}

/* Whether every authoritative root `s` names reads: OK, ERR_TAGFS_CORRUPTED
 * with why, or the medium's or memory's refusal. */
static error_t BookSlotReads(const SpineSlot* s, char* why, size_t room)
{
    char* words = kmalloc(512);
    if (!words)
        return ERR_NO_MEMORY;
    error_t rc = OK;
    for (uint32_t i = 0; i < s->roots && rc == OK; i++) {
        if (!BookTreeAuthoritative(&s->root[i]))
            continue;
        rc = TallyRootReads(s->root[i].tree, &s->root[i], words, 512);
        if (rc == ERR_TAGFS_CORRUPTED)
            ksnprintf(why, room, "its %s root will not read — %s", TallyTreeName(s->root[i].tree), words);
    }
    kfree(words);
    return rc;
}

/* The newest slot that holds and names a Tally whose roots read: each passed
 * over said. OK, or the refusal. */
static error_t BookStand(const char** what)
{
    char why[256];
    for (uint32_t i = 0; i < g_book.nslots; i++) {
        SpineSlot* s  = &g_book.slots[i];
        error_t    rc = BookSlotReads(s, why, sizeof(why));
        if (rc == OK) {
            g_book.stood      = s;
            g_book.said.slot  = s->index;
            kprintf("[TagFS] the book stands on slot %u (seq %llu): its Tally of seq %llu, which took in the "
                    "ribbon to stitch seq %llu\n", s->index, (unsigned long long)s->seq,
                    (unsigned long long)s->head.tally_seq, (unsigned long long)s->head.stitch_seq);
            return OK;
        }
        if (rc != ERR_TAGFS_CORRUPTED) {
            *what = rc == ERR_NO_MEMORY ? "there is no memory to read its Tally"
                                        : "its medium would not give up its Tally";
            return rc;
        }
        kprintf("[TagFS] slot %u (seq %llu) names a Tally that does not read: %s — the next slot is read\n",
                s->index, (unsigned long long)s->seq, why);
    }
    *what = "no slot that holds names a Tally that reads (ERR_TAGFS_CORRUPTED)";
    return ERR_TAGFS_CORRUPTED;
}

/* The slot's own word against the volume: what it carries that this kernel
 * cannot carry on, and a lead its records do not give a sector. */
static void BookSlotSaid(const SpineSlot* s)
{
    for (uint32_t i = 0; i < s->nglass; i++)
        TagFSUnderGlass(s->glass[i]);
    if (s->shape[0]) {
        kprintf("[TagFS] the book is written against its own shape — %s\n", s->shape);
        g_book.said.shapes++;
    }
}

// ----------------------------------------------------------------------------
// The ribbon past the Tally (§4.5 пп.5–6)
// ----------------------------------------------------------------------------

/* The ribbon from the stood slot's `at`: pass 1, the data of its newest
 * stitch, pass 2 — and what the mount says of it in numbers; then the writer's
 * cursor laid where the walk ended (settle/sewing.h), or the reason no writer
 * goes on from there, under glass. */
static error_t BookRibbon(const char** what)
{
    const char* glass = NULL;
    error_t     rc    = RibbonCollate(g_book.stood, &g_book.ribbon);
    if (rc != OK) {
        *what = rc == ERR_NO_MEMORY ? "there is no memory to read the ribbon past its Tally"
                                    : "its medium would not give up the ribbon past its Tally";
        return rc;
    }
    RibbonWalk* w = g_book.ribbon;
    rc = RibbonTranscribe(w, what);
    if (rc != OK)
        return rc;
    g_book.said.sewn  = w->nstitch;
    for (uint32_t i = 0; i < w->nstitch; i++)
        g_book.ribbon_useful += w->stitch[i]->useful;
    g_book.said.last  = w->nstitch ? w->stitch[w->nstitch - 1u]->seq : g_book.stood->head.stitch_seq;
    g_book.said.lied  = w->lied;
    g_book.said.flags = w->nstitch && w->stitch[w->nstitch - 1u]->head_read ? w->stitch[w->nstitch - 1u]->head.flags
                                                                           : 0;
    kprintf("[TagFS] the ribbon past the Tally: %u stitch(es) sewn on, the last seq %llu, %u applied\n",
            w->nstitch, (unsigned long long)g_book.said.last, g_book.said.applied);
    if (g_book.said.flags & STITCH_CLEAN)
        kprintf("[TagFS] the last stitch of the ribbon says CLEAN: the machine that sewed it stopped with no change "
                "in flight\n");
    if (w->lied)
        kprintf("[TagFS] MEDIUM_LIED: this volume's medium has not kept what it confirmed — a Fallow lags two "
                "stitches on it, and the newest two are held to their data at a mount (§3.9); every stitch this "
                "kernel sews on it says so\n");
    if (RibbonCursorMount(w, &glass) != OK)
        TagFSUnderGlass(glass);
    if (w->seen_fid > g_book.next_fid)
        g_book.next_fid = w->seen_fid;
    if (w->seen_tag > g_book.next_tag)
        g_book.next_tag = w->seen_tag;
    g_book.next_bookmark = w->seen_bookmark;
    return OK;
}

// ----------------------------------------------------------------------------
// The walk of the Tally stood on (§4.5 п.3)
// ----------------------------------------------------------------------------

/* DIGESTS at the mount: its inner nodes read, each leaf kept by the range of
 * blocks it covers, read when a block of it is (§8.1, TallyDigestFind). */
static error_t BookDigestNode(void* ctx, const TallyPlace* at)
{
    BookGroundNode(ctx, at);
    return at->level == 0 && at->ptr.copies ? TallyDigestsKeep(at, false) : OK;
}

static error_t BookDigestRot(void* ctx, const TallyPlace* at, const char* why)
{
    (void)ctx;
    (void)why;
    if (at->level > 0)
        EndpaperLaidNode(&g_book.laid, TALLY_DIGESTS, at->level, 0, 1u, at->has_lo ? at->lo.k1 : 0);
    return TallyDigestsKeep(at, true);
}

static error_t BookQuietRot(void* ctx, const TallyPlace* at, const char* why)
{
    (void)ctx;
    (void)at;
    (void)why;
    return OK;
}

/* One root of the slot stood on, walked as its tree is read. */
static error_t BookTree(const SpineRoot* root)
{
    char      words[192];
    TallySink sink = { .ctx = NULL, .item = NULL, .node = BookGroundNode, .said = true,
                       .mended = &g_book.said.mended, .shapes = &g_book.said.shapes };
    bool derived = root->tree == TALLY_MAP || root->tree == TALLY_SHARED || (root->flags & TALLY_ROOT_DERIVED);

    g_book.laid_tree = root->tree;

    if (!derived && !BookTreeAuthoritative(root)) {
        ksnprintf(words, sizeof(words), "slot %u names a tree of kind %llu this reader does not know, and "
                  "not one to derive — written by a kernel that knows more", g_book.stood->index,
                  (unsigned long long)root->tree);
        TagFSUnderGlass(words);
    }
    if (!BookTreeAuthoritative(root)) {
        sink.rotten = BookQuietRot;
        sink.said   = false;
        return TallyRead(root->tree, root, false, &sink);
    }
    if ((root->tree == TALLY_BOOKMARKS || root->tree == TALLY_LIEN) && root->items) {
        ksnprintf(words, sizeof(words), "its %s tree is not empty — a bookmark this kernel cannot hold (§5)",
                  TallyTreeName(root->tree));
        TagFSUnderGlass(words);
    }
    if (root->tree != TALLY_DIGESTS)
        return TallyLeaves(root->tree, root);
    sink.node   = BookDigestNode;
    sink.rotten = BookDigestRot;
    return TallyRead(TALLY_DIGESTS, root, false, &sink);
}

/* Every root of the slot stood on, in the order of its trees. A slot that
 * names no USE root holds an empty Use Context — a writer leaves the root out
 * when there is none (§16.3 п.5) — and that, too, is what the medium
 * remembers: recalled, so a Remember of nothing is the nothing that settled
 * (§7). */
static error_t BookWalk(const char** what)
{
    const SpineSlot* s    = g_book.stood;
    uint64_t         last = 0;
    bool             use  = false;
    error_t          rc   = OK;
    for (uint32_t done = 0; done < s->roots && rc == OK; done++) {
        const SpineRoot* next = NULL;
        for (uint32_t i = 0; i < s->roots; i++)
            if (s->root[i].tree > last && (!next || s->root[i].tree < next->tree))
                next = &s->root[i];
        if (!next)
            break;
        last = next->tree;
        use |= next->tree == TALLY_USE;
        rc   = BookTree(next);
    }
    if (rc == OK && !use)
        rc = BookUseRecalled(NULL, 0);
    if (rc != OK)
        *what = rc == ERR_NO_MEMORY ? "there is no memory to hold what its Tally names"
                                    : "its medium would not give up its Tally";
    return rc;
}

// ----------------------------------------------------------------------------
// The numbers given next (§4.5 п.8)
// ----------------------------------------------------------------------------

static void BookBounds(void)
{
    TagFSState*        fs = tagfs_get_state();
    const SpineCounts* c  = &g_book.stood->counts;
    uint64_t           known_file = 0;
    uint32_t           known_tag  = 0;
    uint64_t           fid = g_book.next_fid > c->next_fid ? g_book.next_fid : c->next_fid;
    uint64_t           tag = g_book.next_tag > c->next_tag ? g_book.next_tag : c->next_tag;

    AwayNumbersKnown(fs->uuid, &known_file, &known_tag);
    if (known_file > fid)
        fid = known_file;
    if (known_tag > tag)
        tag = known_tag;
    uint64_t bookmark = g_book.next_bookmark > c->next_bookmark ? g_book.next_bookmark : c->next_bookmark;
    kprintf("[TagFS] the next file id %llu, tag id %llu, bookmark %llu, seq %llu — past every one the book "
            "names\n", (unsigned long long)fid, (unsigned long long)tag, (unsigned long long)bookmark,
            (unsigned long long)(BookSeqSeen() + 1u));
    g_book.next_fid         = fid;
    g_book.next_tag         = tag;
    g_book.next_bookmark    = bookmark;
    fs->ledger.next_file_id = fid > UINT32_MAX ? UINT32_MAX : (uint32_t)fid;
    fs->ledger.total_files  = g_book.files;
    TagRegistryNumbersFrom(fs->registry, tag > (uint64_t)TAGFS_MAX_TAG_ID + 1u ? TAGFS_MAX_TAG_ID + 1u
                                                                             : (uint32_t)tag);
}

// ----------------------------------------------------------------------------
// The whole read
// ----------------------------------------------------------------------------

/* What every format's mount fills: the registry, the map of live files, the
 * tag index. */
static error_t BookMemory(const char** what)
{
    TagFSState* fs = tagfs_get_state();
    fs->braid      = NULL;
    fs->registry   = kmalloc(sizeof(TagRegistry));
    if (!fs->registry || tag_registry_init(fs->registry) != OK) {
        kfree(fs->registry);
        fs->registry = NULL;
        *what        = "there is no memory for its tag registry";
        return ERR_NO_MEMORY;
    }
    fs->files        = FileMapCreate();
    fs->bitmap_index = fs->files ? tag_bitmap_create() : NULL;
    if (!fs->files || !fs->bitmap_index) {
        *what = "there is no memory for its map of live files and its tag index";
        return ERR_NO_MEMORY;
    }
    return OK;
}

/* The Spine read: every slot that holds, newest first. */
static error_t BookSpine(const char** what)
{
    char*   states = kmalloc(256);
    error_t rc     = states ? SpineRead(&g_book.frame, &g_book.slots, &g_book.nslots, states, 256)
                            : ERR_NO_MEMORY;
    if (rc == OK && g_book.nslots == 0) {
        ksnprintf(s_refusal, sizeof(s_refusal), "none of the %u slots of its Spine holds (%s) — the book is "
                  "not read (ERR_TAGFS_CORRUPTED)", g_book.frame.spine_slots, states);
        *what = s_refusal;
        rc    = ERR_TAGFS_CORRUPTED;
    } else if (rc != OK) {
        *what = rc == ERR_NO_MEMORY ? "there is no memory to read its Spine"
                                    : "its medium would not give up its Spine";
    }
    kfree(states);
    return rc;
}

/* What the slots hold, for the writer of a Tally (settle/binder.h): the ring
 * as its FRAME names it, and each slot that holds, by its seq and its
 * Tally's — their nodes and ribbons come with the census. */
static error_t BookSlotsHeld(const char** what)
{
    if (BinderSlotsMake(g_book.frame.spine_slots) != OK) {
        *what = "there is no memory for what the slots of its Spine hold";
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < g_book.nslots; i++)
        if (BinderSlotHolds(g_book.slots[i].index, g_book.slots[i].seq, g_book.slots[i].head.tally_seq) != OK) {
            *what = "its Spine read a slot past the ring its FRAME names";
            return ERR_TAGFS_CORRUPTED;
        }
    return OK;
}

error_t MountReadBook(const char** what)
{
    BookSeqSeenForget();
    BinderMountReset();
    error_t rc = BookMemory(what);
    if (rc == OK)
        rc = BookSpine(what);
    if (rc == OK)
        rc = BookSlotsHeld(what);
    if (rc == OK)
        rc = BookStand(what);
    if (rc != OK)
        return rc;

    BookSlotSaid(g_book.stood);
    GroundCensusBegin();
    rc = BookWalk(what);
    if (rc == OK)
        rc = BookRibbon(what);
    if (rc == OK) {
        BookFilesLaid();
        rc = BookGroundCensus(what);
    }
    RibbonWalkLetGo(g_book.ribbon);
    g_book.ribbon = NULL;
    GroundCensusEnd();
    if (rc != OK)
        return rc;
    if (g_book.bookmarked)
        TagFSUnderGlass("a Tally of its book names a bookmark, whose ground this kernel does not derive — its "
                        "writer takes no ground without the map of it");
    BookBounds();
    MountBookGrow(tagfs_get_seat());
    BinderMountStood(g_book.stood->index, g_book.tally_blocks, g_book.ribbon_useful, g_book.next_fid,
                     g_book.next_tag, g_book.next_bookmark);
    EndpaperStood(&g_book.laid, g_book.stood->head.tally_seq, g_book.stood->counts.next_fid);
    kprintf("[TagFS] the book as memory holds it: its files print 0x%08X, its Use Context %llu byte(s) print "
            "0x%08X\n", g_book.files_print, (unsigned long long)g_book.use_bytes, g_book.use_print);
    if (g_book.bookmarked)
        kprintf("[TagFS] %llu data blocks, free not counted, %llu files — a Tally of the book names a bookmark, "
                "whose ground this kernel does not read: no block is counted free, and none is given\n",
                (unsigned long long)(g_book.data_end - g_book.frame.data_block), (unsigned long long)g_book.files);
    else
        kprintf("[TagFS] %llu data blocks, %llu free, %llu files\n",
                (unsigned long long)(g_book.data_end - g_book.frame.data_block),
                (unsigned long long)tagfs_get_state()->ledger.free_blocks, (unsigned long long)g_book.files);
    return OK;
}

// ----------------------------------------------------------------------------
// Locked files, and letting go
// ----------------------------------------------------------------------------

bool BookFileLocked(uint64_t fid)
{
    if (tagfs_get_state()->format != DEED_FORMAT_V2)
        return false;
    uint32_t lo = 0, hi = g_book.nlocked;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g_book.locked[mid] == fid)
            return true;
        if (g_book.locked[mid] < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    FileRecord* live = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    FileRecordRelease(live);
    return !live && BookRotLocks(fid);
}

/* Whether a rotten range of FILES holds `fid`. */
static bool BookInRot(uint64_t fid)
{
    for (uint32_t i = 0; i < g_book.nrot; i++) {
        const BookRotten* r = &g_book.rot[i];
        if (fid >= r->lo.k1 && (!r->has_hi || fid < r->hi.k1 || (fid == r->hi.k1 && r->hi.k2 > 0)))
            return true;
    }
    return false;
}

bool BookRotLocks(uint64_t fid)
{
    uint32_t lo = 0, hi = g_book.nborn;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g_book.born[mid] == fid)
            return false;
        if (g_book.born[mid] < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return BookInRot(fid);
}

error_t BookBornNoted(uint64_t fid)
{
    uint32_t lo = 0, hi = g_book.nborn;
    if (!BookInRot(fid))
        return OK;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g_book.born[mid] < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo < g_book.nborn && g_book.born[lo] == fid)
        return OK;
    if (g_book.nborn == g_book.capborn) {
        uint32_t  cap  = g_book.capborn ? g_book.capborn * 2u : 16u;
        uint64_t* more = kmalloc(sizeof(uint64_t) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (g_book.nborn)
            memcpy(more, g_book.born, sizeof(uint64_t) * g_book.nborn);
        kfree(g_book.born);
        g_book.born    = more;
        g_book.capborn = cap;
    }
    memmove(&g_book.born[lo + 1u], &g_book.born[lo], sizeof(uint64_t) * (g_book.nborn - lo));
    g_book.born[lo] = fid;
    g_book.nborn++;
    return OK;
}

error_t BookUseRecalled(const uint8_t* text, uint64_t len)
{
    error_t rc = UseMemoryRecall(text, len);
    if (rc == OK) {
        g_book.use_print = len ? KCrc32c(text, (uint32_t)len) : 0;
        g_book.use_bytes = len;
    }
    return rc;
}

bool TagFSFileLocked(uint64_t fid)
{
    if (!tagfs_enter())
        return false;
    bool locked = BookFileLocked(fid);
    tagfs_leave();
    return locked;
}

void MountBookLetGo(void)
{
    BookFile* f = &g_book.file;
    kfree(f->meta.filename);
    kfree(f->meta.tag_ids);
    kfree(f->strips);
    SpineLetGo(g_book.slots, g_book.nslots);
    kfree(g_book.rot);
    kfree(g_book.locked);
    for (uint32_t i = 0; i < g_book.nkept; i++) {
        BookKeptFile* k = &g_book.kept[i];
        for (uint32_t p = 0; p < k->nparcels; p++)
            kfree(k->parcel_body[p]);
        kfree(k->meta.filename);
        kfree(k->meta.tag_ids);
        kfree(k->parcel_b);
        kfree(k->parcel_body);
        kfree(k->parcel_len);
        kfree(k->parcel_seq);
    }
    kfree(g_book.kept);
    kfree(g_book.fallow);
    kfree(g_book.born);
    for (uint32_t i = 0; i < g_book.naside; i++)
        kfree(g_book.aside[i].text);
    kfree(g_book.aside);
    RibbonWalkLetGo(g_book.ribbon);
    RibbonCursorLetGo();
    BinderMountReset();
    GlossLetGo();
    TallyDigestsLetGo();
    BlockMapDerivedLetGo();
    memset(&g_book, 0, sizeof(g_book));
}

#ifdef CONFIG_VOLUME_TESTS
void MountBookStandSaid(MountBookSaid* out)
{
    *out = g_book.said;
}
#endif
