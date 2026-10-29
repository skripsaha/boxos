/* form_stand — BoxLib's Form (src/form*.c) on the host, over a made-up volume.
 *
 *     tools/form_stand.sh
 *
 * Only BoxLib's own headers are seen here, so the sources are built as they
 * are in BoxOS; fread and the metadata reads, which would reach a volume, are
 * renamed on the command line to the readers below, which serve a memory image.
 * A case is a Form, a line and the answers a person would give; its outcome is
 * one string — the questions asked, then the fields filled as the program
 * would read them out of the Luggage the filler wrote, then the arrow — held to
 * the exact text expected. Every case also holds the filler's own fields to
 * what the Luggage decodes into. */

#include "box/defs.h"
#include "box/types.h"
#include "box/error.h"
#include "box/form.h"
#include "box/file.h"
#include "box/core/form_filler.h"
#include "box/string.h"
#include "box/memory.h"
#include "box/luggage.h"

int printf(const char *format, ...);

static int g_passed;
static int g_failed;

static void check(bool ok, const char *label, const char *got, const char *want)
{
    if (ok) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n  got:  %s\n  want: %s\n", label, got, want);
}

static void check_text(const char *label, const char *got, const char *want)
{
    check(strcmp(got, want) == 0, label, got, want);
}

static void check_int(const char *label, long got, long want)
{
    if (got == want) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n  got:  %ld\n  want: %ld\n", label, got, want);
}

/* ---- text building ----------------------------------------------------- */

#define RESULT_MAX 1024u

static void put(char *buffer, const char *text)
{
    size_t used = strlen(buffer);
    size_t length = strlen(text);
    if (used + length >= RESULT_MAX) length = RESULT_MAX - 1 - used;
    memcpy(buffer + used, text, length);
    buffer[used + length] = '\0';
}

static void put_number(char *buffer, unsigned long long value)
{
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char text[24];
    for (size_t i = 0; i < count; i++) text[i] = digits[count - 1 - i];
    text[count] = '\0';
    put(buffer, text);
}

static void dump(const FormValues *values, char *out)
{
    out[0] = '\0';
    for (uint32_t i = 0; i < values->FieldCount; i++) {
        const FormFieldValue *field = &values->Fields[i];
        if (i > 0) put(out, " ");
        put(out, form_kind_name(field->Kind));
        put(out, "(");
        for (size_t k = 0; k < field->IdCount; k++) {
            if (k > 0) put(out, " ");
            put(out, "#");
            put_number(out, field->Ids[k]);
        }
        for (size_t k = 0; k < field->WordCount; k++) {
            if (k > 0 || field->IdCount > 0) put(out, " ");
            put(out, "=");
            put(out, field->Words[k]);
        }
        put(out, ")");
    }
}

/* ---- the volume and the readers ---------------------------------------- */

typedef struct {
    file_id_t   Id;
    const char *Name;
    bool        InContext;
} StandFile;

static const StandFile g_files[] = {
    { 1, "a.txt", true },       { 2, "b.txt", true },    { 3, "note.txt", true },
    { 4, "note.txt", true },    { 5, "other.txt", false }, { 7, "readme", true },
    { 8, "my file.txt", true }, { 9, "c.txt", true },    { 10, "done", true },
};
#define FILE_COUNT (sizeof(g_files) / sizeof(g_files[0]))

static bool        g_done_exists;
static bool        g_bounded;
static const char *g_find_fails;
static int         g_find_calls;
static file_id_t   g_them[4];
static size_t      g_them_count;
static bool        g_them_known;

static int stand_find(BoxFormLookup *lookup, const char *name, bool everywhere, file_id_t **ids)
{
    (void)lookup;
    g_find_calls++;
    *ids = NULL;
    if (g_find_fails && strcmp(g_find_fails, name) == 0) return -ERR_IO;

    file_id_t found[FILE_COUNT];
    int count = 0;
    for (size_t i = 0; i < FILE_COUNT; i++) {
        if (strcmp(g_files[i].Name, name) != 0) continue;
        if (g_files[i].Id == 10 && !g_done_exists) continue;
        if (!everywhere && !g_files[i].InContext) continue;
        found[count++] = g_files[i].Id;
    }
    if (count == 0) return 0;
    *ids = malloc((size_t)count * sizeof(file_id_t));
    memcpy(*ids, found, (size_t)count * sizeof(file_id_t));
    return count;
}

static int stand_all(BoxFormLookup *lookup, file_id_t **ids, bool *bounded)
{
    (void)lookup;
    file_id_t found[FILE_COUNT];
    int count = 0;
    for (size_t i = 0; i < FILE_COUNT; i++) {
        if (g_files[i].InContext && g_files[i].Id != 10) found[count++] = g_files[i].Id;
    }
    *bounded = g_bounded;
    *ids = malloc((size_t)count * sizeof(file_id_t));
    memcpy(*ids, found, (size_t)count * sizeof(file_id_t));
    return count;
}

static unsigned char *g_image;
static size_t         g_image_size;

int64_t StandRead(file_id_t file, uint64_t offset, void *buffer, size_t size)
{
    (void)file;
    if (offset >= g_image_size) return 0;
    size_t take = g_image_size - (size_t)offset;
    if (take > size) take = size;
    memcpy(buffer, g_image + offset, take);
    return (int64_t)take;
}

int StandMetadata(file_id_t file, FileMetadata *out)
{
    (void)file;
    memset(out, 0, sizeof(*out));
    out->Size = g_image_size;
    return 0;
}

void StandMetadataRelease(FileMetadata *metadata)
{
    (void)metadata;
}

static const char *g_luggage_bytes;
static uint32_t    g_luggage_length;

Luggage luggage(void)
{
    Luggage l = { g_luggage_bytes, g_luggage_length };
    return l;
}

/* ---- running a case ---------------------------------------------------- */

static void reset_volume(void)
{
    g_done_exists = false;
    g_bounded     = true;
    g_find_fails  = NULL;
    g_find_calls  = 0;
    g_them_count  = 0;
    g_them_known  = false;
}

static void set_them(const file_id_t *ids, size_t count)
{
    memcpy(g_them, ids, count * sizeof(file_id_t));
    g_them_count = count;
    g_them_known = true;
}

static void trace_question(char *result, const BoxFormQuestion *question)
{
    switch (question->Step) {
    case BOX_FORM_ASK_WHICH:
        put(result, "WHICH(");
        put(result, question->Word);
        put(result, ":");
        put_number(result, question->ChoiceCount);
        put(result, ") ");
        break;
    case BOX_FORM_ASK_FIELD:
        put(result, "FIELD(");
        put_number(result, question->Field);
        put(result, ":");
        put(result, question->Word ? question->Word : "-");
        put(result, ") ");
        break;
    default:
        put(result, "SURE(");
        put_number(result, question->Count);
        put(result, ") ");
        break;
    }
}

static int answer(BoxFormFiller *filler, const BoxFormQuestion *question, const char *reply)
{
    if (question->Step == BOX_FORM_ASK_WHICH) {
        file_id_t choice = 0;
        for (const char *p = reply + 6; *p; p++) choice = choice * 10 + (file_id_t)(*p - '0');
        return form_fill_choose(filler, choice);
    }
    if (question->Step == BOX_FORM_ASK_FIELD) return form_fill_words(filler, reply + 6);
    return form_fill_sure(filler, strcmp(reply, "yes") == 0);
}

static void finish(const char *label, BoxFormFiller *filler, const BoxForm *form,
                   const char *program, char *result)
{
    file_id_t   screen;
    const char *create;
    char       *bytes;
    size_t      length;
    FormValues  values;
    file_id_t   decoded_screen;
    char        again[RESULT_MAX], own[RESULT_MAX];

    if (program) form_fill_program(filler, program);
    form_fill_arrow(filler, &screen, &create);
    int rc = form_fill_luggage(filler, screen, &bytes, &length);
    check_int(label, rc, 0);
    rc = form_records_decode(form, bytes, length, &values, &decoded_screen);
    check_int(label, rc, 1);
    dump(&values, again);
    dump(&filler->Values, own);
    check_text(label, again, own);
    check_int(label, (long)decoded_screen, (long)screen);

    put(result, "=> ");
    put(result, again);
    if (screen) {
        put(result, " screen=#");
        put_number(result, screen);
    }
    if (create) {
        put(result, " create=");
        put(result, create);
    }
    form_values_release(&values);
    free(bytes);
}

/* The outcome of a line against a Form, the answers given in order. */
static void run(const char *label, const char *form_text, const char *line, const char *program,
                const char *const *answers, char *result)
{
    BoxForm        form;
    BoxFormLookup  lookup = { stand_find, stand_all, g_them, g_them_count, g_them_known };
    BoxFormFiller *filler;
    BoxFormQuestion question;

    result[0] = '\0';
    if (form_parse(form_text, strlen(form_text), &form) != 0) {
        put(result, "FORM REFUSED");
        return;
    }
    if (form_fill_begin(&form, line, &lookup, &filler) != 0) {
        put(result, "BEGIN REFUSED");
        form_release(&form);
        return;
    }
    size_t given = 0;
    for (int turn = 0; turn < 64; turn++) {
        int rc = form_fill_step(filler, &question);
        check_int(label, rc, 0);
        if (question.Step == BOX_FORM_DONE) {
            finish(label, filler, &form, program, result);
            break;
        }
        if (question.Step == BOX_FORM_REFUSED) {
            put(result, "REFUSED: ");
            put(result, question.Word);
            break;
        }
        trace_question(result, &question);
        if (!answers || !answers[given]) {
            put(result, "UNANSWERED");
            break;
        }
        check_int(label, answer(filler, &question, answers[given++]), 0);
    }
    form_fill_end(filler);
    form_release(&form);
}

static void expect(const char *form_text, const char *line, const char *const *answers,
                   const char *want)
{
    char result[RESULT_MAX];
    run(line, form_text, line, NULL, answers, result);
    check_text(line, result, want);
}

/* ---- the Forms ----------------------------------------------------------- */

#define TAG    "form 1\ntakes things \"the files to tag\"\ntakes words \"the tags they get\"\ngives things\n"
#define RENAME "form 1\ntakes thing \"the file\"\ntakes name \"the new name\"\n"
#define SAY    "form 1\ntakes text \"what to say\"\n"
#define COPY   "form 1\ntakes thing \"the file\"\ntakes optional into \"where to put it\"\n"
#define WORDS  "form 1\ntakes words \"words\"\n"
#define NOTE   "form 1\ntakes thing \"the file\"\ntakes text \"the note\"\n"
#define PUT    "form 1\ntakes words \"words\"\ntakes things \"the files\"\n"

static void test_examples(void)
{
    expect(TAG, "a.txt done", NULL, "=> things(#1) words(=done)");
    expect(TAG, "done a.txt, b.txt", NULL, "=> things(#1 #2) words(=done)");
    const file_id_t both[] = { 1, 2 };
    set_them(both, 2);
    expect(TAG, "them done", NULL, "=> things(#1 #2) words(=done)");
    const char *second[] = { "which:2", NULL };
    expect(TAG, "it done", second, "WHICH(it:2) => things(#2) words(=done)");
    reset_volume();
    expect(RENAME, "a.txt idea.txt", NULL, "=> thing(#1) name(=idea.txt)");
}

static void test_order_and_lists(void)
{
    expect(TAG, "done a.txt", NULL, "=> things(#1) words(=done)");
    expect(TAG, "a.txt done b.txt", NULL, "=> things(#1 #2) words(=done)");
    expect(TAG, "a.txt b.txt done", NULL, "=> things(#1 #2) words(=done)");
    expect(TAG, "a.txt,b.txt done", NULL, "=> things(#1 #2) words(=done)");
    expect(TAG, "a.txt ,b.txt done", NULL, "=> things(#1 #2) words(=done)");
    expect(TAG, "a.txt , b.txt done", NULL, "=> things(#1 #2) words(=done)");
    expect(TAG, "x,y a.txt", NULL, "=> things(#1) words(=x =y)");
    expect(TAG, "a.txt, zzz done", NULL, "REFUSED: there is no zzz here");
    expect(TAG, "\"my file.txt\" done", NULL, "=> things(#8) words(=done)");
    expect(TAG, "a.txt \"all\"", NULL, "=> things(#1) words(=all)");
    expect(TAG, "a.txt \"two words\"", NULL, "=> things(#1) words(=two words)");
}

static void test_shell_words(void)
{
    reset_volume();
    expect(TAG, "them done", NULL, "FIELD(0:nothing was given before) UNANSWERED");
    const char *words[] = { "words:a.txt", NULL };
    expect(TAG, "them done", words, "FIELD(0:nothing was given before) => things(#1) words(=done)");
    expect(TAG, "a.txt them", NULL, "REFUSED: nothing was given before");
    set_them((const file_id_t[]){ 1, 2 }, 2);
    expect(RENAME, "them x", NULL, "WHICH(them:2) UNANSWERED");
    const char *pick[] = { "which:2", NULL };
    expect(RENAME, "them x", pick, "WHICH(them:2) => thing(#2) name(=x)");
    expect(RENAME, "it x", pick, "WHICH(it:2) => thing(#2) name(=x)");
    expect(TAG, "it done", NULL, "WHICH(it:2) UNANSWERED");
    expect(TAG, "a.txt it done", pick, "WHICH(it:2) => things(#1 #2) words(=done)");
    expect(TAG, "them it done", pick, "WHICH(it:2) => things(#1 #2 #2) words(=done)");
    reset_volume();
    set_them((const file_id_t[]){ 3 }, 1);
    expect(TAG, "it done", NULL, "=> things(#3) words(=done)");
    expect(RENAME, "it x", NULL, "=> thing(#3) name(=x)");
    expect(TAG, "them done", NULL, "=> things(#3) words(=done)");
    expect(WORDS, "it them", NULL, "=> words(=it =them)");
    reset_volume();
    g_them_known = true;
    expect(TAG, "them done", NULL, "FIELD(0:the last command gave nothing) UNANSWERED");
    expect(TAG, "it done", NULL, "FIELD(0:the last command gave nothing) UNANSWERED");
    reset_volume();
    expect(TAG, "it done", NULL, "FIELD(0:nothing was given before) UNANSWERED");

    reset_volume();
    expect(TAG, "all done", NULL, "=> things(#1 #2 #3 #4 #7 #8 #9) words(=done)");
    g_bounded = false;
    const char *yes[] = { "yes", NULL }, *no[] = { "no", NULL };
    expect(TAG, "all done", yes, "SURE(7) => things(#1 #2 #3 #4 #7 #8 #9) words(=done)");
    expect(TAG, "all done", no, "SURE(7) REFUSED: the whole volume was not agreed to");
    expect(TAG, "all done", NULL, "SURE(7) UNANSWERED");

    reset_volume();
    g_done_exists = true;
    expect(TAG, "a.txt done", NULL, "FIELD(1:-) UNANSWERED");
    expect(RENAME, "a.txt done", NULL, "=> thing(#1) name(=done)");
    g_done_exists = false;
    expect(TAG, "everywhere other.txt done", NULL, "=> things(#5) words(=done)");
    expect(TAG, "other.txt everywhere done", NULL,
           "FIELD(0:there is no other.txt here) UNANSWERED");
    expect(TAG, "other.txt done", words, "FIELD(0:there is no other.txt here) => things(#1) words(=done)");
    expect(TAG, "a.txt other.txt", NULL, "=> things(#1) words(=other.txt)");
}

static void test_which(void)
{
    reset_volume();
    const char *pick3[] = { "which:3", NULL };
    expect(TAG, "note.txt done", pick3, "WHICH(note.txt:2) => things(#3) words(=done)");
    expect(TAG, "note.txt done", NULL, "WHICH(note.txt:2) UNANSWERED");
    const char *two[] = { "which:3", "which:4", NULL };
    expect("form 1\ntakes things \"the files\"\n", "note.txt a.txt note.txt", two,
           "WHICH(note.txt:2) WHICH(note.txt:2) => things(#3 #1 #4)");
    expect(RENAME, "a.txt note.txt", NULL, "=> thing(#1) name(=note.txt)");
    expect(WORDS, "note.txt", NULL, "=> words(=note.txt)");
}

static void test_fields(void)
{
    reset_volume();
    const char *both[] = { "words:a.txt", "words:x y", NULL };
    expect(TAG, "", both, "FIELD(0:-) FIELD(1:-) => things(#1) words(=x =y)");
    expect(TAG, "notte.txt done", NULL, "FIELD(0:there is no notte.txt here) UNANSWERED");
    const char *typo[] = { "words:a.txt", NULL };
    expect(TAG, "notte.txt done", typo, "FIELD(0:there is no notte.txt here) => things(#1) words(=done)");
    const char *again[] = { "words:zzz.txt", "words:b.txt", NULL };
    expect(TAG, "notte.txt", again,
           "FIELD(0:there is no notte.txt here) FIELD(0:there is no zzz.txt here) FIELD(1:-) UNANSWERED");
    expect(TAG, "done", typo, "FIELD(0:-) => things(#1) words(=done)");
    const char *none[] = { "words:", NULL };
    expect(TAG, "done", none, "FIELD(0:-) FIELD(0:-) UNANSWERED");

    const char *which[] = { "words:note.txt", "which:4", NULL };
    expect(TAG, "done", which, "FIELD(0:-) WHICH(note.txt:2) => things(#4) words(=done)");
    expect(RENAME, "a.txt", NULL, "FIELD(1:-) UNANSWERED");
    const char *two_words[] = { "words:x y", "words:z", NULL };
    expect(RENAME, "a.txt", two_words, "FIELD(1:-) FIELD(1:one word is wanted) => thing(#1) name(=z)");
}

static void test_leftovers(void)
{
    reset_volume();
    expect(RENAME, "a.txt b c", NULL, "REFUSED: c has no place in this command");
    expect(RENAME, "a.txt b.txt c.txt", NULL, "REFUSED: c.txt has no place in this command");
    expect(RENAME, "a.txt b.txt", NULL, "=> thing(#1) name(=b.txt)");
    expect(RENAME, "a.txt c.txt b.txt", NULL, "REFUSED: b.txt has no place in this command");
    expect(WORDS, "them all everywhere ->", NULL, "REFUSED: -> wants the name of a file after it");
    expect(WORDS, "them all everywhere", NULL, "=> words(=them =all =everywhere)");
    expect("form 1\n", "x y", NULL, "REFUSED: x has no place in this command");
    expect("form 1\ntakes optional words \"w\"\n", "", NULL, "=> words()");
    expect(PUT, "x a.txt b.txt", NULL, "=> words(=x) things(#1 #2)");
    expect(COPY, "a.txt b.txt", NULL, "REFUSED: b.txt has no place in this command");
}

static void test_text(void)
{
    reset_volume();
    g_find_calls = 0;
    expect(SAY, "hello   world", NULL, "=> text(=hello   world)");
    check_int("say looks up nothing", g_find_calls, 0);
    expect(SAY, "\"a b\"  c", NULL, "=> text(=\"a b\"  c)");
    expect(SAY, "hi there -> out.txt", NULL, "=> text(=hi there) create=out.txt");
    expect(SAY, "hi -> b.txt there", NULL, "=> text(=hi there) screen=#2");
    expect(SAY, "them all", NULL, "=> text(=them all)");
    expect(SAY, "", NULL, "FIELD(0:-) UNANSWERED");
    const char *said[] = { "words:  well then  ", NULL };
    expect(SAY, "", said, "FIELD(0:-) => text(=well then)");
    expect(NOTE, "a.txt remember this", NULL, "=> thing(#1) text(=remember this)");
    expect(NOTE, "remember a.txt", NULL, "=> thing(#1) text(=remember a.txt)");
}

static void test_arrow(void)
{
    reset_volume();
    expect(TAG, "a.txt done -> b.txt", NULL, "=> things(#1) words(=done) screen=#2");
    expect(TAG, "a.txt done -> fresh", NULL, "=> things(#1) words(=done) create=fresh");
    expect(TAG, "a.txt done -> fresh (file)", NULL, "=> things(#1) words(=done) create=fresh");
    const char *pick[] = { "which:4", NULL };
    expect(TAG, "a.txt done -> note.txt", pick, "WHICH(note.txt:2) => things(#1) words(=done) screen=#4");
    expect(TAG, "a.txt done -> other.txt", NULL, "=> things(#1) words(=done) create=other.txt");
    expect(TAG, "everywhere a.txt done -> other.txt", NULL, "=> things(#1) words(=done) screen=#5");
    expect(COPY, "a.txt -> b.txt", NULL, "=> thing(#1) into(=b.txt)");
    expect(COPY, "a.txt -> fresh", NULL, "=> thing(#1) into(=fresh)");
    expect(COPY, "a.txt", NULL, "=> thing(#1) into()");
    expect("form 1\ntakes thing \"the file\"\ntakes into \"where\"\n", "a.txt", NULL,
           "FIELD(1:-) UNANSWERED");
    const char *where[] = { "words:out", NULL };
    expect("form 1\ntakes thing \"the file\"\ntakes into \"where\"\n", "a.txt", where,
           "FIELD(1:-) => thing(#1) into(=out)");
    expect("form 1\ntakes words \"w\"\n", "x -> y", NULL, "=> words(=x) create=y");
}

static void check_luggage(const char *label, const char *form_text, const char *line, const char *program,
                          const char *want, size_t want_length)
{
    BoxForm        form;
    BoxFormLookup  lookup = { stand_find, stand_all, g_them, g_them_count, g_them_known };
    BoxFormFiller *filler;
    BoxFormQuestion question;
    char          *bytes;
    size_t         length;

    form_parse(form_text, strlen(form_text), &form);
    form_fill_begin(&form, line, &lookup, &filler);
    form_fill_step(filler, &question);
    check_int(label, question.Step, BOX_FORM_DONE);
    if (program) form_fill_program(filler, program);
    check_int(label, form_fill_luggage(filler, 77, &bytes, &length), 0);
    check(length == want_length && memcmp(bytes, want, want_length) == 0, label, bytes, want);
    free(bytes);
    form_fill_end(filler);
    form_release(&form);
}

#define TAG_BYTES "tag a.txt done\0filled 1\0field things\0#1\0field words\0=done\0screen #77\0"

#define BYTES(label, form, line, program, text) \
    check_luggage(label, form, line, program, text, sizeof(text) - 1)

static void test_luggage_bytes(void)
{
    reset_volume();
    BYTES("bytes tag", TAG, "a.txt done", "tag", TAG_BYTES);
    BYTES("bytes arrow middle", TAG, "a.txt -> out done", "tag", TAG_BYTES);
    BYTES("bytes arrow first", TAG, "-> out a.txt done", "tag", TAG_BYTES);
    BYTES("bytes arrow last", TAG, "a.txt done -> out (file)", "tag", TAG_BYTES);
    BYTES("bytes no program", WORDS, "x  y", NULL, "x  y\0filled 1\0field words\0=x\0=y\0screen #77\0");
    BYTES("bytes no words", "form 1\n", "", "prog", "prog\0filled 1\0screen #77\0");
    BYTES("bytes undeclared", "form 1\n", "", NULL, "\0filled 1\0screen #77\0");
}

/* ---- the Luggage, read ---------------------------------------------------- */

static int decode(const char *form_text, const char *bytes, size_t length)
{
    BoxForm    form;
    FormValues values;
    file_id_t  screen;

    form_parse(form_text, strlen(form_text), &form);
    int rc = form_records_decode(form.Declared ? &form : NULL, bytes, length, &values, &screen);
    if (rc == 1) form_values_release(&values);
    form_release(&form);
    return rc;
}

#define BAD(label, form, text) \
    check_int("luggage " label, decode(form, text, sizeof(text) - 1), -ERR_INVALID_ARGUMENT)

static void test_luggage_read(void)
{
    check_int("luggage good", decode(TAG, "t\0filled 1\0field things\0#1\0field words\0=a\0", 42), 1);
    check_int("luggage line only", decode(TAG, "tag a.txt", 9), 0);
    check_int("luggage line and nul", decode(TAG, "tag a.txt\0", 10), 0);
    check_int("luggage empty", decode(TAG, "", 0), 0);
    check_int("luggage no form", decode("", "t\0filled 1\0", 11), 1);
    const char optional_empty[] = "t\0filled 1\0field thing\0#1\0field into\0";
    check_int("luggage optional empty", decode(COPY, optional_empty, sizeof(optional_empty) - 1), 1);
    const char big_id[] = "t\0filled 1\0field things\0#18446744073709551615\0field words\0=a\0";
    check_int("luggage big id", decode(TAG, big_id, sizeof(big_id) - 1), 1);

    BAD("no filled", TAG, "t\0field things\0#1\0field words\0=a\0");
    BAD("filled 2", TAG, "t\0filled 2\0field things\0#1\0field words\0=a\0");
    BAD("filled twice", TAG, "t\0filled 1\0filled 1\0field things\0#1\0field words\0=a\0");
    BAD("not terminated", TAG, "t\0filled 1\0field things\0#1\0field words\0=a");
    BAD("empty record", TAG, "t\0filled 1\0\0field things\0#1\0field words\0=a\0");
    BAD("wrong kind", TAG, "t\0filled 1\0field thing\0#1\0field words\0=a\0");
    BAD("unknown kind", TAG, "t\0filled 1\0field bogus\0#1\0field words\0=a\0");
    BAD("missing field", TAG, "t\0filled 1\0field things\0#1\0");
    BAD("extra field", TAG, "t\0filled 1\0field things\0#1\0field words\0=a\0field words\0=b\0");
    BAD("required empty", TAG, "t\0filled 1\0field things\0field words\0=a\0");
    BAD("no field", TAG, "t\0filled 1\0#1\0field things\0#1\0field words\0=a\0");
    BAD("id zero", TAG, "t\0filled 1\0field things\0#0\0field words\0=a\0");
    BAD("id leading zero", TAG, "t\0filled 1\0field things\0#01\0field words\0=a\0");
    BAD("id letters", TAG, "t\0filled 1\0field things\0#1x\0field words\0=a\0");
    BAD("id empty", TAG, "t\0filled 1\0field things\0#\0field words\0=a\0");
    BAD("id signed", TAG, "t\0filled 1\0field things\0#-1\0field words\0=a\0");
    BAD("id overflow", TAG, "t\0filled 1\0field things\0#18446744073709551616\0field words\0=a\0");
    BAD("id too long", TAG, "t\0filled 1\0field things\0#100000000000000000000\0field words\0=a\0");
    BAD("word in things", TAG, "t\0filled 1\0field things\0#1\0=x\0field words\0=a\0");
    BAD("id in words", TAG, "t\0filled 1\0field things\0#1\0field words\0#2\0");
    BAD("empty word", TAG, "t\0filled 1\0field things\0#1\0field words\0=\0");
    BAD("two ids in thing", RENAME, "t\0filled 1\0field thing\0#1\0#2\0field name\0=a\0");
    BAD("two names", RENAME, "t\0filled 1\0field thing\0#1\0field name\0=a\0=b\0");
    BAD("two texts", SAY, "t\0filled 1\0field text\0=a\0=b\0");
    BAD("two screens", TAG, "t\0filled 1\0field things\0#1\0field words\0=a\0screen #2\0screen #3\0");
    BAD("bad screen", TAG, "t\0filled 1\0field things\0#1\0field words\0=a\0screen #x\0");
    BAD("unknown record", TAG, "t\0filled 1\0field things\0#1\0field words\0=a\0bogus\0");
    BAD("field for no form", "", "t\0filled 1\0field words\0=a\0");

    const char screen[] = "t\0filled 1\0field things\0#1\0field words\0=a\0screen #42\0";
    check_int("screen read", (long)form_records_screen(screen, sizeof(screen) - 1), 42);
    check_int("screen absent", (long)form_records_screen("t\0filled 1\0", 11), 0);
    check_int("screen bad", (long)form_records_screen("t\0filled 1\0screen #0x\0", 22), 0);
    check_int("screen twice", (long)form_records_screen("t\0screen #1\0screen #2\0", 22), 0);
    check_int("screen line only", (long)form_records_screen("screen #1", 9), 0);
}

/* ---- the Form's text ------------------------------------------------------ */

static int parse(const char *text, BoxForm *form)
{
    return form_parse(text, strlen(text), form);
}

static void test_form_parse(void)
{
    BoxForm form;

    check_int("form good", parse(TAG, &form), 0);
    check_int("form fields", form.FieldCount, 2);
    check_int("form kind 0", form.Fields[0].Kind, BOX_FORM_THINGS);
    check_text("form what", form.Fields[0].What, "the files to tag");
    check_int("form gives", form.Gives, BOX_FORM_GIVES_THINGS);
    check_int("form declared", form.Declared, 1);
    form_release(&form);

    check_int("form optional", parse("form 1\ntakes optional name \"n\"\ngives text\n", &form), 0);
    check_int("form optional flag", form.Fields[0].Optional, 1);
    check_int("form gives text", form.Gives, BOX_FORM_GIVES_TEXT);
    form_release(&form);

    check_int("form every kind", parse("form 1\ntakes thing \"a\"\ntakes things \"b\"\ntakes name \"c\"\n"
                                       "takes words \"d\"\ntakes text \"e\"\ntakes into \"f\"\n", &form), 0);
    check_int("form kind into", form.Fields[5].Kind, BOX_FORM_INTO);
    form_release(&form);

    check_int("form no what", parse("form 1\ntakes words\n", &form), 0);
    check_text("form no what text", form.Fields[0].What, "");
    form_release(&form);

    check_int("form no final newline", parse("form 1\ntakes words \"w\"", &form), 0);
    check_int("form no final newline fields", form.FieldCount, 1);
    form_release(&form);

    check_int("form unknown lines", parse("form 1\nfrobnicate yes\n\ntakes words \"w\"\ngives bogus\n"
                                          "  \nwhatever \"x\"\n", &form), 0);
    check_int("form unknown lines fields", form.FieldCount, 1);
    check_int("form unknown gives", form.Gives, BOX_FORM_GIVES_NOTHING);
    form_release(&form);

    check_int("form nul ends", form_parse("form 1\ntakes words \"w\"\n\0takes words \"x\"\n", 40, &form), 0);
    check_int("form nul ends fields", form.FieldCount, 1);
    form_release(&form);

    check_int("form head only", parse("form 1", &form), 0);
    check_int("form head only fields", form.FieldCount, 0);
    form_release(&form);
}

static void test_form_refused(void)
{
    BoxForm form;
    long bad = -ERR_INVALID_ARGUMENT;

    check_int("refuse empty", parse("", &form), bad);
    check_int("refuse no head", parse("takes words \"w\"\n", &form), bad);
    check_int("refuse form 2", parse("form 2\ntakes words \"w\"\n", &form), bad);
    check_int("refuse form 10", parse("form 10\n", &form), bad);
    check_int("refuse head glued", parse("form 1x\n", &form), bad);
    check_int("refuse unknown kind", parse("form 1\ntakes bogus \"w\"\n", &form), bad);
    check_int("refuse no kind", parse("form 1\ntakes\n", &form), bad);
    check_int("refuse optional only", parse("form 1\ntakes optional\n", &form), bad);
    check_int("refuse open quote", parse("form 1\ntakes words \"w\n", &form), bad);
    check_int("refuse junk after what", parse("form 1\ntakes words \"w\" more\n", &form), bad);
    check_int("refuse what not quoted", parse("form 1\ntakes words w\n", &form), bad);
    check_int("refuse null", form_parse(NULL, 0, &form), bad);
    check_int("refuse no out", form_parse("form 1\n", 7, NULL), bad);
    check_int("refused leaves nothing", form.Declared, 0);

    char many[512] = "form 1\n";
    for (int i = 0; i < 16; i++) strcat(many, "takes name \"n\"\n");
    check_int("sixteen fields", parse(many, &form), 0);
    check_int("sixteen count", form.FieldCount, 16);
    form_release(&form);
    strcat(many, "takes name \"n\"\n");
    check_int("seventeen fields", parse(many, &form), bad);
}

static void test_sixteen(void)
{
    char form_text[512] = "form 1\n";
    for (int i = 0; i < 16; i++) strcat(form_text, "takes optional name \"n\"\n");

    reset_volume();
    expect(form_text, "a b c d e f g h i j k l m n o p", NULL,
           "=> name(=a) name(=b) name(=c) name(=d) name(=e) name(=f) name(=g) name(=h) "
           "name(=i) name(=j) name(=k) name(=l) name(=m) name(=n) name(=o) name(=p)");
    expect(form_text, "a b c d e f g h i j k l m n o p q", NULL,
           "REFUSED: q has no place in this command");
    expect(form_text, "a", NULL, "=> name(=a) name() name() name() name() name() name() name() name() "
                                 "name() name() name() name() name() name() name()");
}

static void test_lookup_trouble(void)
{
    reset_volume();
    g_find_fails = "a.txt";
    expect(TAG, "a.txt done", NULL, "REFUSED: the volume would not say what a.txt is");
    g_find_fails = "out";
    expect(TAG, "b.txt done -> out", NULL, "REFUSED: the volume would not say what out is");
    reset_volume();

    BoxForm        form;
    BoxFormLookup  lookup = { stand_find, stand_all, g_them, 0, false };
    BoxFormFiller *filler;
    BoxFormQuestion question;

    parse(TAG, &form);
    check_int("begin no line", form_fill_begin(&form, NULL, &lookup, &filler), -ERR_INVALID_ARGUMENT);
    check_int("begin no lookup", form_fill_begin(&form, "a.txt", NULL, &filler), -ERR_INVALID_ARGUMENT);
    check_int("begin no form", form_fill_begin(NULL, "a.txt", &lookup, &filler), -ERR_INVALID_ARGUMENT);
    form_fill_begin(&form, "note.txt done -> note.txt", &lookup, &filler);
    check_int("choose before ask", form_fill_choose(filler, 3), -ERR_INVALID_ARGUMENT);
    check_int("words before ask", form_fill_words(filler, "x"), -ERR_INVALID_ARGUMENT);
    check_int("sure before ask", form_fill_sure(filler, true), -ERR_INVALID_ARGUMENT);
    file_id_t screen;
    const char *create;
    char *bytes;
    size_t length;
    check_int("arrow before done", form_fill_arrow(filler, &screen, &create), -ERR_INVALID_ARGUMENT);
    check_int("luggage before done", form_fill_luggage(filler, 0, &bytes, &length), -ERR_INVALID_ARGUMENT);
    form_fill_step(filler, &question);
    check_int("which asked", question.Step, BOX_FORM_ASK_WHICH);
    check_int("sure on which", form_fill_sure(filler, true), -ERR_INVALID_ARGUMENT);
    check_int("words on which", form_fill_words(filler, "x"), -ERR_INVALID_ARGUMENT);
    check_int("choose stranger", form_fill_choose(filler, 5), -ERR_INVALID_ARGUMENT);
    check_int("choose one", form_fill_choose(filler, 4), 0);
    form_fill_step(filler, &question);
    check_int("arrow which", question.Step, BOX_FORM_ASK_WHICH);
    check_int("arrow choose", form_fill_choose(filler, 3), 0);
    form_fill_step(filler, &question);
    check_int("done", question.Step, BOX_FORM_DONE);
    form_fill_step(filler, &question);
    check_int("done again", question.Step, BOX_FORM_DONE);
    check_int("arrow", form_fill_arrow(filler, &screen, &create), 0);
    check_int("arrow screen", (long)screen, 3);
    form_fill_end(filler);
    form_release(&form);

    parse(TAG, &form);
    form_fill_begin(&form, "a.txt, done", &lookup, &filler);
    form_fill_step(filler, &question);
    check_int("refused", question.Step, BOX_FORM_REFUSED);
    form_fill_step(filler, &question);
    check_int("refused stays", question.Step, BOX_FORM_REFUSED);
    check_int("answer a refusal", form_fill_words(filler, "x"), -ERR_INVALID_ARGUMENT);
    form_fill_end(filler);
    form_release(&form);
}

static void test_undeclared(void)
{
    reset_volume();
    BoxForm        form;
    BoxFormLookup  lookup = { stand_find, stand_all, g_them, 0, false };
    BoxFormFiller *filler;
    BoxFormQuestion question;
    char          *bytes;
    size_t         length;

    memset(&form, 0, sizeof(form));
    form_fill_begin(&form, "a b -> out", &lookup, &filler);
    form_fill_step(filler, &question);
    check_int("undeclared done", question.Step, BOX_FORM_DONE);
    form_fill_program(filler, "prog");
    form_fill_luggage(filler, 9, &bytes, &length);
    check(length == 28 && memcmp(bytes, "prog a b\0filled 1\0screen #9\0", 28) == 0,
          "undeclared bytes", bytes, "prog a b");
    free(bytes);
    form_fill_end(filler);
}

/* ---- the image ------------------------------------------------------------ */

typedef struct {
    bool shoff_past_end, shentsize_bad, shstrndx_bad, form_past_end, form_huge, nobits,
         names_unterminated, no_form, class32, no_names_room, trailing_text;
} Fault;

static void put16(unsigned char *at, unsigned value) { at[0] = (unsigned char)value; at[1] = (unsigned char)(value >> 8); }
static void put32(unsigned char *at, uint64_t value) { for (int i = 0; i < 4; i++) at[i] = (unsigned char)(value >> (8 * i)); }
static void put64(unsigned char *at, uint64_t value) { for (int i = 0; i < 8; i++) at[i] = (unsigned char)(value >> (8 * i)); }

static void put_section(unsigned char *at, uint32_t name, uint32_t type, uint64_t offset, uint64_t size)
{
    put32(at, name);
    put32(at + 4, type);
    put64(at + 24, offset);
    put64(at + 32, size);
}

static void build_image(const char *form_text, Fault fault)
{
    static const char names_ok[] = "\0.text\0.boxos.form\0.shstrtab";
    static const char names_bad[] = "\0.text\0.boxos.form";
    size_t text_length = strlen(form_text);
    size_t names_length = fault.names_unterminated ? sizeof(names_bad) - 1 : sizeof(names_ok);
    size_t text_at = 64, names_at = text_at + text_length, headers_at = (names_at + names_length + 7) & ~(size_t)7;

    free(g_image);
    g_image_size = headers_at + 4 * 64;
    g_image = calloc(1, g_image_size);
    memcpy(g_image, "\x7f" "ELF", 4);
    g_image[4] = fault.class32 ? 1 : 2;
    g_image[5] = 1;
    put64(g_image + 40, fault.shoff_past_end ? g_image_size : headers_at);
    put16(g_image + 58, fault.shentsize_bad ? 40 : 64);
    put16(g_image + 60, 4);
    put16(g_image + 62, fault.shstrndx_bad ? 9 : 3);

    memcpy(g_image + text_at, form_text, text_length);
    memcpy(g_image + names_at, fault.names_unterminated ? names_bad : names_ok, names_length);
    unsigned char *headers = g_image + headers_at;
    put_section(headers + 64, 1, 1, 0, 0);
    put_section(headers + 128, fault.no_form ? 1 : 7, fault.nobits ? 8 : 1,
                fault.form_past_end ? g_image_size : text_at,
                fault.form_huge ? 1u << 20 : text_length);
    put_section(headers + 192, fault.names_unterminated ? 1 : 19, 3, names_at, names_length);
}

static void test_image(void)
{
    BoxForm form;
    Fault clean = { 0 };

    build_image(TAG, clean);
    check_int("image good", form_of_file(7, &form), 0);
    check_int("image declared", form.Declared, 1);
    check_int("image fields", form.FieldCount, 2);
    check_text("image what", form.Fields[1].What, "the tags they get");
    form_release(&form);

    Fault no_form = { .no_form = true };
    build_image(TAG, no_form);
    check_int("image no section", form_of_file(7, &form), 0);
    check_int("image no section declared", form.Declared, 0);

    Fault class32 = { .class32 = true };
    build_image(TAG, class32);
    check_int("image 32 bit", form_of_file(7, &form), 0);
    check_int("image 32 bit declared", form.Declared, 0);

    g_image_size = 20;
    check_int("image tiny", form_of_file(7, &form), 0);
    g_image_size = 0;
    check_int("image empty", form_of_file(7, &form), 0);

    Fault shoff = { .shoff_past_end = true }, size = { .shentsize_bad = true },
          strndx = { .shstrndx_bad = true }, past = { .form_past_end = true },
          huge = { .form_huge = true }, nobits = { .nobits = true },
          names = { .names_unterminated = true };
    build_image(TAG, shoff);
    check_int("image shoff past end", form_of_file(7, &form), -ERR_CORRUPTED);
    build_image(TAG, size);
    check_int("image shentsize", form_of_file(7, &form), -ERR_CORRUPTED);
    build_image(TAG, strndx);
    check_int("image shstrndx", form_of_file(7, &form), -ERR_CORRUPTED);
    build_image(TAG, past);
    check_int("image section past end", form_of_file(7, &form), -ERR_CORRUPTED);
    build_image(TAG, huge);
    check_int("image section huge", form_of_file(7, &form), -ERR_CORRUPTED);
    build_image(TAG, nobits);
    check_int("image nobits", form_of_file(7, &form), -ERR_CORRUPTED);
    build_image(TAG, names);
    check_int("image names unterminated", form_of_file(7, &form), 0);
    check_int("image names unterminated declared", form.Declared, 0);

    build_image("form 2\n", clean);
    check_int("image form 2", form_of_file(7, &form), -ERR_INVALID_ARGUMENT);
    build_image("form 1\ntakes bogus \"x\"\n", clean);
    check_int("image bad form", form_of_file(7, &form), -ERR_INVALID_ARGUMENT);

    char with_nul[80] = "form 1\ntakes name \"n\"\n";
    build_image(with_nul, clean);
    check_int("image form with trailing nul", form_of_file(7, &form), 0);
    check_int("image form with trailing nul fields", form.FieldCount, 1);
    form_release(&form);
    free(g_image);
    g_image = NULL;
    g_image_size = 0;
}

static void test_cut(void)
{
    FormTokens tokens;

    form_line_cut("  a \"b c\",d ,, e\t\"f", &tokens);
    check_int("cut count", tokens.Count, 5);
    check_text("cut 0", tokens.Tokens[0].Text, "a");
    check_text("cut 1", tokens.Tokens[1].Text, "b c");
    check_int("cut 1 quoted", tokens.Tokens[1].Quoted, 1);
    check_int("cut 1 comma after", tokens.Tokens[1].CommaAfter, 1);
    check_text("cut 2", tokens.Tokens[2].Text, "d");
    check_int("cut 2 comma before", tokens.Tokens[2].CommaBefore, 1);
    check_int("cut 2 comma after", tokens.Tokens[2].CommaAfter, 1);
    check_text("cut 3", tokens.Tokens[3].Text, "e");
    check_int("cut 3 comma before", tokens.Tokens[3].CommaBefore, 1);
    check_text("cut 4", tokens.Tokens[4].Text, "f");
    check_int("cut 1 start", tokens.Tokens[1].Start, 4);
    check_int("cut 1 end", tokens.Tokens[1].End, 9);
    form_line_release(&tokens);

    form_line_cut("", &tokens);
    check_int("cut empty", tokens.Count, 0);
    form_line_release(&tokens);
    form_line_cut("   ", &tokens);
    check_int("cut blanks", tokens.Count, 0);
    form_line_release(&tokens);
    form_line_cut("\"a,b\" c,", &tokens);
    check_int("cut quoted comma", tokens.Count, 2);
    check_text("cut quoted comma 0", tokens.Tokens[0].Text, "a,b");
    check_int("cut trailing comma", tokens.Tokens[1].CommaAfter, 1);
    form_line_release(&tokens);
}

static void test_screen_file(void)
{
    static const char bytes[] = "tag a.txt\0filled 1\0field things\0#1\0screen #42\0";

    g_luggage_bytes  = bytes;
    g_luggage_length = sizeof(bytes) - 1;
    check_int("screen file", (long)form_screen_file(), 42);
    g_luggage_length = 0;
    check_int("screen file is read once", (long)form_screen_file(), 42);
}

int main(void)
{
    reset_volume();
    test_cut();
    test_form_parse();
    test_form_refused();
    test_image();
    test_examples();
    test_order_and_lists();
    test_shell_words();
    test_which();
    test_fields();
    test_leftovers();
    test_text();
    test_arrow();
    test_luggage_bytes();
    test_luggage_read();
    test_sixteen();
    test_lookup_trouble();
    test_undeclared();
    test_screen_file();
    printf("form: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
