#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "block_fail.h"
#include "amp.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The chains the registry and the file table grow, registries that are not the
// volume's, and what a registry hands out
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

/* The registry made to outgrow its last block again, and this time the flush
 * that proves the new block's stamp is refused. Nothing may name the block
 * then — its stamp is not proven — and nothing may lose it either: it is
 * taken and stamped, so a flush that forgot it would leave it marked used and
 * owned by nothing for the life of the volume (CR-11). The refused flush keeps
 * it in hand, and the unarmed flush after it links that very block. */
static TestResult TestStandRegistryFlushKeepsItsBlock(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    StandChain  before  = { 0 };
    StandChain  after   = { 0 };
    uint32_t    first   = 0, count = 0, block = 0, landed = 0;
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

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_FLUSH, 0, 0, 1);
    armed = true;
    int  flushed = tag_registry_flush(fs->registry);
    bool fired   = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(fired, "the flush that proves the new block's stamp was the one refused");
    TEST_ASSERT(flushed != 0, "and the registry's flush that met it says so");
    TEST_ASSERT(StandRegistryChain(&after, NULL) && !after.broken && after.links == before.links,
                "nothing on the medium names the new block: the chain is as long as it was");
    TEST_ASSERT(StandBlockInUse(block), "and the block the growth took is still taken");

    owed = false;
    TEST_ASSERT_EQ(tag_registry_flush(fs->registry), 0, "an unarmed flush afterwards is taken");
    TEST_ASSERT(StandRegistryChain(&after, NULL) && !after.broken && after.links == before.links + 1,
                "and grows the registry's chain on the medium by one block");
    for (uint32_t i = 0; i < count; i++) {
        StandGrowTag(first + i);
        landed += StandRegistryHoldsOnMedium(STAND_GROW_KEY, s_grow_value);
    }
    TEST_ASSERT_EQ(landed, count, "carrying every name the test interned");

    bool lost = after.last != block;
    TestEvidence("blocks a refused registry growth took that its retry did not link", lost);
    if (lost) {
        verdict = TEST_DEFECT;
        if (StandBlockInUse(block))
            tagfs_free_blocks(block, 1);    /* nothing names it: the test gives it back */
    }
out:
    if (armed)
        BlockFailDisarm();
    if (owed)
        tag_registry_flush(fs->registry);
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

// ----------------------------------------------------------------------------
// What a registry hands out
// ----------------------------------------------------------------------------

#define STAND_GROUP_FIRST 9u    /* one more than a new key group's first array holds */

static char     s_group_key[24];
static char     s_group_tag[40];
static uint16_t s_group_ids[2 * STAND_GROUP_FIRST];

/* "stand_group<k>:<v>"; its key, "stand_group<k>", is left in s_group_key. */
static const char* StandGroupTag(uint32_t k, uint32_t v)
{
    ksnprintf(s_group_key, sizeof(s_group_key), "stand_group%u", k);
    ksnprintf(s_group_tag, sizeof(s_group_tag), "%s:%u", s_group_key, v);
    return s_group_tag;
}

/* The ids of every tag of one key, asked for by a query of `key:...`, are the
 * caller's copy. The registry's own array of a key is freed by the key's next
 * tag once it outgrows it — on any core, while a query reads — so an array
 * the registry handed out would be read after it was freed. The copy is
 * written over by the test, which the registry must not notice; then the key
 * grows by nine, moving the registry's array, and the copy the test holds
 * still says what it said. A key of the series no tag has yet is taken, since
 * a tag is interned for good. */
static TestResult TestStandRegistryKeyIdsAreCopies(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    TagIdList   copy    = { 0 };
    TagIdList   again   = { 0 };
    bool        ours    = true;
    uint32_t    k       = 0;

    while (tagfs_tag_lookup(StandGroupTag(k, 0)) != TAGFS_INVALID_TAG_ID)
        k++;
    for (uint32_t v = 0; v < STAND_GROUP_FIRST; v++) {
        s_group_ids[v] = tagfs_tag_intern(StandGroupTag(k, v));
        TEST_ASSERT(s_group_ids[v] != TAGFS_INVALID_TAG_ID, "a tag of a key no tag had is interned");
    }
    TEST_ASSERT_OK(TagRegistryKeyIds(fs->registry, s_group_key, &copy), "the key's ids are asked for");
    TEST_ASSERT_EQ(copy.count, STAND_GROUP_FIRST, "and all of them come back");
    TEST_ASSERT(memcmp(copy.ids, s_group_ids, sizeof(uint16_t) * STAND_GROUP_FIRST) == 0,
                "in the order they were interned");

    for (uint32_t v = 0; v < STAND_GROUP_FIRST; v++)
        copy.ids[v] = TAGFS_INVALID_TAG_ID;
    TEST_ASSERT_OK(TagRegistryKeyIds(fs->registry, s_group_key, &again),
                   "the key's ids are asked for again");
    bool shared = again.count != STAND_GROUP_FIRST ||
                  memcmp(again.ids, s_group_ids, sizeof(uint16_t) * STAND_GROUP_FIRST) != 0;
    TestEvidence("copies of a key's tag ids that were the registry's own array", shared);
    if (shared) {
        /* What the test wrote over was the registry's: put back, and freed by
         * nobody but the registry. */
        memcpy(copy.ids, s_group_ids, sizeof(uint16_t) * STAND_GROUP_FIRST);
        ours    = false;
        verdict = TEST_DEFECT;
        goto out;
    }

    memcpy(copy.ids, s_group_ids, sizeof(uint16_t) * STAND_GROUP_FIRST);
    kfree(again.ids);
    again.ids = NULL;
    for (uint32_t v = STAND_GROUP_FIRST; v < 2 * STAND_GROUP_FIRST; v++) {
        s_group_ids[v] = tagfs_tag_intern(StandGroupTag(k, v));
        TEST_ASSERT(s_group_ids[v] != TAGFS_INVALID_TAG_ID, "the key grows by nine more");
    }
    TEST_ASSERT(copy.count == STAND_GROUP_FIRST &&
                memcmp(copy.ids, s_group_ids, sizeof(uint16_t) * STAND_GROUP_FIRST) == 0,
                "and the copy taken before says what it said");
    TEST_ASSERT_OK(TagRegistryKeyIds(fs->registry, s_group_key, &again), "a copy taken now");
    TEST_ASSERT(again.count == 2 * STAND_GROUP_FIRST &&
                memcmp(again.ids, s_group_ids, sizeof(uint16_t) * 2 * STAND_GROUP_FIRST) == 0,
                "holds all eighteen, in order");
out:
    if (ours) {
        kfree(copy.ids);
        kfree(again.ids);
    }
    return verdict;
}

// ============================================================================
// The suite — the tests that grow a chain first, then the ones that lean on a
// chain of more than one block, then what a registry hands out
// ============================================================================

static const TestCase g_stand_chain_tests[] = {
    { .name  = "stand_registry_growth_links_what_landed",
      .func  = TestStandRegistryGrowthLinksWhatLanded,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a registry that grows writes the link to its new block before the block "
                       "itself, so one refused or lost write leaves a chain the mount refuses on "
                       "every boot" },
    { .name  = "stand_registry_flush_keeps_its_block",
      .func  = TestStandRegistryFlushKeepsItsBlock,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a registry flush that fails after taking a block for its chain forgets "
                       "the block, which stays marked used and owned by nothing for the life of "
                       "the volume" },
    { .name  = "stand_table_grows_onto_a_stamp", .func = TestStandTableGrowsOntoAStamp,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.1",
      .caveat_defect = "a sync that lengthens the file table writes the link to its new block "
                       "before the block, so a refused write of that block leaves a chain the "
                       "next mount refuses" },
    { .name  = "stand_registry_scratch_leaves_the_volume_alone",
      .func  = TestStandRegistryScratchLeavesTheVolumeAlone,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a registry built and thrown away beside the volume's shares the volume's "
                       "chain and its mark of what is owed, so its end makes the volume's next "
                       "flush take new blocks and leave the old ones owned by nothing" },
    { .name  = "stand_registry_load_refuses_what_it_cannot_hold",
      .func  = TestStandRegistryLoadRefusesWhatItCannotHold,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a registry load whose allocation is refused drops what it could not hold "
                       "and answers OK, so the volume mounts missing tags its medium carries" },
    { .name  = "stand_registry_key_ids_are_copies",
      .func  = TestStandRegistryKeyIdsAreCopies,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a key's tag ids were handed out as the registry's own array after its "
                       "lock was let go, so the key's next tag could free it under a query still "
                       "reading it" },
};

const TestSuite g_stand_chain_suite = {
    .tests = g_stand_chain_tests, .count = ARRAY_SIZE(g_stand_chain_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
