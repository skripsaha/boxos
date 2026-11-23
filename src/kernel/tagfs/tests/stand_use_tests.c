#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "use_context.h"
#include "process.h"
#include "cabin.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The Use Context in memory (step 1.5b)
//
// Saying a context writes no page of the volume's book; two sayings that race
// to the volume leave the later one whole; and the scheduler's question costs
// one compare whatever the context holds. What the user had — the machine's
// context and the volume's memory of it — is put back on every path out.
// ============================================================================

#define STAND_USE_FRESH  10000u    /* new tags one `use` says */
#define STAND_USE_WIDE   100000u   /* tags of the context the scheduler is asked under */
#define STAND_USE_ASKS   1000u     /* scheduler questions counted */

/* `lead` (may be NULL), then `count` tags "key:0000000", "key:0000001", ...
 * comma-joined, in the heap; NULL with no memory. */
static char* StandUseList(const char* lead, const char* key, uint32_t count, size_t* out_len)
{
    size_t lead_len = lead ? strlen(lead) : 0;
    size_t each     = strlen(key) + 1 + 7 + 1;
    size_t room     = lead_len + 1 + (size_t)count * each + 1;
    char*  list     = kmalloc(room);
    if (!list) return NULL;
    size_t pos = 0;
    if (lead_len) {
        memcpy(list, lead, lead_len);
        pos = lead_len;
    }
    for (uint32_t i = 0; i < count; i++)
        pos += (size_t)ksnprintf(list + pos, room - pos, "%s%s:%07u",
                                 pos ? "," : "", key, i);
    list[pos] = '\0';
    *out_len  = pos;
    return list;
}

/* How many tags of the held list the volume has a number for. */
static uint32_t StandUseNumbered(TagFSVolume* vol, const UseList* l)
{
    uint32_t numbered = 0;
    for (uint32_t i = 0; i < UseListCount(l); i++) {
        TagSpelling tag = UseListTag(l, i);
        if (TagFSLookupSpelling(vol, &tag) != TAGFS_INVALID_TAG_ID)
            numbered++;
    }
    return numbered;
}

/* The machine's context put back as it was — said again from its own text,
 * or cleared — and the held list let go of. */
static void StandUseRestore(UseList* saved)
{
    UseText* text = UseListText(saved);
    if (text)
        UseContextSet(text->text, text->bytes, NULL);
    else
        UseContextClear(NULL);
    UseListDrop(saved);
}

// ----------------------------------------------------------------------------
// `use` looks up, and never issues
// ----------------------------------------------------------------------------

/* Ten thousand tags the volume has never seen, said as one context: every one
 * is in it, in the order said, and the volume has a number for none of them —
 * the numbers are the volume's to give to what wears a tag, and a context
 * wears nothing. Counted tag by tag rather than off the registry's next
 * number, which any program's intern may move while this runs. It used to
 * intern every tag, so a person's `use` spent the volume's lifetime numbers
 * and wrote each into its book. */
static TestResult TestStandUseLooksUpOnly(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult verdict = TEST_PASS;
    UseList*   saved   = UseContextHold();
    UseList*   now     = NULL;
    size_t     len     = 0;
    char*      list    = StandUseList(NULL, "standuse", STAND_USE_FRESH, &len);

    TEST_ASSERT(list != NULL, "memory for ten thousand tags");
    TEST_ASSERT_OK(UseContextSet(list, len, NULL), "a context of ten thousand new tags is said");
    now = UseContextHold();
    TEST_ASSERT_EQ(UseListCount(now), STAND_USE_FRESH, "every tag is in it");
    TEST_ASSERT(UseListText(now)->bytes == len && memcmp(UseListText(now)->text, list, len) == 0,
                "as said, in the order said");

    uint32_t numbered = StandUseNumbered(vol, now);
    TestEvidence("of a context of 10000 new tags, the volume numbered", numbered);
    if (numbered != 0)
        verdict = TEST_DEFECT;
out:
    UseListDrop(now);
    kfree(list);
    StandUseRestore(saved);
    return verdict;
}

// ----------------------------------------------------------------------------
// Two sayings, one whole memory
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// The scheduler's question is one compare
// ----------------------------------------------------------------------------

/* A strand whose cabin wears exactly one tag, known to no process table. */
static process_t* StandUseStrand(uint32_t tag_id, cabin_t** out_cabin)
{
    cabin_t*   cabin = kmalloc(sizeof(cabin_t));
    process_t* proc  = kmalloc(sizeof(process_t));
    uint32_t*  over  = tag_id < 64 ? NULL : kmalloc(sizeof(uint32_t));
    if (!cabin || !proc || (tag_id >= 64 && !over)) {
        kfree(cabin);
        kfree(proc);
        kfree(over);
        return NULL;
    }
    memset(cabin, 0, sizeof(*cabin));
    memset(proc, 0, sizeof(*proc));
    if (over) {
        over[0]                      = tag_id;
        cabin->tag_overflow_ids      = over;
        cabin->tag_overflow_count    = 1;
        cabin->tag_overflow_capacity = 1;
    } else {
        cabin->tag_bits = (uint64_t)1 << tag_id;
    }
    proc->cabin = cabin;
    *out_cabin  = cabin;
    return proc;
}

/* The strand's cabin made to wear its one tag, or nothing. */
static void StandUseWear(cabin_t* cabin, uint32_t tag_id, bool wear)
{
    if (tag_id < 64) cabin->tag_bits = wear ? (uint64_t)1 << tag_id : 0;
    else             cabin->tag_overflow_count = wear ? 1u : 0u;
}

/* How many tags STAND_USE_ASKS scheduler questions compared on this core, and
 * the last answer. */
static uint64_t StandUseAsk(const process_t* proc, uint32_t* out_fit)
{
    uint64_t from = UseContextCompares();
    uint32_t fit  = 0;
    for (uint32_t i = 0; i < STAND_USE_ASKS; i++)
        fit = UseContextFit(proc);
    *out_fit = fit;
    return UseContextCompares() - from;
}

/* The question under a context of one tag and under one of a hundred
 * thousand compares no tag at all, however many times it is asked. And it
 * counts nothing: a strand that stopped wearing the tag is answered its
 * previous verdict until its recount, which is the proof that the question
 * reads a word and not the context. It used to walk the context against the
 * cabin under an interrupts-off lock, at every scheduling decision. */
static TestResult TestStandUseVerdictIsOneCompare(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult  verdict = TEST_PASS;
    UseList*    saved   = UseContextHold();
    cabin_t*    cabin   = NULL;
    process_t*  proc    = NULL;
    char*       list    = NULL;
    size_t      len     = 0;
    uint32_t    fit     = 0;
    TagSpelling system  = TagSpellingOfKey("system");
    uint32_t    id      = TagFSLookupSpelling(vol, &system);

    TEST_ASSERT(id != TAGFS_INVALID_TAG_ID, "the volume knows the system tag");
    proc = StandUseStrand(id, &cabin);
    TEST_ASSERT(proc != NULL, "memory for a stand-in strand");

    TEST_ASSERT_OK(UseContextSet("system", 6, NULL), "a context of one tag is said");
    UseContextVerdictRefresh(proc);
    uint64_t narrow = StandUseAsk(proc, &fit);
    TEST_ASSERT_EQ(fit, USE_CONTEXT_FIT_FULL, "a strand wearing the one tag fits the context whole");

    StandUseWear(cabin, id, false);
    UseContextCabinChanged();
    TEST_ASSERT_EQ(UseContextFit(proc), USE_CONTEXT_FIT_FULL,
                   "until its recount, a strand that stopped wearing it keeps its previous verdict");
    UseContextVerdictRefresh(proc);
    TEST_ASSERT_EQ(UseContextFit(proc), 0, "recounted, it fits not at all");
    StandUseWear(cabin, id, true);
    UseContextCabinChanged();

    list = StandUseList("system", "standwide", STAND_USE_WIDE - 1, &len);
    TEST_ASSERT(list != NULL, "memory for a hundred thousand tags");
    TEST_ASSERT_OK(UseContextSet(list, len, NULL), "a context of a hundred thousand tags is said");
    UseContextVerdictRefresh(proc);
    uint64_t wide = StandUseAsk(proc, &fit);
    TEST_ASSERT_EQ(fit, 0, "one tag worn of a hundred thousand is too little of the context to count");

    TestEvidence("tags compared by 1000 scheduler questions under contexts of 1 and 100000 tags",
                 narrow + wide);
    if (narrow != 0 || wide != 0)
        verdict = TEST_DEFECT;
out:
    kfree(list);
    if (proc) {
        kfree(cabin->tag_overflow_ids);
        kfree(cabin);
        kfree(proc);
    }
    StandUseRestore(saved);
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_use_tests[] = {
    { .name  = "stand_use_looks_up_only", .func = TestStandUseLooksUpOnly,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "`use` interns every tag it is given, spending the volume's lifetime "
                       "tag numbers on a context that wears nothing" },
    { .name  = "stand_use_verdict_is_one_compare", .func = TestStandUseVerdictIsOneCompare,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "the scheduler's question walks the Use Context against the cabin's tags "
                       "under an interrupts-off lock at every decision" },
};

const TestSuite g_stand_use_suite = {
    .tests = g_stand_use_tests, .count = ARRAY_SIZE(g_stand_use_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
