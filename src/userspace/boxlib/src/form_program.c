/*
 * form_program.c — a program reading its Form filled.
 *
 * The shell fills the Form before the program starts and hands it over in the
 * Luggage. A program started by anyone else gets the same fields from the
 * same filler: the first ask fills the Form from the line, over this
 * program's own volume and Use Context, asking nobody. A question cannot be
 * asked there, so it is said once on the screen as what the program needs,
 * and every ask after answers -ERR_INVALID_ARGUMENT.
 *
 * Strands share the cabin: the first to ask does the filling and the others
 * wait for it, as luggage.c does its cutting.
 */

#include "box/form.h"
#include "box/core/form_internal.h"
#include "box/file.h"
#include "box/luggage.h"
#include "box/print.h"
#include "box/use.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"
#include "box/core/result.h"
#include "box/core/cabin.h"
#include "box/program_part.h"

/* The Form an ELF image carries, between the symbols user.ld or the BoxASM
 * linker puts round .boxos.form — at one address when it declares none.
 * Inside the image, so hidden: reached relative to the code. */
extern const char __boxos_form_start[] __attribute__((visibility("hidden")));
extern const char __boxos_form_end[] __attribute__((visibility("hidden")));

/* This program's own Form text, *text NULL for none: a box's part `form`, which
 * the kernel put into its memory at start (a box names everything it has in
 * memory in its table, so a box without it there has none); an ELF image's,
 * between the symbols round its section. */
static void own_form_text(const char **text, size_t *size)
{
    *text = NULL;
    *size = 0;
    if (cabin_info()->parts_addr != 0) {
        const uint8_t *loaded;
        if (program_part_loaded("form", &loaded, size) == 1) *text = (const char *)loaded;
        return;
    }
    if ((uintptr_t)__boxos_form_end > (uintptr_t)__boxos_form_start) {
        *text = __boxos_form_start;
        *size = (size_t)(__boxos_form_end - __boxos_form_start);
    }
}

/* ---- the lookups ------------------------------------------------------- */

static bool called(file_id_t id, const char *name)
{
    FileMetadata metadata;
    if (FileMetadataRead(MY_TAGS, id, &metadata) != 0) return false;
    bool same = metadata.Filename && strcmp(metadata.Filename, name) == 0;
    FileMetadataRelease(&metadata);
    return same;
}

/* A name the person typed means a file in use: a deleted one is
 * not it. */
static int lookup_find(BoxFormLookup *lookup, const char *name, bool everywhere, file_id_t **ids)
{
    file_id_t *found = NULL;
    *ids = NULL;
    if (name[0] == '\0') return 0;

    int n = everywhere ? query_name_all_everywhere(PERSON_TAGS, name, NULL, NULL, &found)
                       : query_name_all(PERSON_TAGS, name, NULL, NULL, &found);
    if (n < 0) return n;

    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (called(found[i], name)) found[kept++] = found[i];
    }
    if (kept == 0) {
        free(found);
        found = NULL;
    }
    *ids = found;
    return kept;
}

/* `all` lists what there is: every file in use, the deleted aside. */
static int lookup_all(BoxFormLookup *lookup, bool everywhere, file_id_t **ids, bool *bounded)
{
    *ids = NULL;
    if (everywhere) {
        *bounded = false;
        return query_all_everywhere(PERSON_TAGS, NULL, NULL, ids);
    }

    char   tags[256];
    size_t needed = 0;
    int    tag_count = use_get(tags, sizeof(tags), &needed);
    *bounded = tag_count > 0 || needed > 1;
    return query_all(PERSON_TAGS, NULL, NULL, ids);
}

void form_lookup_volume(BoxFormLookup *lookup)
{
    memset(lookup, 0, sizeof(*lookup));
    lookup->Find = lookup_find;
    lookup->All = lookup_all;
}

/* ---- the filling ------------------------------------------------------- */

static struct {
    volatile uint32_t claimed;
    volatile uint32_t ready;
    bool              usable;
    FormValues        values;
} g_program;

static void say(const char *program, const char *text, const char *more)
{
    print_to_screen(program);
    print_to_screen(": ");
    print_to_screen(text);
    if (more) print_to_screen(more);
    print_to_screen("\n");
}

static const char *program_name(void)
{
    const char *name = luggage_word(0);
    return name ? name : "this program";
}

/* A question has no one to ask here: what the program needs, said once. */
static void say_question(const BoxForm *form, const BoxFormQuestion *question)
{
    const char *program = program_name();

    switch (question->Step) {
    case BOX_FORM_ASK_WHICH:
        say(program, question->Word, " means more than one file");
        break;
    case BOX_FORM_ASK_SURE:
        say(program, "all would be the whole volume, nothing is bounded by a Use Context", NULL);
        break;
    case BOX_FORM_ASK_FIELD:
        if (question->Word) {
            print_to_screen(program);
            print_to_screen(": ");
            print_to_screen(question->Word);
            print_to_screen(", ");
            print_to_screen(form->Fields[question->Field].What);
            print_to_screen("?\n");
        } else {
            say(program, form->Fields[question->Field].What, "?");
        }
        break;
    default:
        say(program, question->Word, NULL);
        break;
    }
}

static int fill_own_line(const BoxForm *form, FormValues *values)
{
    BoxFormLookup lookup;
    BoxFormFiller *filler;
    BoxFormQuestion question;

    form_lookup_volume(&lookup);
    int rc = form_fill_begin(form, luggage_tail(1), &lookup, &filler);
    if (rc != 0) return rc;
    rc = form_fill_step(filler, &question);
    if (rc == 0 && question.Step != BOX_FORM_DONE) {
        say_question(form, &question);
        rc = -ERR_INVALID_ARGUMENT;
    }

    file_id_t   screen;
    const char *create;
    if (rc == 0) rc = form_fill_arrow(filler, &screen, &create);
    if (rc == 0 && (screen != 0 || create != NULL)) {
        say(program_name(), "-> is the hub's: this program was not started by it", NULL);
        rc = -ERR_INVALID_ARGUMENT;
    }

    char  *bytes = NULL;
    size_t length = 0;
    if (rc == 0) rc = form_fill_luggage(filler, 0, &bytes, &length);
    form_fill_end(filler);
    if (rc != 0) return rc;

    rc = form_records_decode(form, bytes, length, values, &screen);
    free(bytes);
    return rc == 1 ? 0 : -ERR_INVALID_ARGUMENT;
}

/* Words joined by single spaces, for form_text. */
static int join_words(FormFieldValue *field)
{
    size_t length = 0;
    for (size_t i = 0; i < field->WordCount; i++) length += strlen(field->Words[i]) + 1;

    char *joined = malloc(length + 1);
    if (!joined) return -ERR_NO_MEMORY;
    size_t at = 0;
    for (size_t i = 0; i < field->WordCount; i++) {
        size_t n = strlen(field->Words[i]);
        if (i > 0) joined[at++] = ' ';
        memcpy(joined + at, field->Words[i], n);
        at += n;
    }
    joined[at] = '\0';
    field->Joined = joined;
    return 0;
}

static int read_filled(const BoxForm *form, FormValues *values)
{
    Luggage l = luggage_carried();
    file_id_t screen;
    int rc = l.length ? form_records_decode(form->Declared ? form : NULL, l.bytes, l.length,
                                            values, &screen)
                      : 0;
    if (rc < 0) {
        say(program_name(), "the Luggage is not what this program's Form wants", NULL);
        return rc;
    }
    if (rc == 1) return 0;
    if (!form->Declared) {
        say(program_name(), "this program has no Form", NULL);
        return -ERR_INVALID_ARGUMENT;
    }
    return fill_own_line(form, values);
}

static int fill_program(FormValues *values)
{
    BoxForm form;
    memset(&form, 0, sizeof(form));

    const char *text;
    size_t      size;
    own_form_text(&text, &size);
    int rc = text ? form_parse(text, size, &form) : 0;
    if (rc != 0) {
        say(program_name(), "its Form is not one this library reads", NULL);
        return rc;
    }
    rc = read_filled(&form, values);
    for (uint32_t i = 0; rc == 0 && i < values->FieldCount; i++) {
        if (values->Fields[i].Kind != BOX_FORM_THING && values->Fields[i].Kind != BOX_FORM_THINGS)
            rc = join_words(&values->Fields[i]);
    }
    if (rc != 0) form_values_release(values);
    form_release(&form);
    return rc;
}

static void prepare(void)
{
    if (__atomic_load_n(&g_program.ready, __ATOMIC_ACQUIRE)) return;

    if (!__sync_bool_compare_and_swap(&g_program.claimed, 0u, 1u)) {
        while (!__atomic_load_n(&g_program.ready, __ATOMIC_ACQUIRE)) {
            if (result_step_aside()) yield();
            else __asm__ volatile("pause");
        }
        return;
    }
    g_program.usable = fill_program(&g_program.values) == 0;
    __atomic_store_n(&g_program.ready, 1u, __ATOMIC_RELEASE);
}

/* ---- what a program reads ---------------------------------------------- */

static const FormFieldValue *field_of(uint32_t field)
{
    prepare();
    if (!g_program.usable || field >= g_program.values.FieldCount) return NULL;
    return &g_program.values.Fields[field];
}

static bool field_takes_things(const FormFieldValue *value)
{
    return value->Kind == BOX_FORM_THING || value->Kind == BOX_FORM_THINGS;
}

int form_things(uint32_t field, const file_id_t **ids, size_t *count)
{
    static const file_id_t none[1] = { 0 };
    const FormFieldValue *value = field_of(field);

    if (!ids || !count || !value || !field_takes_things(value)) return -ERR_INVALID_ARGUMENT;
    *ids   = value->IdCount ? value->Ids : none;
    *count = value->IdCount;
    return 0;
}

int form_words(uint32_t field, const char *const **words, size_t *count)
{
    static const char *const none[1] = { NULL };
    const FormFieldValue *value = field_of(field);

    if (!words || !count || !value || field_takes_things(value)) return -ERR_INVALID_ARGUMENT;
    *words = value->WordCount ? (const char *const *)value->Words : none;
    *count = value->WordCount;
    return 0;
}

const char *form_text(uint32_t field)
{
    const FormFieldValue *value = field_of(field);
    return value && !field_takes_things(value) ? value->Joined : NULL;
}
