#include "file_table.h"
#include "../scribe/scribe.h"

/*
 * ‼ THE TABLE'S LOCK GUARDS MEMORY, AND NOTHING ELSE IS DONE UNDER IT.
 *
 * A leaf: create, delete, rename, tagging and defrag take it under the
 * volume's state lock, a write's commit under its file's writer lock, and
 * under it no command goes to the medium, nothing is allocated and no other
 * TagFS lock is taken. The flush used to hold it across every block it wrote
 * and to take a block for its chain under it — so the state lock was taken
 * under the table while a create took the table under state (MP-D4), and a
 * core that only wanted to look a file up spun with its interrupts off for as
 * long as the medium took. Now the flush copies one block's rows out under
 * the lock and writes them with no lock held.
 */
static TagFSLock g_lock = TAGFS_LOCK_INIT("table", LOCK_RANK_LEAF);

static FileTableEntry* g_entries  = NULL;   /* the rows, by file id */
static uint32_t        g_capacity = 0;      /* rows memory holds; a whole number of blocks' worth */

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
 * change to what the block carries — one of its rows, or its link to the next
 * block — and `landed` the generation the image last written to it was taken
 * at, so the block is owed to the medium exactly while edited > landed. A row
 * changed while its block is being written moves `edited` past what that write
 * carried, and the block stays owed: a write is never taken for more than the
 * snapshot it wrote.
 *
 * Past g_link_count, a slot whose block is not 0 is a Spare the flush has
 * stamped for the chain and not linked yet — the medium would not yet commit
 * the stamp (see FileTableGrow). The links and the slots change only in the
 * flush, which the volume's sync runs under its Scribe, and in the mount's
 * walk, with the door closed; update and delete only mark links, under g_lock.
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
static uint64_t       g_gen          = 0;   /* one step per change, taken under g_lock */

/* The one block image there is: the mount's walk reads into it, and the flush
 * stamps a Spare and snapshots a link through it — the flush alone, under the
 * sync's Scribe, so no two of them ever share it. */
static FileTableBlock g_image;

/*
 * Make memory hold at least `want` rows, called with g_lock NOT held: the new
 * array is allocated before the lock is taken and the old one given back after
 * it is let go. Two cores growing it at once each allocate, and the one that
 * finds it already grown far enough when it takes the lock gives its own back.
 *
 * The rows grow twice over at a time. A table grown by one block's rows would
 * copy the whole of itself every 510 creates; doubled, what a create pays for
 * the copying stays the same however many files there are. What the MEDIUM
 * holds is not tied to this: the chain grows a block at a time, as far as the
 * rows named need (g_links_wanted), never as far as memory has reserved.
 */
static int table_grow_to(uint64_t want)
{
    TagFSLockTake(&g_lock);
    uint64_t have = g_capacity;
    TagFSLockDrop(&g_lock);
    if (want <= have)
        return 0;

    uint64_t rows = have * 2 > want ? have * 2 : want;
    rows = (rows + TAGFS_FTABLE_PER_BLOCK - 1) / TAGFS_FTABLE_PER_BLOCK * TAGFS_FTABLE_PER_BLOCK;
    if (rows > 0xFFFFFFFFu)
        return -1;
    FileTableEntry* grown = kmalloc(sizeof(FileTableEntry) * rows);
    if (!grown)
        return -1;

    FileTableEntry* spare = grown;
    TagFSLockTake(&g_lock);
    if (rows > g_capacity) {
        if (g_entries)
            memcpy(grown, g_entries, sizeof(FileTableEntry) * g_capacity);
        memset(grown + g_capacity, 0, sizeof(FileTableEntry) * (rows - g_capacity));
        spare      = g_entries;
        g_entries  = grown;
        g_capacity = (uint32_t)rows;
    }
    TagFSLockDrop(&g_lock);
    if (spare)
        kfree(spare);
    return 0;
}

/*
 * Room for `want` links, made BEFORE any block is taken for one: a growth
 * that has taken its block never needs memory to keep it, so no refused
 * allocation can strand a block the allocator already handed out (CR-11: the
 * chain's record used to be lengthened after tagfs_alloc_blocks, and a refusal
 * there returned with the block taken and named by nothing, for good).
 *
 * Called with no lock held — by the flush, or by the mount's walk — and the
 * array swapped under g_lock, since update and delete mark links under it. A
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

    TagFSLockTake(&g_lock);
    if (g_links)
        memcpy(grown, g_links, sizeof(FileTableLink) * g_link_cap);
    memset(grown + g_link_cap, 0, sizeof(FileTableLink) * (want - g_link_cap));
    FileTableLink* old = g_links;
    g_links    = grown;
    g_link_cap = want;
    TagFSLockDrop(&g_lock);
    if (old)
        kfree(old);
    return OK;
}

static void table_release(void) {
    if (g_entries) kfree(g_entries);
    if (g_links)   kfree(g_links);
    g_entries      = NULL;
    g_capacity     = 0;
    g_links        = NULL;
    g_link_count   = 0;
    g_link_cap     = 0;
    g_links_wanted = 0;
    g_gen          = 0;
}

int file_table_init(uint32_t first_block) {
    table_release();

    /*
     * ‼ THE ONLY BOUND ON THIS WALK IS A TERMINATION PROOF, NOT A LENGTH.
     *
     * A chain of distinct blocks cannot be longer than the data run it lives
     * in, so a walk that takes that many hops has come back to a block it has
     * already been to. The hop count proves termination and catches the cycle
     * together, and a next_block outside the run is refused before it is read
     * — nothing under tagfs_read_block bounds a block number.
     */
    TagFSState* fs  = tagfs_get_state();
    uint32_t    run = fs ? fs->layout.data_blocks : 0;
    if (run == 0) {
        kprintf("[FileTable] this volume states a data run of no blocks at "
                "all, so there is nowhere for a file table to be — it is not "
                "mounted\n");
        return -1;
    }

    uint32_t block = first_block;
    uint32_t hops  = 0;

    for (;;) {
        if (block >= run) {
            kprintf("[FileTable] the chain points at block %u and this "
                    "volume's data run is %u blocks — that is outside the "
                    "volume, so it is not mounted\n", block, run);
            table_release();
            return -1;
        }

        /* Same rule as the metadata pool: a table that will not read, or a
         * block holding something that is not a table, means this volume does
         * not mount. Breaking out of the loop left the entries zeroed, which
         * reads as "this volume has no files" — and the writes that follow
         * take the blocks those files are still using.
         *
         * ‼ Tested as `!= OK`, because tagfs_read_block returns an error_t
         * whose failures are POSITIVE and the `< 0` this used to ask was never
         * once true. The refusal below only ever fired on the magic. */
        if (tagfs_read_block(block, &g_image) != OK) {
            kprintf("[FileTable] block %u would not read — this volume's file "
                    "table cannot be reached, so it is not mounted\n", block);
            table_release();
            return -1;
        }

        if (g_image.magic != TAGFS_FILETBL_MAGIC) {
            kprintf("[FileTable] block %u holds 0x%08x where a file table "
                    "should be — this volume is not mounted\n", block, g_image.magic);
            table_release();
            return -1;
        }

        uint32_t entry_offset = g_link_count * TAGFS_FTABLE_PER_BLOCK;

        if ((g_link_count == g_link_cap &&
             LinksReserve(g_link_cap ? g_link_cap * 2 : 1) != OK) ||
            table_grow_to((uint64_t)entry_offset + TAGFS_FTABLE_PER_BLOCK) != 0) {
            kprintf("[FileTable] there is no memory to hold this volume's file "
                    "table at %u entries — it is not mounted\n",
                    entry_offset + TAGFS_FTABLE_PER_BLOCK);
            table_release();
            return -1;
        }
        g_links[g_link_count++] = (FileTableLink){ .block = block };

        uint32_t count = g_image.entry_count;
        if (count > TAGFS_FTABLE_PER_BLOCK) {
            count = TAGFS_FTABLE_PER_BLOCK;
        }
        memcpy(g_entries + entry_offset, g_image.entries, sizeof(FileTableEntry) * count);

        if (g_image.next_block == 0) break;

        if (++hops >= run) {
            kprintf("[FileTable] this volume's file table chain has taken %u "
                    "hops in a data run of %u blocks, so it is going round in "
                    "a circle — it is not mounted\n", hops, run);
            table_release();
            return -1;
        }
        block = g_image.next_block;
    }

    g_links_wanted = g_link_count;
    debug_printf("[FileTable] initialized: first_block=%u blocks=%u capacity=%u\n",
                 first_block, g_link_count, g_capacity);
    return 0;
}

void file_table_shutdown(void) {
    table_release();
    debug_printf("[FileTable] shutdown\n");
}

int file_table_lookup(uint32_t file_id, uint32_t* out_block, uint32_t* out_offset) {
    TagFSLockTake(&g_lock);

    if (file_id >= g_capacity || g_entries[file_id].meta_block == 0) {
        TagFSLockDrop(&g_lock);
        return -1;
    }

    *out_block  = g_entries[file_id].meta_block;
    *out_offset = g_entries[file_id].meta_offset;

    TagFSLockDrop(&g_lock);
    return 0;
}

int file_table_update(uint32_t file_id, uint32_t meta_block, uint32_t meta_offset) {
    if (table_grow_to((uint64_t)file_id + 1) != 0) {
        kprintf("[FileTable] there is no memory to hold a row for file id %u "
                "— the row is not kept, and the change that named it is "
                "refused\n", file_id);
        return -1;
    }

    /* A row past the chain marks no link: the link that will carry it is
     * attached by the flush already owed to the medium (FileTableGrow). */
    uint32_t link = file_id / TAGFS_FTABLE_PER_BLOCK;
    TagFSLockTake(&g_lock);
    g_entries[file_id].meta_block  = meta_block;
    g_entries[file_id].meta_offset = meta_offset;
    g_gen++;
    if (link < g_link_count)
        g_links[link].edited = g_gen;
    if (link >= g_links_wanted)
        g_links_wanted = link + 1;
    TagFSLockDrop(&g_lock);
    return 0;
}

int file_table_delete(uint32_t file_id) {
    uint32_t link = file_id / TAGFS_FTABLE_PER_BLOCK;

    TagFSLockTake(&g_lock);

    if (file_id >= g_capacity) {
        TagFSLockDrop(&g_lock);
        return -1;
    }

    g_entries[file_id].meta_block  = 0;
    g_entries[file_id].meta_offset = 0;
    g_gen++;
    if (link < g_link_count)
        g_links[link].edited = g_gen;

    TagFSLockDrop(&g_lock);
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
 * is allocated, written or waited for under g_lock. A refused stamp gives its
 * block back; a flush the medium will not keep leaves the stamped Spares in
 * their slots, unlinked, for the next pass to link once a flush is kept.
 * Rows past the chain stay in memory until then.
 */
static int FileTableGrow(void)
{
    TagFSLockTake(&g_lock);
    uint32_t wanted  = g_links_wanted;
    uint32_t have    = g_link_count;
    uint32_t in_hand = 0;
    while (have + in_hand < g_link_cap && g_links[have + in_hand].block != 0)
        in_hand++;
    TagFSLockDrop(&g_lock);
    if (have >= wanted)
        return 0;

    if (LinksReserve(wanted) != OK) {
        kprintf("[FileTable] there is no memory to hold the %u links the file "
                "table's rows need — its chain stays %u block(s) long, and the "
                "rows from file id %u on stay in memory until a later sync\n",
                wanted, have, have * TAGFS_FTABLE_PER_BLOCK);
        return -1;
    }

    /* Spares a pass before this one stamped were seen written before this
     * read, so a flush issued after it covers them too. */
    uint64_t need = in_hand ? VolumeFlushNeed() : 0;
    int      rc   = 0;
    while (have + in_hand < wanted) {
        uint32_t block   = 0;
        uint64_t stamped = 0;
        error_t  got     = SpareStamp(TAGFS_FILETBL_MAGIC, &g_image, &block, &stamped);
        if (got != OK) {
            kprintf("[FileTable] the file table needs %u more block(s) to hold "
                    "file ids up to %u and the volume gave none (error %d) — "
                    "the rows from file id %u on stay in memory until a later "
                    "sync\n", wanted - have - in_hand,
                    wanted * TAGFS_FTABLE_PER_BLOCK - 1, (int)got,
                    (have + in_hand) * TAGFS_FTABLE_PER_BLOCK);
            rc = -1;
            break;
        }
        TagFSLockTake(&g_lock);
        g_links[have + in_hand] = (FileTableLink){ .block = block };
        TagFSLockDrop(&g_lock);
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
        return -1;
    }

    /* The old last link's next changes, and each new link is born owed. */
    TagFSLockTake(&g_lock);
    for (; in_hand > 0; in_hand--) {
        g_gen++;
        g_links[g_link_count - 1].edited = g_gen;
        g_links[g_link_count].edited     = g_gen;
        g_link_count++;
    }
    TagFSLockDrop(&g_lock);
    return rc;
}

/*
 * Write the first link at or past *at that is owed to the medium, and leave
 * *at on it: 1 when one was written, 0 when none is owed from *at on, -1 when
 * the medium refused it. Its image is taken under g_lock into g_image and
 * written with no lock held; the generation it was taken at is what the write
 * marks landed.
 */
static int FileTableWriteLink(uint32_t* at)
{
    TagFSLockTake(&g_lock);
    uint32_t i = *at;
    while (i < g_link_count && g_links[i].edited <= g_links[i].landed)
        i++;
    if (i >= g_link_count) {
        TagFSLockDrop(&g_lock);
        return 0;
    }
    uint64_t gen   = g_links[i].edited;
    uint32_t block = g_links[i].block;
    g_image.magic       = TAGFS_FILETBL_MAGIC;
    g_image.next_block  = i + 1 < g_link_count ? g_links[i + 1].block : 0;
    g_image.entry_count = TAGFS_FTABLE_PER_BLOCK;
    g_image.reserved    = 0;
    memcpy(g_image.entries, g_entries + (size_t)i * TAGFS_FTABLE_PER_BLOCK,
           sizeof(g_image.entries));
    TagFSLockDrop(&g_lock);

    *at = i;
    error_t wrote = tagfs_write_block(block, &g_image);
    if (wrote != OK) {
        kprintf("[FileTable] block %u would not take the file table's rows "
                "%u..%u (error %d) — they are written again at the next sync\n",
                block, i * TAGFS_FTABLE_PER_BLOCK,
                i * TAGFS_FTABLE_PER_BLOCK + TAGFS_FTABLE_PER_BLOCK - 1, (int)wrote);
        return -1;
    }

    TagFSLockTake(&g_lock);
    if (g_links[i].landed < gen)
        g_links[i].landed = gen;
    TagFSLockDrop(&g_lock);
    return 1;
}

/*
 * Only the volume's sync calls this, and it holds the sync's Scribe for its
 * whole pass: one flusher at a time, so the links, the Spares and g_image are
 * its alone, and two syncs never write one block from two snapshots. A flush
 * asked for any other way — without that Scribe, or with a TagFS lock held,
 * whose holder would then cross the medium with it — writes nothing.
 *
 * Every link owed is written, in chain order; the first refusal ends the pass,
 * so a link to a new block is never followed by nothing but its rows.
 */
int file_table_flush(void) {
    bool scribed = ScribeHeldHere(&g_sync_scribe);
    if (!scribed || LockRankAnyHeldHere()) {
        kprintf("[FileTable] a flush was asked for %s — nothing is written\n",
                !scribed ? "outside the volume's sync, which alone writes the table"
                         : "under a TagFS lock, which its writes would cross the medium holding");
        return -1;
    }

    int      rc = FileTableGrow();
    uint32_t at = 0;
    int      wrote;
    while ((wrote = FileTableWriteLink(&at)) > 0)
        at++;
    return wrote < 0 ? -1 : rc;
}
