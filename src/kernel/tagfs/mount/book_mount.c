#include "book_mount.h"
#include "../ink/commonhold.h"
#include "facsimile.h"
#include "foxing.h"
#include "../settle/binder.h"
#include "book_unit.h"
#include "away.h"
#include "ground_census.h"
#include "../block_map/block_map_derived.h"
#include "../integrity/gloss.h"
#include "../integrity/integrity.h"
#include "../dedup/dedup.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../settle/sewing.h"
#include "under_glass.h"
#include "../settle/escheat.h"
#include "../settle/foliation.h"
#include "boardroom.h"
#include "crypto.h"
#include "errata.h"
#include "read_pace.h"
#include "logbook.h"
#include "tagfs_volume.h"

/*
 * The volume's mount (book_mount.h): the ground, the Spine, the slot
 * stood on, the walk of its Tally, the census and the numbers given next.
 */

// ----------------------------------------------------------------------------
// The ground (§1.2, §1.5)
// ----------------------------------------------------------------------------

/*
 * The ribbon's atom (volume_book.h BookRibbonAtomOf): a flash page on a
 * removable medium — a seat the Boardroom calls so, every USB stick: a stick
 * that answers INQUIRY with RMB clear (one sold as a fixed disk) is flash
 * behind the same bridge — and the medium's atom on any other. A page is laid
 * on the medium's grid only where the ground begins on one; where it does not,
 * the atom stays the medium's, said.
 */
static void BookRibbonAtom(TagFSVolume* vol, uint8_t seat, uint64_t base, uint64_t first)
{
    uint32_t page = BookRibbonAtomOf(vol->book.atom, BoardroomSeatIsRemovable(seat));
    vol->book.ribbon_atom = vol->book.atom;
    if (page == vol->book.atom)
        return;
    if (!BookGroundOnAtom(base, first, page)) {
        kprintf("[TagFS] seat %u is a removable medium, and its ribbon keeps the medium's %u-byte atom: its ground "
                "begins at sector %llu, off a flash page of %u bytes, and a cut while a stitch is written may spoil "
                "the settled stitch beside it (§16.6 п.2) — lay the ground on a mebibyte\n",
                seat, vol->book.atom, (unsigned long long)base, page);
        return;
    }
    vol->book.ribbon_atom = page;
    kprintf("[TagFS] seat %u is a removable medium: its ribbon is written in atoms of a flash page, %u bytes — the "
            "Quires it takes laid on them, each stitch ending on one, so the one after it begins on a page of its "
            "own (§16.6 п.2)\n", seat, page);
}

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
 * A medium addressed in larger blocks than 512 — 4096 on 4Kn — takes whole
 * ones only (boardroom.h), and A is never smaller than the largest block the
 * room seats: ground on an atom is ground every write of the book covers
 * whole blocks of.
 */
STATIC_ASSERT(BOARDROOM_LOGICAL_MOST <= TAGFS_BLOCK_SIZE,
              "every atom of the book covers whole blocks of any medium the room seats");

static void BookGroundAtom(TagFSVolume* vol, uint8_t seat)
{
    uint32_t physical = 0;
    uint64_t first    = 0;
    uint64_t base     = tagfs_get_volume_base(vol);
    uint32_t atom     = TAGFS_BLOCK_SIZE;
    char     words[224];

    BoardroomSeatGrid(seat, &physical, &first);
    if (vol->state.geometry.physical_bytes > atom)
        atom = vol->state.geometry.physical_bytes;
    if (physical > atom)
        atom = physical;
    vol->book.atom = atom;
    if (BookGroundOnAtom(base, first, atom)) {
        BookRibbonAtom(vol, seat, base, first);
        return;
    }
    vol->book.ribbon_atom = atom;
    ksnprintf(words, sizeof(words), "its ground begins at sector %llu, not on the medium's physical block "
              "of %u bytes, which begin at sector %llu: every write would rewrite its neighbours (§1.5) — "
              "repartition the medium", (unsigned long long)base, atom, (unsigned long long)first);
    TagFSUnderGlass(vol, words);
}

error_t MountBookGround(TagFSVolume* vol, uint8_t seat, const DeedCopy* head, uint64_t ground_sectors)
{
    TagFSState* fs    = &vol->state;
    uint16_t    bytes = 0;
    const void* frame = DeedStamp(head, VOLUME_STAMP_FRAME, &bytes);
    char        words[160];

    MountBookLetGo(vol);
    ReadPaceWatch(seat);                             /* the mount's own reads: the medium's first measure */
    fs->free_blocks = 0;                             /* the counts its block map and its Tally derive, */
    fs->total_files = 0;                             /* never a volume's mounted before */
    memcpy(&vol->book.frame, frame, sizeof(VolumeFrame));
    vol->book.data_end = head->head.tail_sector / VOLUME_BLOCK_SECTORS;
    vol->book.ground   = ground_sectors;
    vol->book.on_head  = head->head.role == VOLUME_DEED_ROLE_HEAD;
    fs->block_end   = vol->book.data_end;
    if (bytes > sizeof(VolumeFrame)) {
        ksnprintf(words, sizeof(words), "its FRAME is %u bytes, longer than the %u this reader knows — "
                  "written by a kernel that knows more (§1.2)", bytes, (unsigned)sizeof(VolumeFrame));
        TagFSUnderGlass(vol, words);
    }
    BookGroundAtom(vol, seat);
    return OK;
}

// ----------------------------------------------------------------------------
// The slot stood on (§1.4, §4.5 п.2)
// ----------------------------------------------------------------------------

error_t BookSlotReads(TagFSVolume* vol, const SpineSlot* s, char* why, size_t room)
{
    char* words = kmalloc(512);
    if (!words)
        return ERR_NO_MEMORY;
    error_t rc = OK;
    for (uint32_t i = 0; i < s->roots && rc == OK; i++) {
        if (!SpineTreeAuthoritative(&s->root[i]))
            continue;
        rc = TallyRootReads(vol, s->root[i].tree, &s->root[i], words, 512);
        if (rc == ERR_TAGFS_CORRUPTED)
            ksnprintf(why, room, "its %s root will not read — %s", TallyTreeName(s->root[i].tree), words);
    }
    kfree(words);
    return rc;
}

/* The newest slot that holds and names a Tally whose roots read — the slots in
 * the order every reader of a book tries them (spine_read.c, SpineKeep), and
 * the roots every reader holds them to (volume_spine.h, SpineTreeAuthoritative)
 * — each passed over said. OK, or the refusal. */
static error_t BookStand(TagFSVolume* vol, const char** what)
{
    char why[256];
    for (uint32_t i = 0; i < vol->book.nslots; i++) {
        SpineSlot* s  = &vol->book.slots[i];
        error_t    rc = BookSlotReads(vol, s, why, sizeof(why));
        if (rc == OK) {
            vol->book.stood      = s;
            vol->book.said.slot  = s->index;
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
static void BookSlotSaid(TagFSVolume* vol, const SpineSlot* s)
{
    for (uint32_t i = 0; i < s->nglass; i++)
        TagFSUnderGlass(vol, s->glass[i]);
    if (s->shape[0]) {
        kprintf("[TagFS] the book is written against its own shape — %s\n", s->shape);
        vol->book.said.shapes++;
    }
}

// ----------------------------------------------------------------------------
// The ribbon past the Tally (§4.5 пп.5–6)
// ----------------------------------------------------------------------------

/* The ribbon from the stood slot's `at`: pass 1, the data of its newest
 * stitch, pass 2 — and what the mount says of it in numbers; then the writer's
 * cursor laid where the walk ended (settle/sewing.h), or the reason no writer
 * goes on from there, under glass. A medium a book given up this boot found
 * lying (away.h, AwayLiedLearned) is held to have lied before pass 2 reads
 * the data of the newest stitches, as one its own ribbon shows lying is. */
static error_t BookRibbon(TagFSVolume* vol, const char** what)
{
    const char* glass = NULL;
    error_t     rc    = RibbonCollate(vol, vol->book.stood, &vol->book.ribbon);
    if (rc != OK) {
        *what = rc == ERR_NO_MEMORY ? "there is no memory to read the ribbon past its Tally"
                                    : "its medium would not give up the ribbon past its Tally";
        return rc;
    }
    RibbonWalk* w = vol->book.ribbon;
    if (!w->lied && AwayLiedLearned(vol, vol->state.uuid)) {
        w->lied = true;
        kprintf("[TagFS] this volume's medium came back this boot holding less than a flush it said it kept "
                "covered (its book was given up then): it is held to have lied\n");
    }
    rc = RibbonTranscribe(vol, w, what);
    if (rc != OK)
        return rc;
    vol->book.said.sewn  = w->nstitch;
    for (uint32_t i = 0; i < w->nstitch; i++)
        vol->book.ribbon_useful += w->stitch[i]->useful;
    vol->book.said.last  = w->nstitch ? w->stitch[w->nstitch - 1u]->seq : vol->book.stood->head.stitch_seq;
    vol->book.said.lied  = w->lied;
    vol->book.said.flags = w->nstitch && w->stitch[w->nstitch - 1u]->head_read ? w->stitch[w->nstitch - 1u]->head.flags
                                                                           : 0;
    kprintf("[TagFS] the ribbon past the Tally: %u stitch(es) sewn on, the last seq %llu, %u applied\n",
            w->nstitch, (unsigned long long)vol->book.said.last, vol->book.said.applied);
    if (vol->book.said.flags & STITCH_CLEAN)
        kprintf("[TagFS] the last stitch of the ribbon says CLEAN: the machine that sewed it stopped with no change "
                "in flight\n");
    if (w->lied)
        kprintf("[TagFS] MEDIUM_LIED: this volume's medium has not kept what it confirmed — a Fallow lags two "
                "stitches on it, and the newest two are held to their data at a mount (§3.9); every stitch this "
                "kernel sews on it says so\n");
    if (RibbonCursorMount(vol, w, &glass) != OK)
        TagFSUnderGlass(vol, glass);
    if (w->seen_fid > vol->book.next_fid)
        vol->book.next_fid = w->seen_fid;
    if (w->seen_tag > vol->book.next_tag)
        vol->book.next_tag = w->seen_tag;
    vol->book.next_bookmark = w->seen_bookmark;
    return OK;
}

// ----------------------------------------------------------------------------
// The walk of the Tally stood on (§4.5 п.3)
// ----------------------------------------------------------------------------

/* DIGESTS at the mount: its inner nodes read, each leaf kept by the range of
 * blocks it covers, read when a block of it is (§8.1, TallyDigestFind). */
static error_t BookDigestNode(TagFSVolume* vol, void* ctx, const TallyPlace* at)
{
    BookGroundNode(vol, ctx, at);
    return at->level == 0 && at->ptr.copies ? TallyDigestsKeep(vol, at, false) : OK;
}

static error_t BookDigestRot(TagFSVolume* vol, void* ctx, const TallyPlace* at, const char* why)
{
    (void)ctx;
    (void)why;
    if (at->level > 0)
        EndpaperLaidNode(&vol->book.laid, TALLY_DIGESTS, at->level, 0, 1u, at->has_lo ? at->lo.k1 : 0);
    return TallyDigestsKeep(vol, at, true);
}

static error_t BookQuietRot(TagFSVolume* vol, void* ctx, const TallyPlace* at, const char* why)
{
    (void)vol;
    (void)ctx;
    (void)at;
    (void)why;
    return OK;
}

/* One root of the slot stood on, walked as its tree is read. */
static error_t BookTree(TagFSVolume* vol, const SpineRoot* root)
{
    char      words[192];
    TallySink sink = { .ctx = NULL, .item = NULL, .node = BookGroundNode, .said = true,
                       .mend = BookGroundMended, .shapes = &vol->book.said.shapes };
    bool derived = root->tree == TALLY_MAP || root->tree == TALLY_SHARED || (root->flags & TALLY_ROOT_DERIVED);

    vol->book.laid_tree = root->tree;

    if (!derived && !SpineTreeAuthoritative(root)) {
        ksnprintf(words, sizeof(words), "slot %u names a tree of kind %llu this reader does not know, and "
                  "not one to derive — written by a kernel that knows more", vol->book.stood->index,
                  (unsigned long long)root->tree);
        TagFSUnderGlass(vol, words);
    }
    if (!SpineTreeAuthoritative(root)) {
        sink.rotten = BookQuietRot;
        sink.said   = false;
        return TallyRead(vol, root->tree, root, false, &sink);
    }
    if ((root->tree == TALLY_BOOKMARKS || root->tree == TALLY_LIEN) && root->items) {
        ksnprintf(words, sizeof(words), "its %s tree is not empty — a bookmark this kernel cannot hold (§5)",
                  TallyTreeName(root->tree));
        TagFSUnderGlass(vol, words);
    }
    if (root->tree != TALLY_DIGESTS)
        return TallyLeaves(vol, root->tree, root);
    sink.node   = BookDigestNode;
    sink.rotten = BookDigestRot;
    return TallyRead(vol, TALLY_DIGESTS, root, false, &sink);
}

/* Every root of the slot stood on, in the order of its trees. A slot that
 * names no USE root holds an empty Use Context — a writer leaves the root out
 * when there is none (§16.3 п.5) — and that, too, is what the medium
 * remembers: recalled, so a Remember of nothing is the nothing that settled
 * (§7). */
static error_t BookWalk(TagFSVolume* vol, const char** what)
{
    const SpineSlot* s    = vol->book.stood;
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
        rc   = BookTree(vol, next);
    }
    if (rc == OK && !use)
        rc = BookUseRecalled(vol, NULL, 0);
    if (rc == OK && !use)
        rc = VolumeNameRecall(vol, NULL, 0);
    if (rc != OK)
        *what = rc == ERR_NO_MEMORY ? "there is no memory to hold what its Tally names"
                                    : "its medium would not give up its Tally";
    return rc;
}

// ----------------------------------------------------------------------------
// The numbers given next (§4.5 п.8)
// ----------------------------------------------------------------------------

static void BookBounds(TagFSVolume* vol)
{
    TagFSState*        fs = &vol->state;
    const SpineCounts* c  = &vol->book.stood->counts;
    uint64_t           known_file = 0;
    uint32_t           known_tag  = 0;
    uint64_t           fid = vol->book.next_fid > c->next_fid ? vol->book.next_fid : c->next_fid;
    uint64_t           tag = vol->book.next_tag > c->next_tag ? vol->book.next_tag : c->next_tag;

    AwayNumbersKnown(vol, fs->uuid, &known_file, &known_tag);
    if (known_file > fid)
        fid = known_file;
    if (known_tag > tag)
        tag = known_tag;
    uint64_t bookmark = vol->book.next_bookmark > c->next_bookmark ? vol->book.next_bookmark : c->next_bookmark;
    kprintf("[TagFS] the next file id %llu, tag id %llu, bookmark %llu, seq %llu — past every one the book "
            "names\n", (unsigned long long)fid, (unsigned long long)tag, (unsigned long long)bookmark,
            (unsigned long long)(BookSeqSeen(vol) + 1u));
    vol->book.next_fid         = fid;
    vol->book.next_tag         = tag;
    vol->book.next_bookmark    = bookmark;
    fs->total_files         = vol->book.files;
    TagRegistryNumbersFrom(fs->registry, tag > (uint64_t)TAGFS_MAX_TAG_ID + 1u ? TAGFS_MAX_TAG_ID + 1u
                                                                             : (uint32_t)tag);
}

// ----------------------------------------------------------------------------
// The whole read
// ----------------------------------------------------------------------------

/* What the mount fills: the registry, the map of live files, the tag index. */
static error_t BookMemory(TagFSVolume* vol, const char** what)
{
    TagFSState* fs = &vol->state;
    fs->braid      = NULL;
#ifdef CONFIG_VOLUME_TESTS
    TagFSStandMountRegistryLoads(vol);
#endif
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
static error_t BookSpine(TagFSVolume* vol, const char** what)
{
    char*   states = kmalloc(256);
    error_t rc     = states ? SpineRead(vol, &vol->book.frame, &vol->book.slots, &vol->book.nslots, states, 256)
                            : ERR_NO_MEMORY;
    if (rc == OK && vol->book.nslots == 0) {
        ksnprintf(vol->book_mount.refusal, sizeof(vol->book_mount.refusal), "none of the %u slots of its Spine holds (%s) — the book is "
                  "not read (ERR_TAGFS_CORRUPTED)", vol->book.frame.spine_slots, states);
        *what = vol->book_mount.refusal;
        rc    = ERR_TAGFS_CORRUPTED;
    } else if (rc == ERR_READ_FAILED && states[0]) {
        ksnprintf(vol->book_mount.refusal, sizeof(vol->book_mount.refusal), "none of the %u slots of its Spine holds, and its medium would "
                  "not read a sector of one (%s) — the medium's refusal, no verdict on the book: asked again "
                  "when its seat answers", vol->book.frame.spine_slots, states);
        *what = vol->book_mount.refusal;
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
static error_t BookSlotsHeld(TagFSVolume* vol, const char** what)
{
    if (BinderSlotsMake(vol, vol->book.frame.spine_slots) != OK) {
        *what = "there is no memory for what the slots of its Spine hold";
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < vol->book.nslots; i++)
        if (BinderSlotHolds(vol, vol->book.slots[i].index, vol->book.slots[i].seq, vol->book.slots[i].head.tally_seq,
                            vol->book.slots[i].head.stitch_seq) != OK) {
            *what = "its Spine read a slot past the ring its FRAME names";
            return ERR_TAGFS_CORRUPTED;
        }
    return OK;
}

/* The data run, its free blocks — less a block promised to each file whose
 * tail a Slip lays (block_map.h), as a program is told — and the files; the
 * promised said beside them. The mounting being made has no Plot of its own
 * yet — its door is not open — and a Plot of the one before promises it
 * nothing, so its promises are its records' alone (FileMapSlips): the count
 * bookcensus makes of the image. */
static void BookFreeSaid(TagFSVolume* vol)
{
    uint64_t data     = vol->book.data_end - vol->book.frame.data_block;
    uint64_t free     = vol->state.free_blocks;
    uint64_t promised = FileMapSlips(vol->state.files);
    free = free > promised ? free - promised : 0;
    if (promised == 0)
        kprintf("[TagFS] %llu data blocks, %llu free, %llu files\n", (unsigned long long)data,
                (unsigned long long)free, (unsigned long long)vol->book.files);
    else
        kprintf("[TagFS] %llu data blocks, %llu free, %llu files — and %llu block(s) promised, one to each file "
                "whose tail a Slip lays, for the Engross that lays it there\n", (unsigned long long)data,
                (unsigned long long)free, (unsigned long long)vol->book.files, (unsigned long long)promised);
}

error_t MountReadBook(TagFSVolume* vol, const char** what)
{
    BookSeqSeenForget(vol);
    BinderMountReset(vol);
    FoxingBegin(vol);
    /* Rot is named from here on, each block once for this mount, on the
     * "integrity" tag of the kernel's Logbook: a rotten block is an occurrence
     * the kernel saw, never a name the volume's registry is given to carry
     * (logbook.h) — and so it is published under glass too. */
    IntegrityRotBegin(vol, TouchLogbookIntern("integrity"));
    error_t rc = BookMemory(vol, what);
    if (rc == OK)
        rc = BookSpine(vol, what);
    if (rc == OK)
        rc = BookSlotsHeld(vol, what);
    if (rc == OK)
        rc = BookStand(vol, what);
    if (rc != OK) {
        FoxingEnd(vol);
        return rc;
    }

    BookSlotSaid(vol, vol->book.stood);
    GroundCensusBegin(vol);
    rc = BookWalk(vol, what);
    if (rc == OK)
        rc = MountFacsimile(vol, what);
    FacsimileLetGo(vol);
    if (rc == OK)
        rc = BookRibbon(vol, what);
    if (rc == OK) {
        BookFilesLaid(vol);
        rc = BookGroundCensus(vol, what);
    }
    RibbonWalkLetGo(vol->book.ribbon);
    vol->book.ribbon = NULL;
    GroundCensusEnd(vol);
    FoxingEnd(vol);
    if (rc != OK)
        return rc;
    if (vol->book.bookmarked)
        TagFSUnderGlass(vol, "a Tally of its book names a bookmark, whose ground this kernel does not derive — its "
                        "writer takes no ground without the map of it");
    BookBounds(vol);
    MountBookGrow(vol, tagfs_get_seat(vol));
    BinderMountStood(vol, vol->book.stood->index, vol->book.tally_blocks, vol->book.ribbon_useful, vol->book.next_fid,
                     vol->book.next_tag, vol->book.next_bookmark);
    /* The leaves of FILES a Parcel's runs lie in, as the walk of the leaves
     * counted them into Foliation — every leaf when Foliation was lost. */
    uint64_t run_leaves    = FoliationLeavesHolding(vol, TALLY_FILES);
    vol->book.laid.run_leaves = run_leaves != UINT64_MAX ? run_leaves : vol->book.laid.runs ? vol->book.laid.files_leaves : 0;
    BinderBarPublish(vol, vol->book.stood->head.stitch_seq);
    EndpaperStood(vol, &vol->book.laid, vol->book.stood->head.tally_seq, vol->book.stood->counts.next_fid);
    kprintf("[TagFS] the book as memory holds it: its files print 0x%08X, its Use Context %llu byte(s) print "
            "0x%08X\n", vol->book.files_print, (unsigned long long)vol->book.use_bytes, vol->book.use_print);
    if (vol->book.bookmarked)
        kprintf("[TagFS] %llu data blocks, free not counted, %llu files — a Tally of the book names a bookmark, "
                "whose ground this kernel does not read: no block is counted free, and none is given\n",
                (unsigned long long)(vol->book.data_end - vol->book.frame.data_block), (unsigned long long)vol->book.files);
    else
        BookFreeSaid(vol);
    /* A ribbon that goes on at the start of the next_quire it names
     * (RibbonGoesOnAt) stands in it with none given past it, and a change
     * claims room past a next_quire (RibbonRoomOf): without one every change
     * but a freeing one waits for a cycle to give it (ERR_WOULD_BLOCK). So
     * the mount asks first, before the door opens and any cycle runs; past
     * the free it says, which is the volume's as it lies (bookcensus.py). A
     * refusal is the first cycle's to ask again (RibbonNextGivenAtMount). A
     * book under glass takes no ground. */
    if (!TagFSIsUnderGlass(vol))
        RibbonNextGivenAtMount(vol);
    return OK;
}

/*
 * Under glass the book is read and nothing of it is written, and promises only
 * that. Otherwise this kernel's writer lays stitches of files, their data on
 * fresh ground, names and memory on its ribbon (settle/sewing.h), and a whole
 * Tally of it once the ribbon holds what one costs (settle/binder.h): the next
 * mount reads that Tally and the ribbon past it; and a medium that leaves and
 * comes back as it was left is taken up, what no stitch settled going with the
 * next cycle — read again from its book otherwise, what no stitch settled then
 * lost (§3.8, §16.5 п.10, perambulation.h). Under glass nothing is written,
 * and nothing waits: a medium that comes back is read again.
 */
void MountBookPromise(TagFSVolume* vol)
{
    if (TagFSIsUnderGlass(vol)) {
        kprintf("[TagFS] what this volume promises: it is read as its book holds it and nothing of it is "
                "written — a change is refused (ERR_TAGFS_READ_ONLY); a medium that leaves and comes back "
                "is read again from its book\n");
        return;
    }
    kprintf("[TagFS] what this volume promises: its book takes files made, written, named, tagged and given "
            "up, the names of tags and the memory of its Use Context, a stitch on its ribbon a Settle cycle — a "
            "change is a stitch kept by its cycle's flush within %llu ms, and a name given goes with the next "
            "stitch sewn; a file's bytes go on fresh ground, flushed before the stitch that names them, so a "
            "block written over reads its old bytes or its new ones, never a mix; an `anchor` is answered once "
            "a stitch of this mount carrying what it waits for is kept; a medium that leaves and comes back is "
            "taken up as it was left — its ribbon and newest slot as this kernel left them, every block written "
            "since the last kept flush read back as written — and read again from its book otherwise, what no "
            "stitch settled then lost (ERR_TAGFS_NOT_LANDED where a block did not land)\n",
            (unsigned long long)(SETTLE_WINDOW_US / 1000u));
    if (BinderNoTallies(vol))
        kprintf("[TagFS] no Tally of the book is written by this mount: %s — the ribbon is read whole at the "
                "next mount, none of its Quires comes back before it, and once they run out files, names, tags "
                "and words are refused (ERR_TAGFS_NO_SPACE)\n", BinderNoTallies(vol));
    else
        kprintf("[TagFS] a whole Tally of the book is written once the ribbon past the newest slot holds as "
                "many useful bytes as one costs (TALLY_SLACK %u), and when the ribbon runs out: the next mount "
                "reads the newest slot's Tally and the ribbon past it, and the ground the slots let go of comes "
                "back\n", TALLY_SLACK);
}

// ----------------------------------------------------------------------------
// Locked files, and letting go
// ----------------------------------------------------------------------------

bool BookFileLocked(TagFSVolume* vol, uint64_t fid)
{
    /* A file Escheat let go is no file from its DONE on, as after the next
     * mount, whose Tally names it no more (settle/escheat.h). */
    if (EscheatLost(vol, fid))
        return false;
    uint32_t lo = 0, hi = vol->book.nlocked;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (vol->book.locked[mid] == fid)
            return true;
        if (vol->book.locked[mid] < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    FileRecord* live = FileMapRecordHold(vol->state.files, fid, NULL);
    FileRecordRelease(live);
    return !live && BookRotLocks(vol, fid);
}

bool BookAnyLocked(TagFSVolume* vol)
{
    if (vol->book.nrot)
        return true;
    for (uint32_t i = 0; i < vol->book.nlocked; i++)
        if (!EscheatLost(vol, vol->book.locked[i]))
            return true;
    return false;
}

/* Whether a rotten range of FILES holds `fid`. */
static bool BookInRot(TagFSVolume* vol, uint64_t fid)
{
    for (uint32_t i = 0; i < vol->book.nrot; i++) {
        const BookRotten* r = &vol->book.rot[i];
        if (fid >= r->lo.k1 && (!r->has_hi || fid < r->hi.k1 || (fid == r->hi.k1 && r->hi.k2 > 0)))
            return true;
    }
    return false;
}

bool BookRotLocks(TagFSVolume* vol, uint64_t fid)
{
    uint32_t lo = 0, hi = vol->book.nborn;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (vol->book.born[mid] == fid)
            return false;
        if (vol->book.born[mid] < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return BookInRot(vol, fid);
}

error_t BookBornNoted(TagFSVolume* vol, uint64_t fid)
{
    uint32_t lo = 0, hi = vol->book.nborn;
    if (!BookInRot(vol, fid))
        return OK;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (vol->book.born[mid] < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo < vol->book.nborn && vol->book.born[lo] == fid)
        return OK;
    if (vol->book.nborn == vol->book.capborn) {
        uint32_t  cap  = vol->book.capborn ? vol->book.capborn * 2u : 16u;
        uint64_t* more = kmalloc(sizeof(uint64_t) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (vol->book.nborn)
            memcpy(more, vol->book.born, sizeof(uint64_t) * vol->book.nborn);
        kfree(vol->book.born);
        vol->book.born    = more;
        vol->book.capborn = cap;
    }
    memmove(&vol->book.born[lo + 1u], &vol->book.born[lo], sizeof(uint64_t) * (vol->book.nborn - lo));
    vol->book.born[lo] = fid;
    vol->book.nborn++;
    return OK;
}

error_t BookUseRecalled(TagFSVolume* vol, const uint8_t* text, uint64_t len)
{
    error_t rc = UseMemoryRecall(vol, text, len);
    if (rc == OK) {
        vol->book.use_print = len ? KCrc32c(text, (uint32_t)len) : 0;
        vol->book.use_bytes = len;
    }
    return rc;
}

bool TagFSFileLocked(TagFSVolume* vol, uint64_t fid)
{
    if (!tagfs_enter(vol))
        return false;
    bool locked = BookFileLocked(vol, fid);
    tagfs_leave(vol);
    return locked;
}

void BookKeptLetGo(BookKeptFile* k)
{
    for (uint32_t p = 0; p < k->nparcels; p++)
        kfree(k->parcel_body[p]);
    kfree(k->meta.filename);
    kfree(k->meta.tag_ids);
    kfree(k->meta.shown_to);
    kfree(k->parcel_b);
    kfree(k->parcel_body);
    kfree(k->parcel_len);
    kfree(k->parcel_seq);
    memset(k, 0, sizeof(*k));
}

void MountBookLetGo(TagFSVolume* vol)
{
    BookFileForget(&vol->book.file);
    SpineLetGo(vol->book.slots, vol->book.nslots);
    kfree(vol->book.rot);
    kfree(vol->book.locked);
    for (uint32_t i = 0; i < vol->book.nkept; i++)
        BookKeptLetGo(&vol->book.kept[i]);
    kfree(vol->book.kept);
    kfree(vol->book.fallow);
    kfree(vol->book.born);
    for (uint32_t i = 0; i < vol->book.naside; i++)
        kfree(vol->book.aside[i].text);
    kfree(vol->book.aside);
    ErrataLetGo(vol);
    RibbonWalkLetGo(vol->book.ribbon);
    RibbonCursorLetGo(vol);
    BinderMountReset(vol);
    DedupLetGo(vol);
    GlossLetGo(vol);
    CommonholdLetGoAll(vol);
    TallyDigestsLetGo(vol);
    BlockMapDerivedLetGo(vol);
    memset(&vol->book, 0, sizeof(vol->book));
}

#ifdef CONFIG_VOLUME_TESTS
void MountBookStandSaid(TagFSVolume* vol, MountBookSaid* out)
{
    *out = vol->book.said;
}
#endif
