#include "tests.h"
#include "../tagfs.h"
#include "use_context.h"   /* the user's tags, narrowing every query */
#include "../use_memory/use_memory.h"
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
static uint64_t    s_inside[256];
static uint64_t    s_everywhere[256];
static char        s_tag_text[512];
static char        s_pair_text[512 + 32];
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
        UseContextSet(text->text, text->bytes, NULL);
    else
        UseContextClear(NULL);
    UseListDrop(saved);
}

/* A strand that wears exactly one tag, for UseContextFit. */
static process_t* use_test_strand(uint32_t tag_id, cabin_t** out_cabin)
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
        cabin->tag_overflow_ids = kmalloc(sizeof(uint32_t));
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
static uint32_t use_fit_after_change(process_t* proc)
{
    UseContextCabinChanged();
    UseContextVerdictRefresh(proc);
    return UseContextFit(proc);
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

    TEST_ASSERT_OK(UseContextSet(said, sizeof(said) - 1, NULL),
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
    TEST_ASSERT_EQ(UseContextSet(unfit, sizeof(unfit) - 1, NULL), ERR_TAGFS_TAG_TOO_LONG,
                   "a key longer than the registry's field is refused as too long");
    now = UseContextHold();
    TEST_ASSERT_EQ(UseListCount(now), 3, "the refusal left the context as it was");
out:
    UseListDrop(now);
    use_snapshot_restore(saved);
    return verdict;
}

/* Change: tags added after those said, one said already kept where it was,
 * tags taken out, one in both lists taken out, a change of nothing told so
 * with no word said, everything taken out clears, an unfit tag refuses the
 * change with the context as it was — and every saying answers with its own
 * word, the earlier one still saying what it said. */
static TestResult test_use_context_change(void)
{
    static const char two[]        = "use:test:a,use:test:b";
    static const char add[]        = "use:test:c, use:test:a";
    static const char drop[]       = "use:test:b";
    static const char changed_to[] = "use:test:a,use:test:c";
    static const char said_again[] = "use:test:a";
    static const char other[]      = "use:test:x";
    static const char both[]       = "use:test:d";
    static const char lone[]       = "use:test:e";
    TestResult verdict = TEST_PASS;
    UseList*   saved   = UseContextHold();
    UseList*   now     = NULL;
    UseWord    first   = { NULL, 0 };
    UseWord    second  = { NULL, 0 };
    UseWord    word    = { NULL, 0 };
    bool       changed = false;
    char       unfit[300];

    TEST_ASSERT_OK(UseContextSet(two, sizeof(two) - 1, &first), "a context of two tags");
    TEST_ASSERT(first.said && strcmp(first.said->text, two) == 0, "the word answered is the one said");

    TEST_ASSERT_OK(UseContextChange(add, sizeof(add) - 1, drop, sizeof(drop) - 1, &second, &changed),
                   "a tag added, one said again and one taken out, in one change");
    TEST_ASSERT(changed, "the change changed the context");
    TEST_ASSERT_EQ(second.saying, first.saying + 1, "a word of its own, the next one");
    TEST_ASSERT(second.said && strcmp(second.said->text, changed_to) == 0,
                "the one said again kept where it was, the new one after it, the other gone");
    now = UseContextHold();
    TEST_ASSERT(now && strcmp(UseListText(now)->text, changed_to) == 0, "and that is the context");
    UseListDrop(now);
    now = NULL;
    TEST_ASSERT(strcmp(first.said->text, two) == 0, "the earlier word still says what it said");

    TEST_ASSERT_OK(UseContextChange(said_again, sizeof(said_again) - 1, other, sizeof(other) - 1,
                                    &word, &changed),
                   "a change of nothing: a tag said already, one not said taken out");
    TEST_ASSERT(!changed, "is told it changed nothing");
    TEST_ASSERT_EQ(word.saying, second.saying, "and says no word: the standing one is answered");
    UseTextDrop(word.said);
    word.said = NULL;

    TEST_ASSERT_OK(UseContextChange(both, sizeof(both) - 1, both, sizeof(both) - 1, &word, &changed),
                   "a tag both added and taken out");
    TEST_ASSERT(!changed, "is taken out, so nothing changed");
    UseTextDrop(word.said);
    word.said = NULL;

    memset(unfit, 'k', sizeof(unfit) - 1);
    unfit[sizeof(unfit) - 1] = '\0';
    TEST_ASSERT_EQ(UseContextChange(unfit, sizeof(unfit) - 1, "", 0, &word, &changed),
                   ERR_TAGFS_TAG_TOO_LONG, "a key longer than the registry's field refuses the change");
    now = UseContextHold();
    TEST_ASSERT(now && strcmp(UseListText(now)->text, changed_to) == 0,
                "the refusal left the context as it was");
    UseListDrop(now);
    now = NULL;

    TEST_ASSERT_OK(UseContextChange("", 0, changed_to, sizeof(changed_to) - 1, &word, &changed),
                   "every tag taken out");
    TEST_ASSERT(changed && word.said == NULL, "clears: the word says no context");
    now = UseContextHold();
    TEST_ASSERT(now == NULL, "and none stands");

    TEST_ASSERT_OK(UseContextChange(lone, sizeof(lone) - 1, "", 0, &word, &changed),
                   "a tag added to no context");
    TEST_ASSERT(changed && word.said && strcmp(word.said->text, lone) == 0, "is the context");
out:
    UseListDrop(now);
    UseTextDrop(first.said);
    UseTextDrop(second.said);
    UseTextDrop(word.said);
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
    uint64_t   ids[8];
    TagFSPage  page;

    TEST_ASSERT_OK(UseContextSet(said, sizeof(said) - 1, NULL),
                   "a tag the volume does not know is still a context");
    now = UseContextHold();
    TEST_ASSERT_EQ(UseListCount(now), 1, "one tag");
    TEST_ASSERT(use_spelled(now), "the context's tags spell");
    TEST_ASSERT_OK(TagFSQueryPage(s_spelled, 1, NULL, 0, 1, ids, ARRAY_SIZE(ids), true, &page),
                   "the context's question is put to the volume");
    TEST_ASSERT_EQ(page.total, 0, "inside walls nobody has built, nothing is visible");

    proc = use_test_strand(0, &cabin);
    TEST_ASSERT(proc != NULL, "no memory for a stand-in strand");
    cabin->tag_bits = ~(uint64_t)0;
    TEST_ASSERT_EQ(use_fit_after_change(proc), 0,
                   "a tag without a number on this volume is worn by nobody");
out:
    if (proc)
        use_test_strand_free(proc, cabin);
    UseListDrop(now);
    use_snapshot_restore(saved);
    return verdict;
}

/* Narrowing: a context of a tag some file wears shows only files wearing it;
 * a strand wearing it fits the context whole and one without not at all, and
 * under a context of that tag and one no file wears, the strand wearing the
 * first fits it half — the tag nobody wears is still half of what was said. */
static TestResult test_use_context_narrows(void)
{
    TestResult    verdict   = TEST_PASS;
    UseList*      saved     = NULL;
    bool          taken     = false;
    UseList*      now       = NULL;
    cabin_t*      cabin     = NULL;
    process_t*    proc      = NULL;
    bool          have_meta = false;
    uint64_t      first[1];
    TagFSPage     page;
    TagFSMetadata meta;

    /* The first file that wears a tag: a file wears none unless it was given
     * one, and its name is no tag. */
    uint64_t from = 1;
    do {
        TEST_ASSERT_OK(TagFSListPage(from, first, 1, false, &page), "the volume's files are listed");
        if (page.delivered == 0)
            break;
        if (have_meta)
            tagfs_metadata_free(&meta);
        have_meta = false;
        TEST_ASSERT(tagfs_get_metadata(first[0], &meta) == 0, "a file's metadata");
        have_meta = true;
        from = page.next;
    } while (meta.tag_count == 0 && from != 0);
    if (!have_meta || meta.tag_count == 0) {
        verdict = test_steps_aside("no file on this volume carries a tag, "
                                   "and a Use Context narrows by tags — there was "
                                   "nothing here to narrow by");
        goto out;
    }
    uint32_t tag_id = meta.tag_ids[0];
    tagfs_metadata_free(&meta);
    have_meta = false;

    size_t tag_len = tagfs_tag_text(tag_id, s_tag_text, sizeof(s_tag_text));
    TEST_ASSERT(tag_len > 0 && tag_len < sizeof(s_tag_text), "the tag has a name, whole");

    saved = UseContextHold();
    taken = true;
    TEST_ASSERT_OK(UseContextSet(s_tag_text, tag_len, NULL), "a context of a tag the volume knows");
    now = UseContextHold();

    TEST_ASSERT(use_spelled(now), "the context's tags spell");
    TEST_ASSERT_OK(TagFSQueryPage(s_spelled, UseListCount(now), NULL, 0, 1, s_inside,
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
    uint32_t wearing = use_fit_after_change(proc);
    cabin->tag_bits           = 0;
    cabin->tag_overflow_count = 0;
    uint32_t bare = use_fit_after_change(proc);
    UseContextClear(NULL);
    if (tag_id < 64) cabin->tag_bits = (uint64_t)1 << tag_id;
    else             cabin->tag_overflow_count = 1;
    uint32_t after_clear = use_fit_after_change(proc);
    TEST_ASSERT_EQ(wearing, USE_CONTEXT_FIT_FULL, "a strand wearing the context's one tag fits it whole");
    TEST_ASSERT_EQ(bare, 0, "a strand without it does not fit at all");
    TEST_ASSERT_EQ(after_clear, 0, "with no context nothing fits");

    static const char nobodys[] = ",use:fit:nobody";
    memcpy(s_pair_text, s_tag_text, tag_len);
    memcpy(s_pair_text + tag_len, nobodys, sizeof(nobodys) - 1);
    TEST_ASSERT_OK(UseContextSet(s_pair_text, tag_len + sizeof(nobodys) - 1, NULL),
                   "a context of the tag and one no file wears");
    TEST_ASSERT_EQ(use_fit_after_change(proc), USE_CONTEXT_FIT_FULL / 2,
                   "a strand wearing one of the context's two tags fits it half");
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
 * sector once held after its fixed fields. */
#define USE_LONG_TAGS 30u

static struct {
    char said[TAGFS_SECTOR_SIZE];
    char words[64];
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

/* A context longer than the Ledger's sector ever held after its fixed fields
 * is remembered whole. It used to be forgotten — told, but not remembered —
 * and a person's context was lost at the next boot. */
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
    { .name = "use_context_change",     .func = test_use_context_change,     .reach = TEST_READS_ONLY },
    { .name = "use_context_walls",      .func = test_use_context_walls,      .reach = TEST_READS_ONLY },
    { .name = "use_context_narrows",    .func = test_use_context_narrows,    .reach = TEST_READS_ONLY },
    { .name  = "use_context_long_remembered", .func = TestUseContextLongRemembered,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a Use Context longer than 432 bytes is forgotten, remembered=0" },
};

const TestSuite g_use_context_suite = {
    .tests = g_use_context_tests, .count = ARRAY_SIZE(g_use_context_tests),
};
