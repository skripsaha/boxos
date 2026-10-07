#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "amp.h"
#include "op_registry.h"
#include "boxos_decks.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// One spelling of a tag (step 1.1b)
//
// A tag's text is read one way at every station (tag_spelling.h): the width
// judged before anything is allocated, a tag the volume could not hold refused
// out loud, never cut to a shorter one. These tests ask the stations a program
// reaches — the storage deck's own handlers, called from kernel memory with no
// OpContext, so the deck's crate copies read and write these buffers directly
// — and the registry under them. A registry of the test's own takes every
// question whose wrong answer would leave a name on the volume for good.
// ============================================================================

#define STAND_LONG_BYTES   256u    /* one byte past the widest key */
#define STAND_VALUE_BYTES  300u    /* past the 255 every station used to cut at, well inside a record */
#define STAND_SET_VALUE    100u    /* past the 63 `tag add` used to cut at, inside its old 127 */
#define STAND_ANSWER_IDS   64u

static char     s_long[STAND_LONG_BYTES + 1];
static char     s_text[TAG_TEXT_WIDTH + 1];
static uint32_t s_answer[2 + STAND_ANSWER_IDS];
static uint8_t  s_info[1024];
static uint8_t  s_op[sizeof(ManifestOp) + 40] __attribute__((aligned(8)));
static Crate    s_crates[2];
static char     s_well_text[2][40];

/* One storage-deck op, asked the way a program asks it. The params are copied
 * in after the op's head; `in` and `out` are the two crates, either absent. */
static int StandStorageOp(uint16_t opcode, const void* params, uint16_t param_size,
                          const void* in, uint64_t in_size,
                          void* out, uint64_t out_cap, uint64_t* out_size)
{
    const OpRegistration* reg = OpRegistryLookup(OP_KIND(DECK_STORAGE, opcode));
    ManifestOp*           op  = (ManifestOp*)s_op;
    uint16_t              n   = 0;

    *out_size = 0;
    if (!reg || param_size > sizeof(s_op) - sizeof(ManifestOp))
        return ERR_NOT_INITIALIZED;
    memset(s_op, 0, sizeof(s_op));
    memset(s_crates, 0, sizeof(s_crates));
    op->op_kind    = OP_KIND(DECK_STORAGE, opcode);
    op->param_size = param_size;
    op->in_crate   = CRATE_INDEX_NONE;
    op->out_crate  = CRATE_INDEX_NONE;
    memcpy(op->params, params, param_size);
    if (in) {
        s_crates[n] = (Crate){ .magic = CRATE_MAGIC, .addr = (uint64_t)(uintptr_t)in,
                               .size = in_size, .capacity = in_size };
        op->in_crate = n++;
    }
    if (out) {
        s_crates[n] = (Crate){ .magic = CRATE_MAGIC, .addr = (uint64_t)(uintptr_t)out,
                               .capacity = out_cap };
        op->out_crate = n++;
    }
    int rc = reg->handler(op, s_crates, n, NULL);
    if (out)
        *out_size = s_crates[op->out_crate].size;
    return rc;
}

/* storage.query everywhere for the `len` bytes at `tags`: the handler's
 * answer, and in s_answer [delivered][total][ids…]. */
static int StandQuery(const char* tags, size_t len, uint64_t* out_size)
{
    uint8_t scope = STORAGE_SCOPE_EVERYWHERE;
    return StandStorageOp(STORAGE_TAG_QUERY, &scope, 1, tags, len,
                          s_answer, sizeof(s_answer), out_size);
}

/* `key:` and `bytes` bytes of `fill` into s_text. Its length. */
static size_t StandTagText(const char* key, char fill, size_t bytes)
{
    size_t k = strlen(key);
    memcpy(s_text, key, k);
    s_text[k] = ':';
    memset(s_text + k + 1, fill, bytes);
    s_text[k + 1 + bytes] = '\0';
    return k + 1 + bytes;
}

// ----------------------------------------------------------------------------
// The query station
// ----------------------------------------------------------------------------

/* A query for one tag of 256 bytes used to be dropped unread, and the answer
 * was every file in scope. It is refused as too long; and a tag whose value
 * runs past the old 255 is read whole and answered — no file wears it. The
 * query for every file that measures "every file" is answered with as many
 * ids as the answer holds, after its two counts. */
static TestResult TestStandLongQueryRefused(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   said    = 0;

    TEST_ASSERT_OK(StandQuery(NULL, 0, &said), "a query for every file is answered");
    TEST_ASSERT_EQ(said, 8 + (uint64_t)s_answer[0] * sizeof(uint32_t),
                   "with its two counts and an id for each file it delivered");
    TEST_ASSERT(s_answer[1] >= s_answer[0], "of every file there is");
    uint32_t every = s_answer[1];

    memset(s_long, 'q', STAND_LONG_BYTES);
    s_long[STAND_LONG_BYTES] = '\0';
    int rc = StandQuery(s_long, STAND_LONG_BYTES, &said);
    TestEvidence("files a query for one 256-byte tag answered, of every file there is",
                 rc == OK ? s_answer[1] : 0);
    if (rc == OK && s_answer[1] == every && every > 0) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(rc, ERR_TAGFS_TAG_TOO_LONG, "a 256-byte tag is refused as too long");
    TEST_ASSERT_EQ(said, 0, "and nothing is answered");

    size_t len = StandTagText("stand_value", 'v', STAND_VALUE_BYTES);
    TEST_ASSERT_OK(StandQuery(s_text, len, &said), "a tag with a 300-byte value is read whole");
    TEST_ASSERT_EQ(s_answer[1], 0, "and no file wears it");

    TEST_ASSERT_EQ(StandQuery(" , ,", 4, &said), ERR_TAGFS_INVALID_TAG,
                   "a query whose text names no tag is refused, not every file");
out:
    return verdict;
}

// ----------------------------------------------------------------------------
// The registry: widths before memory, a full registry out loud
// ----------------------------------------------------------------------------

/* A key one byte past its width, handed to a registry as a spelling nobody
 * judged: refused as too long before a single allocation is made. A key cut
 * by a cast to fit the record would be given the number of a different,
 * shorter tag. The widths themselves are the record's: a 255-byte key and a
 * value to the end of the block are one tag, a byte more of either is not. */
static TestResult TestStandLongKeyRefusedBeforeAllocation(void)
{
    TestResult   verdict = TEST_PASS;
    uint8_t      me      = amp_get_core_index();
    TagRegistry* scratch = NULL;
    bool         built   = false;
    bool         armed   = false;
    uint16_t     id      = TAGFS_INVALID_TAG_ID;

    memset(s_long, 'k', STAND_LONG_BYTES);
    s_long[STAND_LONG_BYTES] = '\0';
    TagSpelling spelled;
    TEST_ASSERT_EQ(TagSpell(s_long, STAND_LONG_BYTES, &spelled), ERR_TAGFS_TAG_TOO_LONG,
                   "the spelling refuses a 256-byte key");
    TEST_ASSERT_EQ(tagfs_tag_intern(s_long, &id), ERR_TAGFS_TAG_TOO_LONG,
                   "and so does the volume's door");

    scratch = kmalloc(sizeof(TagRegistry));
    TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
    built = true;

    TagSpelling unjudged = { s_long, NULL, STAND_LONG_BYTES, 0 };
    HeapFailArm(me, 1);
    armed = true;
    error_t  rc   = TagRegistryIntern(scratch, &unjudged, &id);
    uint32_t seen = HeapFailSeen();
    HeapFailDisarm();
    armed = false;

    TestEvidence("allocations a registry made for a 256-byte key, and whether it numbered it",
                 seen + (rc == OK ? 1000u : 0u));
    if (rc == OK || seen != 0) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(rc, ERR_TAGFS_TAG_TOO_LONG, "the registry refuses the key as too long");
    TEST_ASSERT_EQ(id, TAGFS_INVALID_TAG_ID, "and gives it no number");

    TagSpelling widest = { s_long, NULL, TAG_KEY_WIDTH, 0 };
    TEST_ASSERT_OK(TagRegistryIntern(scratch, &widest, &id), "a 255-byte key is a tag");
    size_t      room = TAGFS_REGISTRY_DATA_SIZE - TAGFS_REGISTRY_RECORD_HEAD - 1;
    TagSpelling full = { "w", s_text, 1, room };
    memset(s_text, 'v', room + 1);
    TEST_ASSERT_OK(TagRegistryIntern(scratch, &full, &id), "a value to the end of the block is one");
    full.value_len = room + 1;
    TEST_ASSERT_EQ(TagRegistryIntern(scratch, &full, &id), ERR_TAGFS_TAG_TOO_LONG,
                   "and a byte more is refused");
out:
    if (armed)
        HeapFailDisarm();
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    return verdict;
}

/* A registry that has given out every number it may: a new tag is refused
 * with ERR_TAGFS_REGISTRY_FULL, every time, and said on the log the first
 * time. The registry is the test's own, told it is full by its next number —
 * on the volume the wall is 32767 tags away. It used to answer an invalid id
 * with nothing said but a line that compiles to nothing. */
static TestResult TestStandFullRegistryRefusesLoudly(void)
{
    TestResult   verdict = TEST_PASS;
    TagRegistry* scratch = NULL;
    bool         built   = false;
    uint16_t     id      = 0;

    scratch = kmalloc(sizeof(TagRegistry));
    TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
    built = true;
    scratch->next_id = TAGFS_MAX_TAG_ID + 1;

    TagSpelling tag = TagSpellingOfKey("stand_full");
    error_t     rc  = TagRegistryIntern(scratch, &tag, &id);
    TestEvidence("new tags a registry with every number given out numbered", rc == OK ? 1 : 0);
    if (rc == OK) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(rc, ERR_TAGFS_REGISTRY_FULL, "the registry refuses it as full");
    TEST_ASSERT_EQ(id, TAGFS_INVALID_TAG_ID, "with no number");
    TEST_ASSERT(scratch->said_full, "and has said so");
    TEST_ASSERT_EQ(TagRegistryIntern(scratch, &tag, &id), ERR_TAGFS_REGISTRY_FULL,
                   "the next ask is refused the same way");
out:
    if (built) {
        scratch->next_id = 0;
        tag_registry_destroy(scratch);
    }
    kfree(scratch);
    return verdict;
}

// ----------------------------------------------------------------------------
// trashed and hidden past tag 63
// ----------------------------------------------------------------------------

/* A fresh "stand_well:<k>" numbered past 63, its text left in `text`. A tag
 * is interned for good, so a stand volume carries a few more of them. */
static bool StandHighTag(char* text, size_t text_size, uint16_t* out_id)
{
    for (uint32_t k = 0; k < 2u * (TAGFS_MAX_TAG_ID + 1u); k++) {
        ksnprintf(text, text_size, "stand_well:%u", k);
        if (tagfs_tag_lookup(text) != TAGFS_INVALID_TAG_ID)
            continue;
        if (tagfs_tag_intern(text, out_id) != OK)
            return false;
        if (*out_id >= 64)
            return true;
    }
    return false;
}

/* Whether a query of `count` spellings lists `file_id`: its page from that id
 * on begins with it. */
static bool StandListed(const TagSpelling* tags, uint32_t count, uint32_t file_id)
{
    TagFSPage page;
    uint32_t  first = 0;
    return TagFSQueryPage(tags, count, file_id, &first, 1, false, &page) == OK &&
           page.delivered == 1 && first == file_id;
}

/* The flags storage.getinfo gives a file, 0 when it would not answer. */
static uint32_t StandInfoFlags(uint32_t file_id)
{
    uint64_t said  = 0;
    uint32_t flags = 0;
    if (StandStorageOp(STORAGE_OBJ_GET_INFO, &file_id, sizeof(file_id), NULL, 0,
                       s_info, sizeof(s_info), &said) != OK || said < 8)
        return 0;
    memcpy(&flags, s_info + 4, sizeof(flags));
    return flags;
}

/* trashed and hidden were kept as 1ULL << id, which is 0 for every id past 63:
 * on a volume that interned them late, a trashed file was listed with
 * everything else and a program read it as not trashed. Here the two are made
 * tags numbered past 63, for the length of the test, and a file wearing each
 * must leave a listing that does not ask for it and carry its flag. */
static TestResult TestStandWellKnownPastTag63(void)
{
    TestResult     verdict  = TEST_PASS;
    WellKnownTags* wk       = tagfs_get_well_known_tags();
    WellKnownTags  saved    = *wk;
    bool           swapped  = false;
    uint32_t       files[2] = { 0, 0 };
    uint16_t       ids[2]   = { TAGFS_INVALID_TAG_ID, TAGFS_INVALID_TAG_ID };
    static const char* const names[2] = { "stand_well_t", "stand_well_h" };

    for (uint32_t i = 0; i < 2; i++) {
        TEST_ASSERT(StandHighTag(s_well_text[i], sizeof(s_well_text[i]), &ids[i]),
                    "a tag numbered past 63");
        TEST_ASSERT(StandMakeFile(names[i], NULL, 0, &files[i]), "a file of the test's own");
        TEST_ASSERT_EQ(tagfs_add_tag(files[i], ids[i], INK_UNWARRANTED), 0, "wears it");
    }

    wk->trashed = ids[0];
    wk->hidden  = ids[1];
    swapped     = true;

    uint32_t shown = 0, unflagged = 0;
    for (uint32_t i = 0; i < 2; i++) {
        TagSpelling asked[2];
        TEST_ASSERT_OK(TagSpellKey(names[i], strlen(names[i]), &asked[0]), "its name spells");
        TEST_ASSERT_OK(TagSpell(s_well_text[i], strlen(s_well_text[i]), &asked[1]),
                       "its tag spells");
        shown += StandListed(asked, 1, files[i]);
        TEST_ASSERT(StandListed(asked, 2, files[i]), "a query that names the tag lists the file");
        uint32_t want = i == 0 ? TAGFS_FILE_TRASHED : TAGFS_FILE_HIDDEN;
        unflagged += (StandInfoFlags(files[i]) & want) == 0;
    }
    TestEvidence("files past tag 63 listed unasked or read without their flag", shown + unflagged);
    if (shown + unflagged)
        verdict = TEST_DEFECT;
out:
    if (swapped)
        *wk = saved;
    for (uint32_t i = 0; i < 2; i++)
        if (files[i])
            tagfs_delete_file(files[i], INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// The create, tag and info stations
// ----------------------------------------------------------------------------

/* A name whose stem is longer than a key used to be cut to 127 bytes and worn
 * as that shorter tag; a create whose tag list held a tag past 255 made the
 * file without it. Both are refused now, and no file is made. */
static TestResult TestStandCreateRefusesWhatItCannotTag(void)
{
    TestResult  verdict = TEST_PASS;
    VolumeTally before, after;
    uint32_t    file_id = 0;
    uint64_t    said    = 0;

    TEST_ASSERT(VolumeTallyTake(&before), "the volume's files are counted");

    memset(s_text, 'n', STAND_VALUE_BYTES);
    memcpy(s_text + STAND_VALUE_BYTES, ".txt", 5);
    int made = tagfs_create_file(s_text, NULL, 0, &file_id);

    uint8_t params[33] = { 0 };
    memcpy(params, "stand_spell_c", 13);
    params[32] = STORAGE_SCOPE_EVERYWHERE;
    memset(s_long, 'k', STAND_LONG_BYTES);
    s_long[STAND_LONG_BYTES] = '\0';
    size_t len = (size_t)ksnprintf(s_text, sizeof(s_text), "stand_spell,%s", s_long);
    uint32_t created = 0;
    int deck = StandStorageOp(STORAGE_OBJ_CREATE, params, sizeof(params), s_text, len,
                              &created, sizeof(created), &said);

    TEST_ASSERT(VolumeTallyTake(&after), "and counted again");
    TestEvidence("files made with a tag they could not wear", after.files - before.files);
    if (made == 0 || deck == OK) {
        if (made == 0) tagfs_delete_file(file_id, INK_UNWARRANTED);
        if (deck == OK && said == sizeof(created)) tagfs_delete_file(created, INK_UNWARRANTED);
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(made, -(int)ERR_TAGFS_TAG_TOO_LONG, "a 300-byte stem is refused as too long");
    TEST_ASSERT_EQ(deck, ERR_TAGFS_TAG_TOO_LONG, "and so is a create with a 256-byte tag");
    TEST_ASSERT_EQ(after.files, before.files, "and no file was made");
out:
    return verdict;
}

/* Whether storage.getinfo gives back the tag `text` spells whole: its key and
 * value, each at its own length. The record is [u32 id][u32 flags][u64 size]
 * [u16 tags][u16 name_len][name], then per tag [u16 key_len][u16 val_len]
 * [u8 type][key][val]. */
static bool StandInfoHasWhole(uint32_t file_id, const char* text)
{
    TagSpelling tag;
    uint64_t    said = 0;
    if (TagSpell(text, strlen(text), &tag) != OK ||
        StandStorageOp(STORAGE_OBJ_GET_INFO, &file_id, sizeof(file_id), NULL, 0,
                       s_info, sizeof(s_info), &said) != OK || said < 20)
        return false;

    uint16_t tags, name_len;
    memcpy(&tags, s_info + 16, 2);
    memcpy(&name_len, s_info + 18, 2);
    for (uint64_t at = 20u + name_len, t = 0; t < tags && at + 5 <= said; t++) {
        uint16_t kl, vl;
        memcpy(&kl, s_info + at, 2);
        memcpy(&vl, s_info + at + 2, 2);
        if (kl == tag.key_len && vl == tag.value_len && at + 5 + kl + vl <= said &&
            memcmp(s_info + at + 5, tag.key, kl) == 0 &&
            memcmp(s_info + at + 5 + kl, tag.value, vl) == 0)
            return true;
        at += 5u + kl + vl;
    }
    return false;
}

/* `tag add` of a tag whose value runs past 63 bytes used to put a different
 * tag on the file — the value cut at 63 — and the file's info cut every key
 * and value at 127. The file wears exactly the tag it was given, gives back a
 * 300-byte value whole in its info, and unset takes exactly the tag off. */
static TestResult TestStandTagSetWearsWhatItWasGiven(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   file_id = 0;
    uint64_t   said    = 0;
    TagSpelling spelled;

    TEST_ASSERT(StandMakeFile("stand_spell_s", NULL, 0, &file_id), "a file of the test's own");
    size_t   len  = StandTagText("stand_spell", 's', STAND_SET_VALUE);
    uint32_t fid  = file_id;
    int      set  = StandStorageOp(STORAGE_TAG_SET, &fid, sizeof(fid), s_text, len, NULL, 0, &said);
    uint16_t want = tagfs_tag_lookup(s_text);
    bool     worn = want != TAGFS_INVALID_TAG_ID && tagfs_has_tag(file_id, want);
    TestEvidence("tag adds of a 100-byte value that put some other tag on the file", set == OK && !worn);
    if (set == OK && !worn) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_OK(set, "the tag is put on the file");
    TEST_ASSERT(worn, "and the file wears exactly it");
    TEST_ASSERT(StandInfoHasWhole(file_id, s_text), "its info carries the tag whole");

    TEST_ASSERT_OK(StandStorageOp(STORAGE_TAG_UNSET, &fid, sizeof(fid), s_text, len, NULL, 0, &said),
                   "unset of the same text is taken");
    TEST_ASSERT(!tagfs_has_tag(file_id, want), "and takes exactly that tag off");

    len = StandTagText("stand_info", 'i', STAND_VALUE_BYTES);
    TEST_ASSERT_OK(TagSpell(s_text, len, &spelled), "a tag with a 300-byte value spells");
    TEST_ASSERT_OK(tagfs_add_tag_string(file_id, &spelled, INK_UNWARRANTED),
                   "and is put on the file");
    TEST_ASSERT(StandInfoHasWhole(file_id, s_text), "and its info carries all 300 bytes of it");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_spelling_tests[] = {
    { .name  = "stand_long_query_refused", .func = TestStandLongQueryRefused,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a query of 256 bytes or more is dropped unread, and the answer is every "
                       "file in scope" },
    { .name  = "stand_long_key_refused_before_allocation",
      .func  = TestStandLongKeyRefusedBeforeAllocation, .reach = TEST_READS_ONLY,
      .caveat_defect = "a key past 255 bytes is cut by a cast, or allocated for before its width "
                       "is judged, and numbered as a different, shorter tag" },
    { .name  = "stand_full_registry_refuses_loudly", .func = TestStandFullRegistryRefusesLoudly,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a registry that has given out every number answers an invalid id and says "
                       "nothing a shipped kernel prints" },
    { .name  = "stand_well_known_past_tag_63", .func = TestStandWellKnownPastTag63,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "trashed and hidden kept as 1ULL << id are 0 past id 63, so a trashed file "
                       "is listed with everything else and read as not trashed" },
    { .name  = "stand_create_refuses_what_it_cannot_tag",
      .func  = TestStandCreateRefusesWhatItCannotTag, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a create cuts its name's tag to 127 bytes, or makes the file without a tag "
                       "it could not read" },
    { .name  = "stand_tag_set_wears_what_it_was_given",
      .func  = TestStandTagSetWearsWhatItWasGiven, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "tag add cuts a value past 63 bytes and puts a different tag on the file" },
};

const TestSuite g_stand_spelling_suite = {
    .tests = g_stand_spelling_tests, .count = ARRAY_SIZE(g_stand_spelling_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
