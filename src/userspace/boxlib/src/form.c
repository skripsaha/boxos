/*
 * form.c — a program's Form: its text read into fields, and found in an image.
 *
 * The text is whatever a program put in .boxos.form, so it is untrusted
 * bytes: every line is read inside its own bounds, a field the reader cannot
 * fill is refused rather than passed over (a shifted field would sort every
 * word wrong), and a line of a kind the reader does not know is passed over,
 * as the contract says. The image is read at offsets — the ELF header, the
 * section headers, the name table and the one section — never whole.
 */

#include "box/form.h"
#include "box/core/form_internal.h"
#include "box/file.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define FORM_TEXT_MAX      65536u
#define FORM_SECTION_NAME  ".boxos.form"
#define FORM_HEADER_BATCH  16u

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

typedef struct {
    uint8_t  Ident[16];
    uint16_t Type;
    uint16_t Machine;
    uint32_t Version;
    uint64_t Entry;
    uint64_t ProgramHeaders;
    uint64_t SectionHeaders;
    uint32_t Flags;
    uint16_t HeaderSize;
    uint16_t ProgramHeaderSize;
    uint16_t ProgramHeaderCount;
    uint16_t SectionHeaderSize;
    uint16_t SectionHeaderCount;
    uint16_t NameSection;
} FormElfHeader;

typedef struct {
    uint32_t Name;
    uint32_t Type;
    uint64_t Flags;
    uint64_t Address;
    uint64_t Offset;
    uint64_t Size;
    uint32_t Link;
    uint32_t Info;
    uint64_t Align;
    uint64_t EntrySize;
} FormElfSection;

_Static_assert(sizeof(FormElfHeader) == 64, "an ELF64 header is 64 bytes");
_Static_assert(sizeof(FormElfSection) == 64, "an ELF64 section header is 64 bytes");

#define FORM_ELF_CLASS_64   2u
#define FORM_ELF_LITTLE     1u
#define FORM_SECTION_NOBITS 8u

static int read_exact(file_id_t file, uint64_t offset, void *buffer, size_t size)
{
    int64_t got = fread(file, offset, buffer, size);
    if (got < 0) return (int)got;
    return (size_t)got == size ? 0 : -ERR_CORRUPTED;
}

static bool span_inside(uint64_t offset, uint64_t size, uint64_t whole)
{
    return offset <= whole && size <= whole - offset;
}

static bool name_is_form(const char *names, uint64_t names_size, uint32_t at)
{
    static const char want[] = FORM_SECTION_NAME;
    if (at >= names_size || names_size - at < sizeof(want)) return false;
    return memcmp(names + at, want, sizeof(want)) == 0;
}

/* The section headers, a few at a time, until the one called .boxos.form. */
static int find_section(file_id_t file, const FormElfHeader *header, const char *names,
                        uint64_t names_size, FormElfSection *found)
{
    for (uint32_t first = 0; first < header->SectionHeaderCount; first += FORM_HEADER_BATCH) {
        FormElfSection batch[FORM_HEADER_BATCH];
        uint32_t take = header->SectionHeaderCount - first;
        if (take > FORM_HEADER_BATCH) take = FORM_HEADER_BATCH;

        int rc = read_exact(file, header->SectionHeaders + (uint64_t)first * sizeof(FormElfSection),
                            batch, take * sizeof(FormElfSection));
        if (rc != 0) return rc;
        for (uint32_t i = 0; i < take; i++) {
            if (!name_is_form(names, names_size, batch[i].Name)) continue;
            *found = batch[i];
            return 1;
        }
    }
    return 0;
}

static int read_names(file_id_t file, const FormElfHeader *header, uint64_t file_size,
                      char **names, uint64_t *names_size)
{
    FormElfSection section;
    int rc = read_exact(file, header->SectionHeaders +
                              (uint64_t)header->NameSection * sizeof(FormElfSection),
                        &section, sizeof(section));
    if (rc != 0) return rc;
    if (section.Type == FORM_SECTION_NOBITS || !span_inside(section.Offset, section.Size, file_size))
        return -ERR_CORRUPTED;

    char *copy = malloc((size_t)section.Size + 1);
    if (!copy) return -ERR_NO_MEMORY;
    rc = section.Size ? read_exact(file, section.Offset, copy, (size_t)section.Size) : 0;
    if (rc != 0) {
        free(copy);
        return rc;
    }
    copy[section.Size] = '\0';
    *names      = copy;
    *names_size = section.Size;
    return 0;
}

static int headers_usable(const FormElfHeader *header, uint64_t file_size)
{
    if (header->SectionHeaders == 0 || header->SectionHeaderCount == 0) return 0;
    if (header->SectionHeaderSize != sizeof(FormElfSection)) return -ERR_CORRUPTED;
    if (header->NameSection >= header->SectionHeaderCount) return -ERR_CORRUPTED;
    uint64_t table = (uint64_t)header->SectionHeaderCount * sizeof(FormElfSection);
    if (!span_inside(header->SectionHeaders, table, file_size)) return -ERR_CORRUPTED;
    return 1;
}

static int read_form_section(file_id_t file, const FormElfSection *section, uint64_t file_size,
                             BoxForm *out)
{
    if (section->Type == FORM_SECTION_NOBITS || section->Size > FORM_TEXT_MAX ||
        !span_inside(section->Offset, section->Size, file_size))
        return -ERR_CORRUPTED;
    /* BoxASM's linker writes the section's header in every image, empty for
     * a program that declares no Form: that is a program without one. */
    if (section->Size == 0) return 0;

    char *text = malloc((size_t)section->Size + 1);
    if (!text) return -ERR_NO_MEMORY;
    int rc = section->Size ? read_exact(file, section->Offset, text, (size_t)section->Size) : 0;
    if (rc == 0) rc = form_parse(text, (size_t)section->Size, out);
    free(text);
    return rc;
}

int form_of_file(file_id_t program, BoxForm *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    FileMetadata metadata;
    int rc = FileMetadataRead(program, &metadata);
    if (rc != 0) return rc;
    uint64_t file_size = metadata.Size;
    FileMetadataRelease(&metadata);

    FormElfHeader header;
    if (file_size < sizeof(header)) return 0;
    rc = read_exact(program, 0, &header, sizeof(header));
    if (rc != 0) return rc;
    if (memcmp(header.Ident, "\x7f" "ELF", 4) != 0 || header.Ident[4] != FORM_ELF_CLASS_64 ||
        header.Ident[5] != FORM_ELF_LITTLE)
        return 0;

    rc = headers_usable(&header, file_size);
    if (rc <= 0) return rc;

    char    *names;
    uint64_t names_size;
    rc = read_names(program, &header, file_size, &names, &names_size);
    if (rc != 0) return rc;

    FormElfSection section = { 0 };
    rc = find_section(program, &header, names, names_size, &section);
    free(names);
    if (rc <= 0) return rc;
    return read_form_section(program, &section, file_size, out);
}
