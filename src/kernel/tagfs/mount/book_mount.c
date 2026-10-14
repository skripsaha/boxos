#include "book_mount.h"
#include "book_unit.h"
#include "away.h"
#include "ground_census.h"
#include "../block_map/block_map_derived.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "boardroom.h"

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

error_t MountBookGround(uint8_t seat, const DeedCopy* head)
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
// Nothing sewn past the Tally (§4.5 п.5 is a newer kernel's)
// ----------------------------------------------------------------------------

#define BOOK_LOOK_SECTORS  256u

/* Whether a sector of [block, block + blocks) holds as a stitch of a seq past
 * `past`: OK with *sewn, or the medium's or memory's refusal. */
static error_t BookRibbonLook(uint64_t block, uint64_t blocks, uint64_t past, bool* sewn)
{
    uint64_t at = block * TAGFS_BLOCK_SECTORS, end = (block + blocks) * TAGFS_BLOCK_SECTORS;
    while (at < end && !*sewn) {
        BookUnit u;
        uint32_t n  = end - at > BOOK_LOOK_SECTORS ? BOOK_LOOK_SECTORS : (uint32_t)(end - at);
        error_t  rc = BookUnitRead(at, n, 0, 0, &u);
        for (uint32_t k = 0; rc == OK && k < n; k++) {
            BookSectorHead h;
            memcpy(&h, u.sectors + (uint64_t)k * BOOK_SECTOR_BYTES, sizeof(h));
            if (u.state[k] == BOOK_SECTOR_HOLDS && h.kind == BOOK_UNIT_STITCH && h.seq > past)
                *sewn = true;
        }
        BookUnitLetGo(&u);
        if (rc != OK)
            return rc;
        at += n;
    }
    return OK;
}

/* The ribbon from the stood slot's `at` to the end of its Quire, and all of
 * the next_quire it names inside the data run: nothing there may hold as a
 * stitch past the one the Tally took in (bookcensus.py KernelRibbonEmpty). A
 * slot that puts the ribbon outside its own Quire names none to look at — said
 * in volume-read's words (stitch.py Ribbon, RIBBON_TORN). */
static error_t BookRibbonEmpty(const char** what)
{
    const SpineRibbon* r    = &g_book.stood->ribbon;
    uint64_t           past = g_book.stood->head.stitch_seq;
    bool               sewn = false;
    error_t            rc   = OK;
    if (!r->quire_blocks || r->at < r->quire_block || r->at - r->quire_block >= r->quire_blocks) {
        kprintf("[TagFS] slot %u puts the ribbon on at block %llu, outside the Quire %llu+%llu it names — "
                "no stitch past its Tally is read\n", g_book.stood->index, (unsigned long long)r->at,
                (unsigned long long)r->quire_block, (unsigned long long)r->quire_blocks);
        return OK;
    }
    rc = BookRibbonLook(r->at, r->quire_block + r->quire_blocks - r->at, past, &sewn);
    if (rc == OK && !sewn && r->next_blocks &&
        !(r->next_block == r->quire_block && r->next_blocks == r->quire_blocks) &&
        r->next_block >= g_book.frame.data_block && r->next_block + r->next_blocks <= g_book.data_end)
        rc = BookRibbonLook(r->next_block, r->next_blocks, past, &sewn);
    if (rc != OK) {
        *what = rc == ERR_NO_MEMORY ? "there is no memory to look past its Tally"
                                    : "its medium would not give up the ribbon past its Tally";
        return rc;
    }
    if (!sewn)
        return OK;
    *what = "the ribbon past its Tally is not empty — this kernel reads a book's Tally, and not yet the "
            "stitches sewn past it (ERR_UNSUPPORTED)";
    return ERR_UNSUPPORTED;
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
        rc = UseMemoryRecall(NULL, 0);
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
    kprintf("[TagFS] the next file id %llu, tag id %llu, bookmark %llu — past every one the book names\n",
            (unsigned long long)fid, (unsigned long long)tag, (unsigned long long)c->next_bookmark);
    if (fid > UINT32_MAX)
        kprintf("[TagFS] file ids past %u are this book's, and this kernel counts the ids it gives in 32 "
                "bits — it gives none on this volume\n", UINT32_MAX);
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

error_t MountReadBook(const char** what)
{
    error_t rc = BookMemory(what);
    if (rc == OK)
        rc = BookSpine(what);
    if (rc == OK)
        rc = BookStand(what);
    if (rc == OK)
        rc = BookRibbonEmpty(what);
    if (rc != OK)
        return rc;

    TagFSUnderGlass("this kernel reads format-v2 books and writes none of them");
    BookSlotSaid(g_book.stood);
    GroundCensusBegin();
    rc = BookWalk(what);
    if (rc == OK)
        rc = BookGroundCensus(what);
    GroundCensusEnd();
    if (rc != OK)
        return rc;
    BookBounds();
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
    for (uint32_t i = 0; i < g_book.nrot; i++) {
        const BookRotten* r = &g_book.rot[i];
        bool after_lo = fid > r->lo.k1 || (fid == r->lo.k1);
        bool before_hi = !r->has_hi || fid < r->hi.k1 || (fid == r->hi.k1 && r->hi.k2 > 0);
        if (after_lo && before_hi)
            return true;
    }
    return false;
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
