#include "tests.h"
#include "../tagfs.h"
#include "use_context.h"   /* the user's tags, narrowing every query */
#include "process.h"       /* a stand-in strand for the scheduler's question */
#include "cabin.h"

// ============================================================================
// Use Context
//
// The context is the user's and the volume is the user's. Every context set
// here only looks the volume's numbers up (a tag the volume does not know
// stays unknown), and whatever the user had — in the machine, and in the
// volume's memory — is put back on every path out, not only when the test
// passed.
// ============================================================================

/* Kept off the stack: two answers of 256 file ids and a tag's text would make
 * a 2.7 KiB frame, and the stack guard refuses anything over 1 KiB. */
static uint32_t    s_inside[256];
static uint32_t    s_everywhere[256];
static char        s_tag_text[512];
static TagSpelling s_spelled[8];

/* The held context's tags, spelled into s_spelled for a query. */
static bool use_spelled(const UseList* l)
{
    uint32_t count = UseListCount(l);
    if (count > ARRAY_SIZE(s_spelled)) return false;
    for (uint32_t i = 0; i < count; i++)
        s_spelled[i] = UseListTag(l, i);
    return true;
}

/* The context as it was, said again from its own text — or cleared, when there
 * was none — and the held list let go of. */
static void use_snapshot_restore(UseList* saved)
{
    const UseText* text = UseListText(saved);
    if (text)
        UseContextSet(text->text, text->bytes);
    else
        UseContextClear();
    UseListDrop(saved);
}

/* A strand that wears exactly one tag, for UseContextMatches. */
static process_t* use_test_strand(uint16_t tag_id, cabin_t** out_cabin)
{
    cabin_t*   cabin = kmalloc(sizeof(cabin_t));
    process_t* proc  = kmalloc(sizeof(process_t));
    if (!cabin || !proc) {
        kfree(cabin);
        kfree(proc);
        return NULL;
    }
    memset(cabin, 0, sizeof(*cabin));
    memset(proc, 0, sizeof(*proc));
    if (tag_id < 64) {
        cabin->tag_bits = (uint64_t)1 << tag_id;
    } else {
        cabin->tag_overflow_ids = kmalloc(sizeof(uint16_t));
        if (!cabin->tag_overflow_ids) {
            kfree(cabin);
            kfree(proc);
            return NULL;
        }
        cabin->tag_overflow_ids[0]   = tag_id;
        cabin->tag_overflow_count    = 1;
        cabin->tag_overflow_capacity = 1;
    }
    proc->cabin = cabin;
    *out_cabin  = cabin;
    return proc;
}

static void use_test_strand_free(process_t* proc, cabin_t* cabin)
{
    if (cabin) kfree(cabin->tag_overflow_ids);
    kfree(cabin);
    kfree(proc);
}

/* The scheduler's answer for a strand whose cabin was just changed by hand:
 * the change said, the verdict recounted as the strand's entry would, and the
 * question asked. */
static bool use_verdict_after_change(process_t* proc)
{
    UseContextCabinChanged();
    UseContextVerdictRefresh(proc);
    return UseContextMatches(proc);
}

/* Spelling: blanks dropped, "key:" is "key", a repeat kept once where it was
 * first said, the order the person's, an unfit tag refused with the context
 * left as it was. */
static TestResult test_use_context_spelling(void)
{
    static const char said[] = " code , cpp:, ,project:alpha ,code";
    TestResult verdict = TEST_PASS;
    UseList*   saved   = UseContextHold();
    UseList*   now     = NULL;
    char       unfit[300];

    TEST_ASSERT_OK(UseContextSet(said, sizeof(said) - 1),
                   "a list with blanks, an empty value and a repeat is a context");
    now = UseContextHold();
    TEST_ASSERT(now != NULL, "the context is set");
    TEST_ASSERT_EQ(UseListCount(now), 3, "three distinct tags");
    TEST_ASSERT(strcmp(UseListText(now)->text, "code,cpp,project:alpha") == 0,
                "canonical, in the order said, the repeat where it was first said");
    UseListDrop(now);
    now = NULL;

    memset(unfit, 'k', sizeof(unfit) - 1);
    unfit[sizeof(unfit) - 1] = '\0';
    TEST_ASSERT_EQ(UseContextSet(unfit, sizeof(unfit) - 1), ERR_TAGFS_TAG_TOO_LONG,
                   "a key longer than the registry's field is refused as too long");
    now = UseContextHold();
    TEST_ASSERT_EQ(UseListCount(now), 3, "the refusal left the context as it was");
out:
    UseListDrop(now);
    use_snapshot_restore(saved);
    return verdict;
}

/* Walls: a context of a tag nobody wears shows nothing, and no strand can be
 * in its tier. */
static TestResult test_use_context_walls(void)
{
    static const char said[] = "use:test:nobody-wears-this";
    TestResult verdict = TEST_PASS;
    UseList*   saved   = UseContextHold();
    UseList*   now     = NULL;
    cabin_t*   cabin   = NULL;
    process_t* proc    = NULL;
    uint32_t   ids[8];
    TagFSPage  page;

    TEST_ASSERT_OK(UseContextSet(said, sizeof(said) - 1),
                   "a tag the volume does not know is still a context");
    now = UseContextHold();
    TEST_ASSERT_EQ(UseListCount(now), 1, "one tag");
    TEST_ASSERT(use_spelled(now), "the context's tags spell");
    TEST_ASSERT_OK(TagFSQueryPage(s_spelled, 1, 1, ids, ARRAY_SIZE(ids), true, &page),
                   "the context's question is put to the volume");
    TEST_ASSERT_EQ(page.total, 0, "inside walls nobody has built, nothing is visible");

    proc = use_test_strand(0, &cabin);
    TEST_ASSERT(proc != NULL, "no memory for a stand-in strand");
    cabin->tag_bits = ~(uint64_t)0;
    TEST_ASSERT(!use_verdict_after_change(proc),
                "a tag without a number on this volume puts nobody in the tier");
out:
    if (proc)
        use_test_strand_free(proc, cabin);
    UseListDrop(now);
    use_snapshot_restore(saved);
    return verdict;
}

/* Narrowing: a context of a tag some file wears shows only files wearing it,
 * and a strand wearing it is in the tier while one without is not. */
static TestResult test_use_context_narrows(void)
{
    TestResult    verdict   = TEST_PASS;
    UseList*      saved     = NULL;
    bool          taken     = false;
    UseList*      now       = NULL;
    cabin_t*      cabin     = NULL;
    process_t*    proc      = NULL;
    bool          have_meta = false;
    uint32_t      first[1];
    TagFSPage     page;
    TagFSMetadata meta;

    TEST_ASSERT_OK(TagFSListPage(1, first, 1, false, &page), "the volume's files are listed");
    TEST_ASSERT_EQ(page.delivered, 1, "a volume with at least one file");
    TEST_ASSERT(tagfs_get_metadata(first[0], &meta) == 0, "the first file's metadata");
    have_meta = true;
    if (meta.tag_count == 0) {
        verdict = test_steps_aside("the first file on this volume carries no tag, "
                                   "and a Use Context narrows by tags — there was "
                                   "nothing here to narrow by");
        goto out;
    }
    uint16_t tag_id = meta.tag_ids[0];
    tagfs_metadata_free(&meta);
    have_meta = false;

    size_t tag_len = tagfs_tag_text(tag_id, s_tag_text, sizeof(s_tag_text));
    TEST_ASSERT(tag_len > 0 && tag_len < sizeof(s_tag_text), "the tag has a name, whole");

    saved = UseContextHold();
    taken = true;
    TEST_ASSERT_OK(UseContextSet(s_tag_text, tag_len), "a context of a tag the volume knows");
    now = UseContextHold();

    TEST_ASSERT(use_spelled(now), "the context's tags spell");
    TEST_ASSERT_OK(TagFSQueryPage(s_spelled, UseListCount(now), 1, s_inside,
                                  ARRAY_SIZE(s_inside), true, &page),
                   "the context's question is put to the volume");
    uint32_t n_inside = page.total;
    TEST_ASSERT(n_inside >= 1, "the file that wears the tag is visible inside");
    for (uint32_t i = 0; i < page.delivered; i++)
        TEST_ASSERT(tagfs_has_tag(s_inside[i], tag_id), "everything visible inside wears the tag");

    TEST_ASSERT_OK(TagFSListPage(1, s_everywhere, ARRAY_SIZE(s_everywhere), true, &page),
                   "the volume's files are listed");
    TEST_ASSERT(n_inside <= page.total, "inside is never more than everywhere");

    proc = use_test_strand(tag_id, &cabin);
    TEST_ASSERT(proc != NULL, "no memory for a stand-in strand");
    bool wearing = use_verdict_after_change(proc);
    cabin->tag_bits           = 0;
    cabin->tag_overflow_count = 0;
    bool bare = use_verdict_after_change(proc);
    UseContextClear();
    if (tag_id < 64) cabin->tag_bits = (uint64_t)1 << tag_id;
    else             cabin->tag_overflow_count = 1;
    bool after_clear = use_verdict_after_change(proc);
    TEST_ASSERT(wearing, "a strand wearing the context's tag is in the tier");
    TEST_ASSERT(!bare, "a strand without it is not");
    TEST_ASSERT(!after_clear, "with no context there is no tier");
out:
    if (proc)
        use_test_strand_free(proc, cabin);
    if (have_meta)
        tagfs_metadata_free(&meta);
    UseListDrop(now);
    if (taken)
        use_snapshot_restore(saved);
    return verdict;
}

// ----------------------------------------------------------------------------
// The volume remembers the context.
//
// Only the volume's memory is touched here, never the machine's context: the
// boot road that takes one up runs after these tests, on an empty context. So
// what the volume held before is put back on every path out, said as the same
// word it was, and the road recalls exactly what it would have.
// ----------------------------------------------------------------------------

/* 30 x "use:test:longNN" and 29 commas: 479 bytes, past the 432 a Ledger
 * sector holds after its fixed fields and inside one sector. */
#define USE_LONG_TAGS 30u

static struct {
    char         said[TAGFS_SECTOR_SIZE];
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    char         tail[VOLUME_LEDGER_COPIES][TAGFS_SECTOR_SIZE];
    uint16_t     tail_len[VOLUME_LEDGER_COPIES];
    char         words[64];
} s_ledger;

/* What the volume held before the test is said to it again, as the word it
 * was; the held text is let go of. */
static TestResult UseLedgerPutBack(TestResult verdict, UseText* before, uint64_t saying)
{
    bool    kept = false;
    error_t rc   = tagfs_remember_use_context(before, saying, &kept);
    bool    had  = before != NULL;
    UseTextDrop(before);
    if (rc != OK || (had && !kept))
        return TestFailedAt(__FILE__, __LINE__,
                            "the earlier context is put back and remembered again "
                            "(error, remembered)", rc, kept);
    return verdict;
}

/* A context the record holds goes onto the medium as said — read back off it
 * by the Ledger's own reader, the newer of the two copies. */
static TestResult test_use_context_remembered(void)
{
    static const char said[] = "use:test:night,use:test:orders";

    TestResult verdict = TEST_PASS;
    bool       changed = false;
    bool       kept    = false;
    int        newest  = -1;
    uint64_t   saying  = 0;
    UseText*   before  = tagfs_recall_use_context(&saying);
    UseText*   text    = UseTextMake(said, sizeof(said) - 1);
    UseText*   back    = NULL;

    TEST_ASSERT(text != NULL, "memory for the text said");
    changed = true;
    TEST_ASSERT_OK(tagfs_remember_use_context(text, saying, &kept),
                   "the volume takes the context");
    TEST_ASSERT(kept, "and remembers it");

    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (!tagfs_ledger_peek(c, &s_ledger.led[c], s_ledger.tail[c], &s_ledger.tail_len[c]))
            continue;
        if (newest < 0 || s_ledger.led[c].seq > s_ledger.led[newest].seq)
            newest = (int)c;
    }
    TEST_ASSERT(newest >= 0, "a Ledger copy is readable");
    TEST_ASSERT_EQ(s_ledger.tail_len[newest], sizeof(said) - 1,
                   "the medium carries the context at the length it was said");
    TEST_ASSERT(memcmp(s_ledger.tail[newest], said, sizeof(said) - 1) == 0,
                "the medium carries the context as said");
    TEST_ASSERT_EQ(s_ledger.led[newest].use_context_offset, sizeof(VolumeLedger),
                   "right after the fixed fields");
    back = tagfs_recall_use_context(NULL);
    TEST_ASSERT(back != NULL && back->bytes == sizeof(said) - 1, "and the volume recalls it");
out:
    UseTextDrop(text);
    UseTextDrop(back);
    if (changed)
        verdict = UseLedgerPutBack(verdict, before, saying);
    else
        UseTextDrop(before);
    return verdict;
}

/* A context longer than the Ledger's sector holds after its fixed fields. The
 * volume forgets it whole rather than keeping half — told, but not remembered
 * — and a person's context is lost at the next boot. Held under its caveat
 * until step 1.6 gives it room. */
static TestResult TestUseContextLongRemembered(void)
{
    TestResult verdict  = TEST_PASS;
    bool       changed  = false;
    bool       kept     = false;
    size_t     said_len = 0;
    uint64_t   saying   = 0;
    UseText*   before   = tagfs_recall_use_context(&saying);
    UseText*   text     = NULL;
    UseText*   back     = NULL;

    for (uint32_t i = 0; i < USE_LONG_TAGS; i++)
        said_len += (size_t)ksnprintf(s_ledger.said + said_len,
                                      sizeof(s_ledger.said) - said_len,
                                      "%suse:test:long%02u", i ? "," : "", i);
    text = UseTextMake(s_ledger.said, said_len);
    TEST_ASSERT(text != NULL, "memory for the text said");

    changed = true;
    error_t rc   = tagfs_remember_use_context(text, saying, &kept);
    back         = tagfs_recall_use_context(NULL);
    size_t  left = back ? back->bytes : 0;
    ksnprintf(s_ledger.words, sizeof(s_ledger.words),
              "of %u bytes said, the volume remembers", (unsigned)said_len);
    TestEvidence(s_ledger.words, left);

    TEST_ASSERT_OK(rc, "a long context is taken without error");
    if (!kept && left == 0) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT(kept, "a long context is remembered");
    TEST_ASSERT_EQ(left, said_len, "whole, not in part");
    TEST_ASSERT(memcmp(back->text, s_ledger.said, said_len) == 0, "and recalled as it was said");
out:
    UseTextDrop(text);
    UseTextDrop(back);
    if (changed)
        verdict = UseLedgerPutBack(verdict, before, saying);
    else
        UseTextDrop(before);
    return verdict;
}

static const TestCase g_use_context_tests[] = {
    { .name = "use_context_spelling",   .func = test_use_context_spelling,   .reach = TEST_READS_ONLY },
    { .name = "use_context_walls",      .func = test_use_context_walls,      .reach = TEST_READS_ONLY },
    { .name = "use_context_narrows",    .func = test_use_context_narrows,    .reach = TEST_READS_ONLY },
    { .name = "use_context_remembered", .func = test_use_context_remembered, .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "use_context_long_remembered", .func = TestUseContextLongRemembered,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_step   = "1.6",
      .caveat_defect = "a Use Context longer than 432 bytes is forgotten, remembered=0" },
};

const TestSuite g_use_context_suite = {
    .tests = g_use_context_tests, .count = ARRAY_SIZE(g_use_context_tests),
};
