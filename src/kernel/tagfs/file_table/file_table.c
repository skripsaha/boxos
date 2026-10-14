#include "file_table.h"
#include "../scribe/scribe.h"
#include "../file_map/file_map.h"
#include "../settle/settle.h"
#include "../mount/ground_census.h"

/*
 * ‼ THE TABLE IS THE VOLUME'S STATE, AND ITS LOCK IS THE STATE LOCK.
 *
 * It had a leaf lock of its own, and an Ink — which changes a file's record,
 * its row and its tags in one hold of the state lock (ink/) — would have taken
 * it under state on every change. The links are changed under the state lock
 * now, by an Ink or by the flush, and by the mount's walk with the volume's
 * door shut; nothing under it goes to the medium, allocates or waits. The
 * flush copies one block's rows out under it and writes them with no lock
 * held. It used to hold its lock across every block it wrote and to take a
 * block for its chain under it — so the state lock was taken under the table
 * while a create took the table under state (MP-D4), and a core that only
 * wanted to look a file up spun with its interrupts off for as long as the
 * medium took.
 *
 * ‼ THE ROWS ARE THE LIVE FILES', AND LIVE IN THE MAP OF THEM (file_map/).
 * Memory used to hold the table as the medium does, a row for every file id
 * below the highest ever issued — eight bytes a slot, dead files' included.
 * A row is kept now beside its file's record in the map, and only while the
 * file has one; what this file keeps is the chain, a few words per block of
 * 510 ids. A block is written from the rows the map holds for its ids.
 */
static TagFSLock* table_lock(void)
{
    return &tagfs_get_state()->lock;
}

static FileMap* table_files(void)
{
    TagFSState* fs = tagfs_get_state();
    return fs ? fs->files : NULL;
}

/*
 * ‼ THE TABLE IS AS LONG AS THE CHAIN, AND THE CHAIN IS ON THE MEDIUM.
 *
 * The capacity used to be file_table_blocks × 510, out of the Deed. mkfs wrote
 * a 1 there and nothing has ever raised it, so every volume this kernel has
 * ever mounted came up able to hold exactly 510 files. The walk below already
 * READ the second and later blocks; the entries in them were dropped on the
 * floor by a bounds test, and the next flush wrote the zeros back over them.
 * A volume that had ever held a file numbered 510 or higher lost every entry
 * from there up, permanently, at the next mount.
 *
 * So the chain is counted, one block at a time, as it is followed — and the
 * blocks it is made of are kept here, so a flush writes back into the blocks
 * the table already owns.
 *
 * One entry per block of the chain. `edited` is the generation of the last
 * change to what the block carries — which files it names, or its link to the
 * next block — and `landed` the generation the image last written to it was
 * taken at, so the block is owed to the medium exactly while edited > landed.
 * A file named or let go while its block is being written moves `edited` past
 * what that write carried, and the block stays owed: a write is never taken
 * for more than the snapshot it wrote.
 *
 * ‼ A BLOCK IS OWED BY WHICH FILES IT NAMES, NOT BY WHERE THEIR RECORDS ARE.
 * A file's record moves on every change of it, and the pool is the authority
 * on where it is: the mount corrects a row naming an older record to the
 * newest one the pool holds (mount/mount.c, MountFile). So a row follows its
 * record in memory for nothing, and a block goes to the medium when a file
 * comes or goes in it — which is what the table decides on its own while the
 * volume carries no SettleMark (step 1.6).
 *
 * Past g_link_count, a slot whose block is not 0 is a Spare the flush has
 * stamped for the chain and not linked yet — the medium would not yet commit
 * the stamp (see FileTableGrow). The links and the slots change only in the
 * flush, which the Settle cycle runs under its Scribe, and in the mount's
 * walk, with the door closed; update and delete only mark links, under the
 * state lock.
 *
 * A row is changed in the map before its link is marked, and a flush reads the
 * mark before it copies the rows: a change the copy missed has marked its link
 * past what that write carries, and the link stays owed.
 */
typedef struct {
    uint32_t block;    /* the data-run block this link is */
    uint64_t edited;
    uint64_t landed;
} FileTableLink;

static FileTableLink* g_links        = NULL;
static uint32_t       g_link_count   = 0;   /* links of the chain on the medium */
static uint32_t       g_link_cap     = 0;   /* slots in g_links */
static uint32_t       g_links_wanted = 0;   /* links the rows named since the mount need */
static uint64_t       g_gen          = 0;   /* one step per change, under the state lock */
static bool           g_grow_short   = false; /* short of its rows' links: said (FileTableGrow) */

/* The one block image there is: the mount's walk reads into it, and the flush
 * stamps a Spare and snapshots a link through it — the flush alone, under the
 * Settle's Scribe, so no two of them ever share it. */
static FileTableBlock g_image;

/*
 * Room for `want` links, made BEFORE any block is taken for one: a growth
 * that has taken its block never needs memory to keep it, so no refused
 * allocation can strand a block the allocator already handed out (CR-11: the
 * chain's record used to be lengthened after tagfs_alloc_blocks, and a refusal
 * there returned with the block taken and named by nothing, for good).
 *
 * Called with no lock held — by the flush, or by the mount's walk — and the
 * array swapped under the state lock, since update and delete mark links under
 * it. A
 * new array holds exactly `want`; the walk, which cannot know the chain's
 * length before it has followed it, asks twice as many each time it runs out.
 */
static error_t LinksReserve(uint32_t want)
{
    if (want <= g_link_cap)
        return OK;

    FileTableLink* grown = kmalloc(sizeof(FileTableLink) * want);
    if (!grown)
        return ERR_NO_MEMORY;

    TagFSLockTake(table_lock());
    if (g_links)
        memcpy(grown, g_links, sizeof(FileTableLink) * g_link_cap);
    memset(grown + g_link_cap, 0, sizeof(FileTableLink) * (want - g_link_cap));
    FileTableLink* old = g_links;
    g_links    = grown;
    g_link_cap = want;
    TagFSLockDrop(table_lock());
    if (old)
        kfree(old);
    return OK;
}

static void table_release(void) {
    if (g_links)   kfree(g_links);
    g_links        = NULL;
    g_link_count   = 0;
    g_link_cap     = 0;
    g_links_wanted = 0;
    g_gen          = 0;
    g_grow_short   = false;
}

/*
 * A block of the chain that does not read as the table — it would not read,
 * holds another magic, or the link naming it points out of the run or round
 * in a circle. With no seal the table decides which files there are, so a
 * table that will not read is a volume that does not mount: carrying on would
 * read as "no files", and the writes that follow take their blocks. Under a
 * seal the pool decides (the mount derives every row from the sealed span), so
 * the chain ends there: a block that was reached is kept as the chain's last,
 * owed whole to the medium, and anything its link named after it is ground the
 * census names. True: the walk stops and the mount goes on; false: refused.
 * Said only by the mount (`take`): the stand's question of a chain it built
 * aside is no word on the volume.
 */
static bool TableBlockRefused(bool sealed, bool reached, bool take, uint32_t block,
                              const char* what)
{
    if (!sealed) {
        if (take)
            kprintf("[FileTable] block %u %s — this volume's file table cannot be walked, so "
                    "it is not mounted\n", block, what);
        return false;
    }
    if (take)
        kprintf("[FileTable] block %u %s — under the volume's seal the table is derived from "
                "the pool, so the volume mounts, and the next Settle cycle writes the table's "
                "chain whole from %s\n", block, what,
                reached ? "that block on" : "its last good block");
    return true;
}

/* One link taken into the chain, and its block's rows into the map of live
 * files. A table the map has no room for is not a table held in part: the rows
 * past the refusal would read as no files. ERR_NO_MEMORY, said, when memory
 * ran out — a refusal the mount waits out, not one it holds against the
 * volume. */
static error_t TableTakeLink(uint32_t block, const FileTableBlock* image, bool rows)
{
    if (g_link_count == g_link_cap && LinksReserve(g_link_cap ? g_link_cap * 2 : 1) != OK) {
        kprintf("[FileTable] there is no memory to hold this volume's file table's chain at "
                "%u blocks — it is not mounted\n", g_link_count + 1);
        return ERR_NO_MEMORY;
    }
    uint32_t entry_offset = g_link_count * TAGFS_FTABLE_PER_BLOCK;
    g_links[g_link_count++] = (FileTableLink){ .block = block };

    uint32_t count = rows ? image->entry_count : 0;
    if (count > TAGFS_FTABLE_PER_BLOCK)
        count = TAGFS_FTABLE_PER_BLOCK;
    for (uint32_t r = 0; r < count; r++) {
        const FileTableEntry* row = &image->entries[r];
        if (row->meta_block == 0 || entry_offset + r == 0)
            continue;
        if (FileMapRowSet(table_files(), entry_offset + r, row->meta_block, row->meta_offset) != OK) {
            kprintf("[FileTable] there is no memory to hold the row of file %u — this volume "
                    "is not mounted\n", entry_offset + r);
            return ERR_NO_MEMORY;
        }
    }
    return OK;
}

/*
 * The chain walked from `first_block` through `image`: taken into the table
 * when `take` (the mount), or only walked (the stand's question of whether a
 * mount would stand on it). OK, or — said — ERR_NO_MEMORY when the machine
 * had no memory to take it, the read's own code when the medium refused one,
 * ERR_FILE_TABLE_CORRUPT when it is no table a mount stands on.
 *
 * ‼ THE ONLY BOUND ON THIS WALK IS A TERMINATION PROOF, NOT A LENGTH. A chain
 * of distinct blocks cannot be longer than the data run it lives in, so a walk
 * that takes that many hops has come back to a block it has been to; and a
 * next_block outside the run is refused before it is read — nothing under
 * TagFSMetaBlockRead bounds a block number.
 */
static error_t TableWalk(uint32_t first_block, bool sealed, bool take, FileTableBlock* image)
{
    TagFSState* fs    = tagfs_get_state();
    uint32_t    run   = fs ? TagFSLayout()->data_blocks : 0;
    if (run == 0 || (take && !table_files())) {
        kprintf("[FileTable] this volume states a data run of no blocks at "
                "all, so there is nowhere for a file table to be — it is not "
                "mounted\n");
        return ERR_FILE_TABLE_CORRUPT;
    }

    uint32_t block = first_block;
    bool     cut   = false;     /* the chain ends short of its last link: owed whole */
    error_t  taken = OK;
    for (uint32_t hops = 0; ; hops++) {
        if (block >= run) {
            if (!TableBlockRefused(sealed, false, take, block,
                                   "lies outside the volume's data run"))
                return ERR_FILE_TABLE_CORRUPT;
            cut = true;
            break;
        }

        /* ‼ Tested as `!= OK`, because TagFSMetaBlockRead returns an error_t
         * whose failures are POSITIVE and the `< 0` this used to ask was never
         * once true. And the medium refusing the read is told apart from its
         * answer: a refusal says nothing of what the block holds — under the
         * seal too, where judging it would cut a whole chain short — so the
         * mount is refused as the read was (counted where the read met it,
         * TagFSMetaBlockRead) and asked again when the seat answers. A sector
         * the medium says it cannot read is what the block holds. */
        error_t got = TagFSMetaBlockRead(block, image);
        if (got != OK && got != ERR_BAD_SECTOR) {
            if (take)
                kprintf("[FileTable] the medium refused the read of block %u (error %d) — a "
                        "refusal says nothing of what the block holds, so the table is not "
                        "judged on it: the volume is not mounted\n", block, (int)got);
            return got;
        }
        bool table = got == OK && image->magic == TAGFS_FILETBL_MAGIC;
        if (!table) {
            if (!TableBlockRefused(sealed, true, take, block,
                                   got == OK ? "holds no file table"
                                             : "holds a sector the medium cannot read"))
                return ERR_FILE_TABLE_CORRUPT;
            if (take && (taken = TableTakeLink(block, image, false)) != OK)
                return taken;
            cut = true;
            break;
        }
        if (take && (taken = TableTakeLink(block, image, true)) != OK)
            return taken;
        if (image->next_block == 0)
            break;
        if (hops + 1 >= run) {
            if (!TableBlockRefused(sealed, false, take, block,
                                   "links a chain that goes round in a circle"))
                return ERR_FILE_TABLE_CORRUPT;
            cut = true;
            break;
        }
        block = image->next_block;
    }

    /* A chain cut short is written again from its last link: that link's
     * image names no next block, and its rows are the map's. */
    if (take && cut && g_link_count)
        g_links[g_link_count - 1].edited = ++g_gen;
    return OK;
}

error_t file_table_init(uint32_t first_block, bool sealed) {
    table_release();
    error_t walked = TableWalk(first_block, sealed, true, &g_image);
    if (walked != OK) {
        table_release();
        return walked;
    }
    g_links_wanted = g_link_count;
    return OK;
}

#ifdef CONFIG_VOLUME_TESTS
bool FileTableWouldStand(uint32_t first_block, bool sealed)
{
    FileTableBlock* image = kmalloc(sizeof(FileTableBlock));
    bool            stand = image && TableWalk(first_block, sealed, false, image) == OK;
    kfree(image);
    return stand;
}
#endif

void FileTableClaimGround(void)
{
    for (uint32_t i = 0; i < g_link_count; i++)
        GroundClaim(GROUND_TABLE, g_links[i].block, 1, 0);
}

void file_table_shutdown(void) {
    table_release();
    debug_printf("[FileTable] shutdown\n");
}

int file_table_lookup(uint64_t file_id, uint32_t* out_block, uint32_t* out_offset) {
    FileMap* files = table_files();
    return files && FileMapRowGet(files, file_id, out_block, out_offset) ? 0 : -1;
}

/* The link that carries a row marked owed, after the row changed in the map.
 * A row past the chain marks no link: the link that will carry it is attached
 * by the flush already owed to the medium (FileTableGrow). Under the state
 * lock, or with the door shut. A row's id is format v1's u32 — no record of
 * that format carries another (PackV1) — so its link is one too. */
static void table_mark(uint64_t file_id, bool named) {
    uint32_t link = (uint32_t)(file_id / TAGFS_FTABLE_PER_BLOCK);
    g_gen++;
    if (link < g_link_count)
        g_links[link].edited = g_gen;
    if (named && link >= g_links_wanted)
        g_links_wanted = link + 1;
}

int file_table_update(uint64_t file_id, uint32_t meta_block, uint32_t meta_offset) {
    FileMap* files = table_files();
    if (!files || FileMapRowSet(files, file_id, meta_block, meta_offset) != OK) {
        kprintf("[FileTable] there is no memory to hold a row for file id %llu "
                "— the row is not kept, and the change that named it is "
                "refused\n", (unsigned long long)file_id);
        return -1;
    }
    table_mark(file_id, true);
    return 0;
}

int file_table_delete(uint64_t file_id) {
    FileMap* files = table_files();
    if (!files)
        return -1;
    FileMapRowClear(files, file_id);
    table_mark(file_id, false);
    return 0;
}

/*
 * ‼ THE CHAIN GROWS ONTO SPARES, AND A SPARE IS LINKED ONLY ONCE ITS STAMP IS
 * DURABLE.
 *
 * The chain used to grow by writing its last block's link to a block it had
 * just taken, and that block's rows after it: a power cut between the two, a
 * refused write of the new block, or a cut that kept the link and lost the
 * block out of the same flush epoch, left a chain running into ground holding
 * whatever it held — and every later mount refused the volume there (MP-D5).
 *
 * So each block the rows need is first a Spare: taken from the allocator,
 * stamped with an empty table header, and made durable by a flush issued
 * after the stamp was seen written (SpareStamp, then VolumeFlushFor its need
 * — THE SPARE RULE, scribe.h). Only then is it attached, as the new last link,
 * and the links are written in chain order: the old last block with its link,
 * then the new block's rows. Any cut of what follows the flush finds a
 * stamped block at the end of every link on the medium; a cut before it finds
 * the chain as it was, and at worst the Spare taken and named by nothing.
 *
 * The array for every link is reserved before any block is taken, and nothing
 * is allocated, written or waited for under the state lock. A refused stamp gives its
 * block back; a flush the medium will not keep leaves the stamped Spares in
 * their slots, unlinked, for the next pass to link once a flush is kept.
 * Rows past the chain stay in memory until then.
 *
 * ‼ A TABLE SHORT OF GROUND OR MEMORY IS NOT A REFUSED CYCLE. Its rows wait in
 * memory, and the pool's records name them: under the seal every cycle
 * writes, the mount derives the table's rows from the pool (mount/mount.c,
 * MountFile), so a cycle settles whole without them. Refusing it instead
 * refused every cycle after it at the same place — and the ground a short
 * volume waits for comes back only when a cycle settles (Fallow, at S6): the
 * volume stopped settling for good. The allocator asks for the cycle that
 * gives Fallow's ground back itself (MapShort's ROOM), and a later cycle
 * grows the chain. Said once until the chain has grown to what its rows need.
 * Only the medium's own refusal — a stamp, a flush — refuses the cycle.
 */
static void FileTableShortSay(uint32_t wanted, uint32_t have, error_t why)
{
    if (!g_grow_short)
        kprintf("[FileTable] the file table needs %u more block(s) to hold file ids up to %u "
                "and the %s (error %u) — the rows from file id %u on wait in memory, the "
                "pool's records name them, and a later Settle cycle grows the chain\n",
                wanted - have, wanted * TAGFS_FTABLE_PER_BLOCK - 1,
                why == ERR_NO_MEMORY ? "machine had no memory to take them with"
                                     : "volume gave none",
                (unsigned)why, have * TAGFS_FTABLE_PER_BLOCK);
    g_grow_short = true;
}

/* A refusal that is not the medium's: no ground, ground on its way, or no
 * memory. */
static bool FileTableShortOfRoom(error_t got)
{
    return got == ERR_TAGFS_NO_SPACE || got == ERR_WOULD_BLOCK || got == ERR_NO_MEMORY;
}

static error_t FileTableGrow(void)
{
    TagFSLockTake(table_lock());
    uint32_t wanted  = g_links_wanted;
    uint32_t have    = g_link_count;
    uint32_t in_hand = 0;
    while (have + in_hand < g_link_cap && g_links[have + in_hand].block != 0)
        in_hand++;
    TagFSLockDrop(table_lock());
    if (have >= wanted) {
        g_grow_short = false;
        return OK;
    }

    if (LinksReserve(wanted) != OK) {
        FileTableShortSay(wanted, have, ERR_NO_MEMORY);
        return OK;
    }

    /* Spares a pass before this one stamped were seen written before this
     * read, so a flush issued after it covers them too. */
    uint64_t need = in_hand ? VolumeFlushNeed() : 0;
    error_t  rc   = OK;
    while (have + in_hand < wanted) {
        uint32_t block   = 0;
        uint64_t stamped = 0;
        error_t  got     = SpareStamp(TAGFS_FILETBL_MAGIC, &g_image, &block, &stamped);
        if (got != OK) {
            if (FileTableShortOfRoom(got)) {
                FileTableShortSay(wanted, have + in_hand, got);
            } else {
                kprintf("[FileTable] the medium would not take the stamp of a block the file "
                        "table's chain grows by (error %u) — the rows from file id %u on are "
                        "written by the next Settle cycle\n",
                        (unsigned)got, (have + in_hand) * TAGFS_FTABLE_PER_BLOCK);
                rc = got;
            }
            break;
        }
        TagFSLockTake(table_lock());
        g_links[have + in_hand] = (FileTableLink){ .block = block };
        TagFSLockDrop(table_lock());
        in_hand++;
        need = stamped;
    }
    if (in_hand == 0)
        return rc;

    error_t ready = VolumeFlushFor(need);
    if (ready != OK) {
        kprintf("[FileTable] %u block(s) were stamped for the file table and the "
                "medium would not commit its cache (error %d) — they are linked "
                "only once it does\n", in_hand, (int)ready);
        return ready;
    }

    /* The old last link's next changes, and each new link is born owed. */
    TagFSLockTake(table_lock());
    for (; in_hand > 0; in_hand--) {
        g_gen++;
        g_links[g_link_count - 1].edited = g_gen;
        g_links[g_link_count].edited     = g_gen;
        g_link_count++;
    }
    TagFSLockDrop(table_lock());
    return rc;
}

/*
 * Write the first link at or past *at that is owed to the medium, and leave
 * *at on it: OK and *wrote_one when one was written, OK alone when none is owed
 * from *at on, or the medium's refusal. The generation it is written at is
 * read, and its rows are copied out of the map into g_image, in one hold of the
 * state lock — the rows of the files the generation names, no fewer; the image
 * is written with no lock held, and that generation is what the write marks
 * landed.
 */
static error_t FileTableWriteLink(uint32_t* at, bool* wrote_one)
{
    *wrote_one = false;
    TagFSLockTake(table_lock());
    uint32_t i = *at;
    while (i < g_link_count && g_links[i].edited <= g_links[i].landed)
        i++;
    if (i >= g_link_count) {
        TagFSLockDrop(table_lock());
        return OK;
    }
    uint64_t gen   = g_links[i].edited;
    uint32_t block = g_links[i].block;
    g_image.magic       = TAGFS_FILETBL_MAGIC;
    g_image.next_block  = i + 1 < g_link_count ? g_links[i + 1].block : 0;
    g_image.entry_count = TAGFS_FTABLE_PER_BLOCK;
    g_image.reserved    = 0;
    FileMapRowsInto(table_files(), i * TAGFS_FTABLE_PER_BLOCK, TAGFS_FTABLE_PER_BLOCK,
                    g_image.entries);
    TagFSLockDrop(table_lock());

    *at = i;
    error_t wrote = TagFSMetaBlockWrite(block, &g_image);
    if (wrote != OK) {
        kprintf("[FileTable] block %u would not take the file table's rows "
                "%u..%u (error %d) — they are written again by the next Settle cycle\n",
                block, i * TAGFS_FTABLE_PER_BLOCK,
                i * TAGFS_FTABLE_PER_BLOCK + TAGFS_FTABLE_PER_BLOCK - 1, (int)wrote);
        return wrote;
    }

    TagFSLockTake(table_lock());
    if (g_links[i].landed < gen)
        g_links[i].landed = gen;
    TagFSLockDrop(table_lock());
    *wrote_one = true;
    return OK;
}

/*
 * Only the Settle cycle calls this, and it holds the Settle's Scribe for its
 * whole pass: one flusher at a time, so the links, the Spares and g_image are
 * its alone, and two cycles never write one block from two snapshots. A flush
 * asked for any other way — without that Scribe, or with a TagFS lock held,
 * whose holder would then cross the medium with it — writes nothing.
 *
 * Every link owed is written, in chain order; the first refusal ends the pass,
 * so a link to a new block is never followed by nothing but its rows.
 */
error_t file_table_flush(void) {
    bool scribed = ScribeHeldHere(&g_settle_scribe);
    if (!scribed || LockRankAnyHeldHere()) {
        kprintf("[FileTable] a flush was asked for %s — nothing is written\n",
                !scribed ? "outside the Settle cycle, which alone writes the table"
                         : "under a TagFS lock, which its writes would cross the medium holding");
        return ERR_INVALID_STATE;
    }

    error_t  rc      = FileTableGrow();
    error_t  refused = OK;
    uint32_t at      = 0;
    bool     wrote   = true;
    while (refused == OK && wrote) {
        refused = FileTableWriteLink(&at, &wrote);
        if (wrote)
            at++;
    }
    return refused != OK ? refused : rc;
}
