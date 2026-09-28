#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "block_fail.h"
#include "amp.h"
#include "../scribe/scribe.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The chains the registry and the file table grow, and registries that are
// not the volume's
//
// A chain that grows takes a block, and the link naming that block is written
// into the chain's last block. Written before the block itself, one refused
// or lost write leaves a chain on the medium that runs into a block without
// the chain's magic, and every later mount refuses the volume there. The
// growth tests refuse exactly the first write of the block the growth takes —
// the head of the allocator's free list, which a first-fit allocation of one
// block takes — and walk the chain on the medium afterwards.
//
// ‼ EVERY TEST HERE LEAVES THE CHAINS WHOLE, AND WHAT IT GREW IS THE VOLUME'S.
// Whatever a refused write left owed, an unarmed retry pays before the test
// returns, so no stamped block is left in hand; the block the growth took is
// then a link of its chain, which the runner's residue guard counts as the
// volume's own (tests.c). What the volume keeps from this suite is burned file
// ids, a longer registry and file table, and names: a tag is interned for
// good, so each test takes the first names of its own series the volume does
// not know yet, and a stand volume carries a few more of them after each boot.
// ============================================================================

#define STAND_GROW_KEY        "stand_grow"
#define STAND_GROW_VALUE_LEN  240u      /* with the key and the record's head, 256 bytes */
#define STAND_GROW_RECORD     (6u + (uint32_t)sizeof(STAND_GROW_KEY) - 1u + STAND_GROW_VALUE_LEN)
#define STAND_CHAIN_LISTED    64u       /* links a chain walk lists; more are counted */
#define STAND_LOAD_SWEEP      8u        /* allocations of a registry load refused, one per load */
#define STAND_GROWTH_SWEEP    16u       /* allocations of a table growth refused, one per pass */

static uint8_t  s_link[TAGFS_BLOCK_SIZE];
static uint32_t s_links_before[STAND_CHAIN_LISTED];
static uint32_t s_links_after[STAND_CHAIN_LISTED];

// ----------------------------------------------------------------------------
// The chains on the medium
// ----------------------------------------------------------------------------

bool StandChainOnMedium(uint32_t head_volume_block, uint32_t magic, StandChain* out)
{
    TagFSState* fs    = tagfs_get_state();
    uint32_t    block = head_volume_block - fs->layout.data_block;

    out->links     = 0;
    out->last      = 0;
    out->broken    = false;
    out->broken_at = 0;
    for (uint32_t hops = 0; hops < fs->layout.data_blocks; hops++) {
        if (block >= fs->layout.data_blocks) {
            out->broken    = true;
            out->broken_at = block;
            return true;
        }
        if (tagfs_read_block(block, s_link) != OK)
            return false;
        uint32_t says, next;
        memcpy(&says, s_link, sizeof(says));
        memcpy(&next, s_link + sizeof(says), sizeof(next));
        if (says != magic) {
            out->broken    = true;
            out->broken_at = block;
            return true;
        }
        if (out->blocks && out->links < out->capacity)
            out->blocks[out->links] = block;
        out->links++;
        out->last = block;
        if (next == 0)
            break;
        block = next;
    }
    return true;
}

static bool StandRegistryChain(StandChain* out, uint32_t* list)
{
    out->blocks   = list;
    out->capacity = list ? STAND_CHAIN_LISTED : 0;
    return StandChainOnMedium(tagfs_get_state()->layout.tag_registry_block,
                              TAGFS_REGISTRY_MAGIC, out);
}

/* Whether one registry block's records hold `key` and `value`, read the way the
 * registry's own loader reads them: entry_count records, packed, none past the
 * block's data. */
static bool StandRegistryBlockHolds(const TagRegistryBlock* rb, const char* key, const char* value)
{
    size_t   key_len   = strlen(key);
    size_t   value_len = value ? strlen(value) : 0;
    uint32_t at        = 0;

    for (uint16_t e = 0; e < rb->entry_count; e++) {
        const uint8_t* p = rb->data + at;
        if (at + 6u > TAGFS_REGISTRY_DATA_SIZE)
            return false;
        uint8_t  says_key = p[3];
        uint16_t says_value;
        memcpy(&says_value, p + 4, sizeof(says_value));
        uint32_t size = 6u + says_key + says_value;
        if (at + size > TAGFS_REGISTRY_DATA_SIZE)
            return false;
        if (says_key == key_len && says_value == value_len &&
            memcmp(p + 6, key, key_len) == 0 &&
            (value_len == 0 || memcmp(p + 6 + key_len, value, value_len) == 0))
            return true;
        at += size;
    }
    return false;
}

bool StandRegistryHoldsOnMedium(const char* key, const char* value)
{
    TagFSState*             fs    = tagfs_get_state();
    const TagRegistryBlock* rb    = (const TagRegistryBlock*)s_link;
    uint32_t                block = fs->layout.tag_registry_block - fs->layout.data_block;

    for (uint32_t hops = 0; hops < fs->layout.data_blocks; hops++) {
        if (block >= fs->layout.data_blocks || tagfs_read_block(block, s_link) != OK ||
            rb->magic != TAGFS_REGISTRY_MAGIC)
            return false;
        if (StandRegistryBlockHolds(rb, key, value))
            return true;
        if (rb->next_block == 0)
            return false;
        block = rb->next_block;
    }
    return false;
}

/* The block the allocator hands out next: the head of its free list, which a
 * first-fit allocation of one block takes (tagfs_alloc_blocks_internal). */
static bool StandNextBlock(uint32_t* out_block)
{
    TagFSState* fs = tagfs_get_state();

    TagFSLockTake(&fs->lock);
    const FreeExtent* head = fs->block_bitmap.free_list;
    if (head)
        *out_block = head->start;
    TagFSLockDrop(&fs->lock);
    return head != NULL;
}

// ----------------------------------------------------------------------------
// The registry's growth series
// ----------------------------------------------------------------------------

static char s_grow_value[STAND_GROW_VALUE_LEN + 1];
static char s_grow_tag[sizeof(STAND_GROW_KEY) + STAND_GROW_VALUE_LEN + 1];

/* The k-th name of the series, "stand_grow:<k>-ggg…" with 240 bytes of value,
 * so that each one is a 256-byte record of the registry. */
static const char* StandGrowTag(uint32_t k)
{
    ksnprintf(s_grow_value, sizeof(s_grow_value), "%u-", k);
    size_t head = strlen(s_grow_value);
    memset(s_grow_value + head, 'g', STAND_GROW_VALUE_LEN - head);
    s_grow_value[STAND_GROW_VALUE_LEN] = '\0';
    ksnprintf(s_grow_tag, sizeof(s_grow_tag), "%s:%s", STAND_GROW_KEY, s_grow_value);
    return s_grow_tag;
}

/* As many fresh names of the series as the last block of the chain has room
 * for on the medium, and one more — so the registry's next flush grows it by
 * exactly one block. Interned only; nothing is flushed here. */
static bool StandRegistryOverfill(const StandChain* chain, uint32_t* out_first, uint32_t* out_count)
{
    const TagRegistryBlock* tail = (const TagRegistryBlock*)g_test_output;

    if (tagfs_read_block(chain->last, g_test_output) != OK ||
        tail->used_bytes > TAGFS_REGISTRY_DATA_SIZE)
        return false;
    uint32_t count = (TAGFS_REGISTRY_DATA_SIZE - tail->used_bytes) / STAND_GROW_RECORD + 1;
    uint32_t first = 0;
    while (tagfs_tag_lookup(StandGrowTag(first)) != TAGFS_INVALID_TAG_ID)
        first++;
    for (uint32_t i = 0; i < count; i++)
        if (tagfs_tag_intern(StandGrowTag(first + i)) == TAGFS_INVALID_TAG_ID)
            return false;
    *out_first = first;
    *out_count = count;
    return true;
}

/* The unarmed flush after a growth the medium refused, or after one it took:
 * the chain on the medium is whole, one block longer than `before`, ends at
 * the block the growth took, and carries every name of the series the test
 * interned. The verdict stands when it does and is a failure when it does
 * not. */
static TestResult StandRegistryGrowthLanded(TestResult verdict, const StandChain* before,
                                            uint32_t block, uint32_t first, uint32_t count)
{
    StandChain after  = { 0 };
    uint32_t   landed = 0;

    int flushed = tag_registry_flush(tagfs_get_state()->registry);
    if (flushed != 0)
        return TestFailedAt(__FILE__, __LINE__, "an unarmed flush afterwards is taken "
                            "(answered, OK)", flushed, 0);
    if (!StandRegistryChain(&after, NULL) || after.broken)
        return TestFailedAt(__FILE__, __LINE__, "and leaves the registry's chain whole on the "
                            "medium (broken at, none)", after.broken_at, -1);
    if (after.links != before->links + 1 || after.last != block)
        return TestFailedAt(__FILE__, __LINE__, "one block longer, ending at the block the growth "
                            "took (last link, predicted)", after.last, block);
    for (uint32_t i = 0; i < count; i++) {
        StandGrowTag(first + i);
        landed += StandRegistryHoldsOnMedium(STAND_GROW_KEY, s_grow_value);
    }
    if (landed != count)
        return TestFailedAt(__FILE__, __LINE__, "and carrying every name the test interned (on "
                            "the medium, interned)", landed, count);
    return verdict;
}

// ----------------------------------------------------------------------------
// Growth, refused at the new block
// ----------------------------------------------------------------------------

/* The registry is settled, and then made to outgrow its last block by one
 * record; the first write of the block its growth takes is refused. Written
 * link first, the chain on the medium now runs into that block — which holds
 * whatever it held — and the mount would refuse the volume there. Written
 * block first, the refusal leaves the chain as it was. */
static TestResult TestStandRegistryGrowthLinksWhatLanded(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    StandChain  before  = { 0 };
    StandChain  after   = { 0 };
    uint32_t    first   = 0, count = 0, block = 0;
    bool        owed    = false;
    bool        armed   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tag_registry_flush(fs->registry), 0, "an unarmed flush settles the registry first");
    TEST_ASSERT(StandRegistryChain(&before, NULL) && !before.broken,
                "the registry's chain on the medium is whole");
    owed = StandRegistryOverfill(&before, &first, &count);
    TEST_ASSERT(owed, "names that outgrow the registry's last block by one record are interned");
    TEST_ASSERT(StandNextBlock(&block), "the allocator has a block for the growth to take");

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_WRITE, tagfs_block_to_sector(block),
                 TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    int  flushed = tag_registry_flush(fs->registry);
    bool fired   = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(fired, "the first write of the block the growth takes was the write refused");
    TEST_ASSERT(flushed != 0, "and the flush that met the refusal says so");
    TEST_ASSERT(StandRegistryChain(&after, NULL), "the chain is walked on the medium after it");

    TestEvidence("registry chains on the medium that run into a block without the registry's magic",
                 after.broken);
    if (after.broken) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(after.links, before.links, "a refusal that left the chain whole left it as long");
out:
    if (armed)
        BlockFailDisarm();
    if (owed)
        verdict = StandRegistryGrowthLanded(verdict, &before, block, first, count);
    return verdict;
}

/* The same question of the file table. File ids are spent up to the first id
 * past the table's chain on the medium, one file is made with that id, and
 * the sync that has to grow the table for it meets the refusal. The file's
 * row is on the medium once the unarmed sync afterwards has run. */
static TestResult TestStandTableGrowsOntoAStamp(void)
{
    TestResult     verdict = TEST_PASS;
    TagFSState*    fs      = tagfs_get_state();
    StandChain     before  = { 0 };
    StandChain     after   = { 0 };
    uint32_t       file_id = 0, block = 0;
    bool           armed   = false;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(tagfs_sync(), "an unarmed sync settles the file table first");
    TEST_ASSERT(StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &before) &&
                !before.broken, "the file table's chain on the medium is whole");
    uint32_t edge = before.links * TAGFS_FTABLE_PER_BLOCK;
    TEST_ASSERT(tagfs_file_ceiling() <= edge, "no file id has been issued past the table's chain");
    while (tagfs_file_ceiling() < edge) {
        uint32_t spent = 0;
        TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &spent), 0,
                       "a file id is spent on the way to the chain's end");
        TEST_ASSERT_EQ(tagfs_delete_file(spent), 0, "and its file deleted");
    }
    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0, "a file is created");
    TEST_ASSERT_EQ(file_id, edge, "with the first id past the table's chain");
    TEST_ASSERT(StandNextBlock(&block), "the allocator has a block for the growth to take");

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_WRITE, tagfs_block_to_sector(block),
                 TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    tagfs_sync();
    bool fired = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(fired, "the first write of the block the table's growth takes was the write refused");
    TEST_ASSERT(StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &after),
                "the chain is walked on the medium after it");

    TestEvidence("file table chains on the medium that run into a block without the table's magic",
                 after.broken);
    if (after.broken) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(after.links, before.links, "a refusal that left the chain whole left it as long");
out:
    if (armed)
        BlockFailDisarm();
    if (file_id) {
        uint32_t       meta_block = 0, meta_offset = 0;
        FileTableEntry row        = { 0, 0 };
        error_t        again      = tagfs_sync();
        if (again != OK)
            verdict = TestFailedAt(__FILE__, __LINE__, "an unarmed sync afterwards is taken "
                                   "(error, OK)", again, OK);
        else if (!StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &after) ||
                 after.broken || after.links <= before.links)
            verdict = TestFailedAt(__FILE__, __LINE__, "and grows the table's chain whole on the "
                                   "medium (links, links before)", after.links, before.links);
        else if (!StandTableRowOnMedium(file_id, &row, NULL) ||
                 file_table_lookup(file_id, &meta_block, &meta_offset) != 0 ||
                 row.meta_block != meta_block || row.meta_offset != meta_offset)
            verdict = TestFailedAt(__FILE__, __LINE__, "with the file's row on it as memory holds "
                                   "it (on the medium, in memory)", row.meta_block, meta_block);
        tagfs_delete_file(file_id);
    }
    return verdict;
}

/* CR-11's guard. The file table is made to need one more block — a row past
 * its chain, set to nothing, is all a growth asks for — and its flush is run
 * with the n-th allocation it makes refused, n = 1, 2, … until a pass makes
 * fewer than n. The flush is asked directly, under the sync's own Scribe, so
 * the sweep counts the table's allocations and nobody else's; a pass that grew
 * the chain is followed by a row past its new end. None of it may leave a
 * block taken that the chain does not name: the slot a link is kept in is
 * reserved before its block is taken, so nothing refused after the take can
 * strand it. Counted once an unarmed sync has written whatever the passes left
 * owed — a link attached in memory whose write is still to come is not a
 * stranded block. The table is the only taker of blocks in a pass, so a block
 * a pass took is the head of the free list it began with, and one of those the
 * chain does not name at the end is given back. */
static TestResult TestStandTableGrowthGivesBackItsGround(void)
{
    TestResult  verdict  = TEST_PASS;
    TagFSState* fs       = tagfs_get_state();
    uint8_t     me       = amp_get_core_index();
    StandChain  chain    = { 0 };
    VolumeTally before   = { 0 };
    VolumeTally after    = { 0 };
    uint32_t    taken[STAND_GROWTH_SWEEP];   /* the block each pass's growth took, 0 for none */
    uint32_t    passes   = 0;
    uint64_t    stranded = 0;
    bool        ran_out  = false;
    const char* aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(tagfs_sync(), "an unarmed sync settles the file table first");
    TEST_ASSERT(VolumeTallyTake(&before), "the volume is counted before the sweep");

    for (uint32_t n = 1; n <= STAND_GROWTH_SWEEP && !ran_out; n++) {
        uint32_t block = 0;
        uint64_t cover = 0;
        error_t  said  = OK;

        TEST_ASSERT(StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &chain) &&
                    !chain.broken, "the file table's chain on the medium is whole");
        TagFSLockTake(&fs->lock);
        int named = file_table_update(chain.links * TAGFS_FTABLE_PER_BLOCK, 0, 0);
        TagFSLockDrop(&fs->lock);
        TEST_ASSERT_EQ(named, 0, "a row past the chain makes the table need one more block");
        TEST_ASSERT(StandNextBlock(&block), "the allocator has a block for the growth to take");
        TEST_ASSERT_EQ(ScribeWait(&g_sync_scribe, ScribeAsk(&g_sync_scribe), &cover, &said),
                       SCRIBE_MINE, "the test holds the sync's Scribe, as the sync does");

        HeapFailArm(me, n);
        file_table_flush();
        ran_out = !HeapFailFired();
        HeapFailDisarm();
        ScribeGive(&g_sync_scribe, cover, OK);
        taken[passes++] = StandBlockInUse(block) ? block : 0;
    }
    TEST_ASSERT(ran_out, "the sweep reaches a pass that makes fewer allocations than it refuses at");

out:
    if (passes) {
        error_t again = tagfs_sync();
        chain.blocks   = s_links_after;
        chain.capacity = STAND_CHAIN_LISTED;
        if (again != OK)
            verdict = TestFailedAt(__FILE__, __LINE__, "an unarmed sync afterwards writes what "
                                   "the passes left owed (answered, OK)", again, OK);
        else if (!StandChainOnMedium(fs->layout.file_table_block, TAGFS_FILETBL_MAGIC, &chain) ||
                 chain.broken || chain.links > STAND_CHAIN_LISTED)
            verdict = TestFailedAt(__FILE__, __LINE__, "and leaves the table's chain whole and "
                                   "listed on the medium (links, listed)", chain.links,
                                   STAND_CHAIN_LISTED);
        else if (!VolumeTallyTake(&after))
            verdict = TestFailedAt(__FILE__, __LINE__, "the volume is counted after the sweep",
                                   0, 1);
        else {
            stranded = after.held > before.held ? after.held - before.held : 0;
            for (uint32_t i = 0; i < passes; i++) {
                bool linked = false;
                for (uint32_t j = 0; j < chain.links && !linked; j++)
                    linked = s_links_after[j] == taken[i];
                if (taken[i] && !linked && StandBlockInUse(taken[i]))
                    tagfs_free_blocks(taken[i], 1);
            }
            if (verdict == TEST_PASS) {
                TestEvidence("blocks a growth of the file table took and left named by nothing",
                             stranded);
                if (stranded)
                    verdict = TEST_DEFECT;
            }
        }
    }
    return verdict;
}

// ----------------------------------------------------------------------------
// Registries that are not the volume's
// ----------------------------------------------------------------------------

/* Settle the volume's registry, and give back every block the chain `before`
 * listed that the medium's chain no longer runs through and the allocator
 * still calls taken: the links a registry that lost hold of its own chain
 * left behind when its next flush took new ones. */
static void StandRegistryGiveBack(const StandChain* before)
{
    StandChain after = { 0 };

    tag_registry_flush(tagfs_get_state()->registry);
    if (!StandRegistryChain(&after, s_links_after) || after.broken)
        return;
    uint32_t listed = before->links < STAND_CHAIN_LISTED ? before->links : STAND_CHAIN_LISTED;
    uint32_t still  = after.links < STAND_CHAIN_LISTED ? after.links : STAND_CHAIN_LISTED;
    for (uint32_t i = 0; i < listed; i++) {
        bool linked = false;
        for (uint32_t j = 0; j < still && !linked; j++)
            linked = s_links_after[j] == before->blocks[i];
        if (!linked && StandBlockInUse(before->blocks[i]))
            tagfs_free_blocks(before->blocks[i], 1);
    }
}

/* The volume's registry settled and its chain on the medium listed in
 * s_links_before — grown to two blocks first when it has one, since a chain
 * of one block has no link a registry could lose hold of. */
static TestResult StandRegistrySettledAndListed(StandChain* before)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint32_t    first   = 0, count = 0;

    TEST_ASSERT_EQ(tag_registry_flush(fs->registry), 0, "an unarmed flush settles the registry first");
    TEST_ASSERT(StandRegistryChain(before, s_links_before) && !before->broken,
                "the registry's chain on the medium is whole");
    if (before->links < 2) {
        TEST_ASSERT(StandRegistryOverfill(before, &first, &count),
                    "names that outgrow the registry's only block are interned");
        TEST_ASSERT_EQ(tag_registry_flush(fs->registry), 0, "and flushed");
        TEST_ASSERT(StandRegistryChain(before, s_links_before) && !before->broken &&
                    before->links >= 2, "so the chain on the medium is two blocks long");
    }
    TEST_ASSERT(before->links <= STAND_CHAIN_LISTED, "every link of the chain is listed");
out:
    return verdict;
}

/* A registry of the test's own reads the volume's off the medium, takes a
 * name of its own and is thrown away; then a create that interns nothing new
 * runs on the volume. Nothing the scratch registry did was the volume's, so
 * the volume's chain on the medium must be the one it was, block for block. */
static TestResult TestStandRegistryScratchLeavesTheVolumeAlone(void)
{
    TestResult   verdict = TEST_PASS;
    TagRegistry* scratch = NULL;
    bool         built   = false;
    uint32_t     file_id = 0;
    StandChain   before  = { 0 };
    StandChain   after   = { 0 };

    verdict = StandRegistrySettledAndListed(&before);
    if (verdict != TEST_PASS)
        goto out;

    scratch = kmalloc(sizeof(TagRegistry));
    TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
    built = true;
    TEST_ASSERT_EQ(tag_registry_load(scratch, before.blocks[0]), 0,
                   "it reads the volume's registry off the medium");
    TEST_ASSERT(tag_registry_intern(scratch, "stand_scratch", NULL) != TAGFS_INVALID_TAG_ID,
                "and takes a name of its own");
    tag_registry_destroy(scratch);
    built = false;
    kfree(scratch);
    scratch = NULL;

    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0,
                   "a create that interns nothing new runs on the volume");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "and its file is deleted");
    file_id = 0;
    TEST_ASSERT(StandRegistryChain(&after, s_links_after) && !after.broken,
                "the registry's chain on the medium is whole after it");

    bool moved = after.links != before.links;
    for (uint32_t i = 0; i < before.links && !moved; i++)
        moved = s_links_after[i] != s_links_before[i];
    TestEvidence("registry chains on the medium a scratch registry moved by living and dying", moved);
    if (moved)
        verdict = TEST_DEFECT;
out:
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    if (file_id)
        tagfs_delete_file(file_id);
    if (before.links)
        StandRegistryGiveBack(&before);
    return verdict;
}

/* A registry of the test's own reads the volume's off the medium with its
 * n-th allocation refused, n = 1..8, until one answers OK. A load that could
 * not hold what the medium carries has to refuse — the volume must not mount
 * missing tags — so one that answers OK is the defect. The sweep's registries
 * are the test's; what they did to the volume's is given back as the scratch
 * test gives it back. */
static TestResult TestStandRegistryLoadRefusesWhatItCannotHold(void)
{
    TestResult   verdict  = TEST_PASS;
    uint8_t      me       = amp_get_core_index();
    TagRegistry* scratch  = NULL;
    bool         built    = false;
    bool         armed    = false;
    bool         answered = false;
    StandChain   before   = { 0 };

    verdict = StandRegistrySettledAndListed(&before);
    if (verdict != TEST_PASS)
        goto out;

    for (uint32_t n = 1; n <= STAND_LOAD_SWEEP && !answered; n++) {
        scratch = kmalloc(sizeof(TagRegistry));
        TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
        TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
        built = true;

        HeapFailArm(me, n);
        armed = true;
        int  loaded = tag_registry_load(scratch, before.blocks[0]);
        bool fired  = HeapFailFired();
        HeapFailDisarm();
        armed = false;

        tag_registry_destroy(scratch);
        built = false;
        kfree(scratch);
        scratch = NULL;
        TEST_ASSERT(fired, "the load made the allocation the plan refused");
        answered = loaded == 0;
    }

    TestEvidence("registry loads with an allocation refused that answered OK, stopping at the first",
                 answered);
    if (answered)
        verdict = TEST_DEFECT;
out:
    if (armed)
        HeapFailDisarm();
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    if (before.links)
        StandRegistryGiveBack(&before);
    return verdict;
}

// ============================================================================
// The suite — the tests that grow a chain first, then the ones that lean on a
// chain of more than one block
// ============================================================================

static const TestCase g_stand_chain_tests[] = {
    { .name  = "stand_registry_growth_links_what_landed",
      .func  = TestStandRegistryGrowthLinksWhatLanded,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.1",
      .caveat_defect = "a registry that grows writes the link to its new block before the block "
                       "itself, so one refused or lost write leaves a chain the mount refuses on "
                       "every boot" },
    { .name  = "stand_table_grows_onto_a_stamp", .func = TestStandTableGrowsOntoAStamp,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a sync that lengthens the file table writes the link to its new block "
                       "before the block, so a refused write of that block leaves a chain the "
                       "next mount refuses" },
    { .name  = "stand_table_growth_gives_back_its_ground",
      .func  = TestStandTableGrowthGivesBackItsGround,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a growth of the file table refused memory after it has taken its block "
                       "returns with the block taken and named by nothing, for good (CR-11)" },
    { .name  = "stand_registry_scratch_leaves_the_volume_alone",
      .func  = TestStandRegistryScratchLeavesTheVolumeAlone,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.1",
      .caveat_defect = "a registry built and thrown away beside the volume's shares the volume's "
                       "chain and its mark of what is owed, so its end makes the volume's next "
                       "flush take new blocks and leave the old ones owned by nothing" },
    { .name  = "stand_registry_load_refuses_what_it_cannot_hold",
      .func  = TestStandRegistryLoadRefusesWhatItCannotHold,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.1",
      .caveat_defect = "a registry load whose allocation is refused drops what it could not hold "
                       "and answers OK, so the volume mounts missing tags its medium carries" },
};

const TestSuite g_stand_chain_suite = {
    .tests = g_stand_chain_tests, .count = ARRAY_SIZE(g_stand_chain_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
