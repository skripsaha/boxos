/*
 * form.c — a program's Form: its text read into fields, and found in an image.
 *
 * The text is whatever a program put in .boxos.form, so it is untrusted
 * bytes: every line is read inside its own bounds, a field the reader cannot
 * fill is refused rather than passed over (a shifted field would sort every
 * word wrong), and a line of a kind the reader does not know is passed over,
 * as the contract says. The image is read at offsets (image_section.c).
 */

#include "box/form.h"
#include "box/core/form_internal.h"
#include "box/core/image_section.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define FORM_TEXT_MAX      65536u
#define FORM_SECTION_NAME  ".boxos.form"

static const char *const g_kind_names[] = {
    "thing", "things", "name", "words", "text", "into"
};

const char *form_kind_name(uint8_t kind)
{
    if (kind >= sizeof(g_kind_names) / sizeof(g_kind_names[0])) return NULL;
    return g_kind_names[kind];
}

int form_kind_of(const char *name, size_t length)
{
    for (size_t i = 0; i < sizeof(g_kind_names) / sizeof(g_kind_names[0]); i++) {
        if (strlen(g_kind_names[i]) == length && memcmp(g_kind_names[i], name, length) == 0)
            return (int)i;
    }
    return -1;
}

/* ---- the text --------------------------------------------------------- */

typedef struct {
    const char *At;
    const char *End;
} FormCursor;

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static void skip_blanks(FormCursor *cursor)
{
    while (cursor->At < cursor->End && blank(*cursor->At)) cursor->At++;
}

static size_t take_word(FormCursor *cursor, const char **word)
{
    skip_blanks(cursor);
    *word = cursor->At;
    while (cursor->At < cursor->End && !blank(*cursor->At)) cursor->At++;
    return (size_t)(cursor->At - *word);
}

static bool word_is(const char *word, size_t length, const char *text)
{
    return strlen(text) == length && memcmp(word, text, length) == 0;
}

/* The `"what it is"` that ends a takes line; none is an empty string. */
static int take_what(FormCursor *cursor, char **what)
{
    skip_blanks(cursor);
    const char *start = cursor->At;
    size_t      length = 0;

    if (cursor->At < cursor->End) {
        if (*cursor->At != '"') return -ERR_INVALID_ARGUMENT;
        start = ++cursor->At;
        while (cursor->At < cursor->End && *cursor->At != '"') cursor->At++;
        if (cursor->At >= cursor->End) return -ERR_INVALID_ARGUMENT;
        length = (size_t)(cursor->At - start);
        cursor->At++;
        skip_blanks(cursor);
        if (cursor->At < cursor->End) return -ERR_INVALID_ARGUMENT;
    }

    char *copy = malloc(length + 1);
    if (!copy) return -ERR_NO_MEMORY;
    memcpy(copy, start, length);
    copy[length] = '\0';
    *what = copy;
    return 0;
}

static int parse_takes(FormCursor *cursor, BoxForm *out)
{
    const char *word;
    size_t      length = take_word(cursor, &word);
    uint8_t     optional = 0;

    if (word_is(word, length, "optional")) {
        optional = 1;
        length = take_word(cursor, &word);
    }
    int kind = form_kind_of(word, length);
    if (kind < 0 || out->FieldCount >= BOX_FORM_FIELDS) return -ERR_INVALID_ARGUMENT;

    char *what;
    int rc = take_what(cursor, &what);
    if (rc != 0) return rc;

    BoxFormField *field = &out->Fields[out->FieldCount++];
    field->Kind     = (uint8_t)kind;
    field->Optional = optional;
    field->What     = what;
    return 0;
}

static void parse_gives(FormCursor *cursor, BoxForm *out)
{
    const char *word;
    size_t      length = take_word(cursor, &word);

    if (word_is(word, length, "things")) out->Gives = BOX_FORM_GIVES_THINGS;
    else if (word_is(word, length, "text")) out->Gives = BOX_FORM_GIVES_TEXT;
}

static int parse_line(const char *start, const char *end, BoxForm *out)
{
    FormCursor  cursor = { start, end };
    const char *verb;
    size_t      length = take_word(&cursor, &verb);

    if (word_is(verb, length, "takes")) return parse_takes(&cursor, out);
    if (word_is(verb, length, "gives")) parse_gives(&cursor, out);
    return 0;
}

int form_parse(const char *text, size_t length, BoxForm *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!text) return -ERR_INVALID_ARGUMENT;

    const char *nul = memchr(text, '\0', length);
    const char *end = nul ? nul : text + length;
    const char *at  = text;

    static const char head[] = "form 1";
    size_t head_length = sizeof(head) - 1;
    if ((size_t)(end - at) < head_length || memcmp(at, head, head_length) != 0)
        return -ERR_INVALID_ARGUMENT;
    at += head_length;
    if (at < end && *at != '\n') return -ERR_INVALID_ARGUMENT;

    while (at < end) {
        const char *line = at + 1;
        const char *stop = memchr(line, '\n', (size_t)(end - line));
        if (!stop) stop = end;
        int rc = parse_line(line, stop, out);
        if (rc != 0) {
            form_release(out);
            return rc;
        }
        at = stop;
    }
    out->Declared = 1;
    return 0;
}

void form_release(BoxForm *form)
{
    if (!form) return;
    for (uint32_t i = 0; i < form->FieldCount; i++) free(form->Fields[i].What);
    memset(form, 0, sizeof(*form));
}

/* ---- the image -------------------------------------------------------- */

int form_of_file(file_id_t program, BoxForm *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    char  *text;
    size_t size;
    int    rc = image_section_read(program, FORM_SECTION_NAME, FORM_TEXT_MAX, &text, &size, NULL);
    /* BoxASM's linker writes the section's header in every image, empty for
     * a program that declares no Form: that is a program without one. */
    if (rc <= 0 || !text) return rc < 0 ? rc : 0;
    rc = form_parse(text, size, out);
    free(text);
    return rc;
}
