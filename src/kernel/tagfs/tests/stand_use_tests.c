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

/* The Ledger's copies as the medium holds them. */
static VolumeLedger s_use_ledger[VOLUME_LEDGER_COPIES];

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
static uint32_t StandUseNumbered(const UseList* l)
{
    uint32_t numbered = 0;
    for (uint32_t i = 0; i < UseListCount(l); i++) {
        TagSpelling tag = UseListTag(l, i);
        if (TagFSLookupSpelling(&tag) != TAGFS_INVALID_TAG_ID)
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
        UseContextSet(text->text, text->bytes);
    else
        UseContextClear();
    UseListDrop(saved);
}

/* The text the newest intact Ledger copy on the medium remembers — its chain
 * read back off the medium — held (UseTextDrop); NULL when it remembers none. */
static bool StandUseOnMedium(UseText** out_text)
{
    int newest = -1;
    *out_text  = NULL;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (!tagfs_ledger_peek(c, &s_use_ledger[c], NULL))
            continue;
        if (newest < 0 || s_use_ledger[c].seq > s_use_ledger[newest].seq)
            newest = (int)c;
    }
    return newest >= 0 && tagfs_ledger_peek((uint32_t)newest, &s_use_ledger[newest], out_text);
}

// ----------------------------------------------------------------------------
// `use` looks up, and never issues
// ----------------------------------------------------------------------------

/* Ten thousand tags the volume has never seen, said as one context: every one
 * is in it, in the order said, and the volume has a number for none of them —
 * the numbers are the volume's to give to what wears a tag, and a context
 * wears nothing. Counted tag by tag rather than off the registry's next
 * number, which any program's intern may move while this runs. It used to
 * intern every tag, so a person's `use` spent the volume's 32766 lifetime
 * numbers and wrote each into its book. */
static TestResult TestStandUseLooksUpOnly(void)
{
    TestResult verdict = TEST_PASS;
    UseList*   saved   = UseContextHold();
    UseList*   now     = NULL;
    size_t     len     = 0;
    char*      list    = StandUseList(NULL, "standuse", STAND_USE_FRESH, &len);

    TEST_ASSERT(list != NULL, "memory for ten thousand tags");
    TEST_ASSERT_OK(UseContextSet(list, len), "a context of ten thousand new tags is said");
    now = UseContextHold();
    TEST_ASSERT_EQ(UseListCount(now), STAND_USE_FRESH, "every tag is in it");
    TEST_ASSERT(UseListText(now)->bytes == len && memcmp(UseListText(now)->text, list, len) == 0,
                "as said, in the order said");

    uint32_t numbered = StandUseNumbered(now);
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

/* Two `use` writers of different lengths: the short word is said, then the
 * long one, and the long one's remembering lands. Then the short word's
 * remembering lands last — as it would from a core that took it before the
 * long one was said and was held up on its way to the volume. The volume
 * keeps the later word, whole, in memory and on the medium. */
static TestResult TestStandUseLaterWordStands(void)
{
    static const char quiet[] = "use:test:quiet";
    static const char night[] = "use:test:night,use:test:orders,use:test:north,use:test:long";

    TestResult  verdict     = TEST_PASS;
    UseList*    saved       = UseContextHold();
    uint64_t    saying      = 0;
    UseText*    before      = tagfs_recall_use_context(&saying);
    UseList*    early       = NULL;
    UseList*    late        = NULL;
    UseText*    held        = NULL;
    bool        changed     = false;
    bool        kept_late   = false;
    bool        kept_early  = true;
    uint64_t    late_saying = 0;
    UseText*    medium      = NULL;

    TEST_ASSERT_OK(UseContextSet(quiet, sizeof(quiet) - 1), "the short word is said");
    early = UseContextHold();
    TEST_ASSERT_OK(UseContextSet(night, sizeof(night) - 1), "and the long one after it");
    late = UseContextHold();

    changed = true;
    UseContextRemember(&kept_late);
    TEST_ASSERT(kept_late, "the volume remembers the long word");
    UseTextDrop(tagfs_recall_use_context(&late_saying));
    TEST_ASSERT(late_saying > 0, "as a word the machine said");

    TEST_ASSERT_OK(tagfs_remember_use_context(UseListText(early), late_saying - 1, &kept_early),
                   "the short word's remembering lands last");

    held = tagfs_recall_use_context(NULL);
    TEST_ASSERT(StandUseOnMedium(&medium), "a Ledger copy is readable");
    TestEvidence("bytes the volume remembers after the earlier, shorter word landed last",
                 held ? held->bytes : 0);
    if (kept_early || !UseTextSame(held, UseListText(late))) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT(medium != NULL && medium->bytes == sizeof(night) - 1,
                "the medium carries the long word's length");
    TEST_ASSERT(memcmp(medium->text, night, sizeof(night) - 1) == 0, "and the long word, whole");
out:
    UseTextDrop(medium);
    UseTextDrop(held);
    UseListDrop(early);
    UseListDrop(late);
    if (changed) {
        /* Put back as the word the volume now stands on, so it is taken. */
        uint64_t now = 0;
        bool     put = false;
        UseTextDrop(tagfs_recall_use_context(&now));
        error_t  rc  = tagfs_remember_use_context(before, now, &put);
        if (rc != OK || (before && !put))
            verdict = TestFailedAt(__FILE__, __LINE__, "the volume's earlier memory is put "
                                   "back (error, remembered)", rc, put);
    }
    UseTextDrop(before);
    StandUseRestore(saved);
    return verdict;
}

// ----------------------------------------------------------------------------
// The scheduler's question is one compare
// ----------------------------------------------------------------------------

/* A strand whose cabin wears exactly one tag, known to no process table. */
static process_t* StandUseStrand(uint16_t tag_id, cabin_t** out_cabin)
{
    cabin_t*   cabin = kmalloc(sizeof(cabin_t));
    process_t* proc  = kmalloc(sizeof(process_t));
    uint16_t*  over  = tag_id < 64 ? NULL : kmalloc(sizeof(uint16_t));
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
static void StandUseWear(cabin_t* cabin, uint16_t tag_id, bool wear)
{
    if (tag_id < 64) cabin->tag_bits = wear ? (uint64_t)1 << tag_id : 0;
    else             cabin->tag_overflow_count = wear ? 1u : 0u;
}

/* How many tags STAND_USE_ASKS scheduler questions compared on this core, and
 * the last answer. */
static uint64_t StandUseAsk(const process_t* proc, bool* out_answer)
{
    uint64_t from   = UseContextCompares();
    bool     answer = false;
    for (uint32_t i = 0; i < STAND_USE_ASKS; i++)
        answer = UseContextMatches(proc);
    *out_answer = answer;
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
    TestResult  verdict = TEST_PASS;
    UseList*    saved   = UseContextHold();
    cabin_t*    cabin   = NULL;
    process_t*  proc    = NULL;
    char*       list    = NULL;
    size_t      len     = 0;
    bool        answer  = false;
    TagSpelling system  = TagSpellingOfKey("system");
    uint16_t    id      = TagFSLookupSpelling(&system);

    TEST_ASSERT(id != TAGFS_INVALID_TAG_ID, "the volume knows the system tag");
    proc = StandUseStrand(id, &cabin);
    TEST_ASSERT(proc != NULL, "memory for a stand-in strand");

    TEST_ASSERT_OK(UseContextSet("system", 6), "a context of one tag is said");
    UseContextVerdictRefresh(proc);
    uint64_t narrow = StandUseAsk(proc, &answer);
    TEST_ASSERT(answer, "a strand wearing the one tag is in the tier");

    StandUseWear(cabin, id, false);
    UseContextCabinChanged();
    TEST_ASSERT(UseContextMatches(proc),
                "until its recount, a strand that stopped wearing it keeps its previous verdict");
    UseContextVerdictRefresh(proc);
    TEST_ASSERT(!UseContextMatches(proc), "recounted, it is out of the tier");
    StandUseWear(cabin, id, true);
    UseContextCabinChanged();

    list = StandUseList("system", "standwide", STAND_USE_WIDE - 1, &len);
    TEST_ASSERT(list != NULL, "memory for a hundred thousand tags");
    TEST_ASSERT_OK(UseContextSet(list, len), "a context of a hundred thousand tags is said");
    UseContextVerdictRefresh(proc);
    uint64_t wide = StandUseAsk(proc, &answer);
    TEST_ASSERT(!answer, "tags the volume has no number for put nobody in the tier");

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
    { .name  = "stand_use_later_word_stands", .func = TestStandUseLaterWordStands,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "an earlier Use Context whose remembering lands after a later one's "
                       "replaces it on the volume, which then remembers what the machine no "
                       "longer holds" },
    { .name  = "stand_use_verdict_is_one_compare", .func = TestStandUseVerdictIsOneCompare,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "the scheduler's question walks the Use Context against the cabin's tags "
                       "under an interrupts-off lock at every decision" },
};

const TestSuite g_stand_use_suite = {
    .tests = g_stand_use_tests, .count = ARRAY_SIZE(g_stand_use_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
