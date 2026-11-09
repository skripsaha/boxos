/*
 * recipe.c — a program's Recipe: its text read and written, and found in the
 * program.
 *
 * The text is whatever a program carries as its part `recipe`, so it is untrusted
 * bytes: a line of a kind the reader knows is read exactly or the whole
 * Recipe is refused — a rebuild follows it, and a misread file or context
 * would make something else than the program was.
 */

#include "box/recipe.h"
#include "box/program_part.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define RECIPE_HEX_DIGITS   (2u * SHA256_BYTES)

void recipe_release(BoxRecipe *recipe)
{
    if (!recipe) return;
    for (size_t i = 0; i < recipe->FileCount; i++) free(recipe->Files[i].Name);
    free(recipe->Files);
    free(recipe->By);
    free(recipe->Context);
    memset(recipe, 0, sizeof(*recipe));
}

/* ---- reading ----------------------------------------------------------- */

static char *copy_text(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* The line's first word is Word, alone or followed by a blank; *Rest is what
 * follows that blank. */
static bool line_kind(const char *line, size_t length, const char *word, const char **rest,
                      size_t *rest_length)
{
    size_t n = strlen(word);
    if (length < n || memcmp(line, word, n) != 0) return false;
    if (length == n) {
        *rest = line + n;
        *rest_length = 0;
        return true;
    }
    if (line[n] != ' ') return false;
    *rest = line + n + 1;
    *rest_length = length - n - 1;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* A canonical decimal id: digits only, no leading zero, not zero, 64 bits. */
static bool read_id(const char *digits, size_t length, file_id_t *id)
{
    if (length == 0 || length > 20 || digits[0] == '0') return false;

    file_id_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (digits[i] < '0' || digits[i] > '9') return false;
        file_id_t digit = (file_id_t)(digits[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *id = value;
    return true;
}

/* `#ID HEX NAME` */
static int read_from(BoxRecipe *out, const char *text, size_t length)
{
    const char *blank = length > 1 ? memchr(text, ' ', length) : NULL;
    BoxRecipeFile file;

    if (!blank || text[0] != '#' || !read_id(text + 1, (size_t)(blank - text) - 1, &file.Id))
        return -ERR_INVALID_ARGUMENT;
    const char *hex  = blank + 1;
    size_t      left = length - (size_t)(hex - text);
    if (left < RECIPE_HEX_DIGITS + 2 || hex[RECIPE_HEX_DIGITS] != ' ') return -ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < SHA256_BYTES; i++) {
        int high = hex_value(hex[2 * i]), low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) return -ERR_INVALID_ARGUMENT;
        file.Digest[i] = (uint8_t)(high << 4 | low);
    }

    BoxRecipeFile *bigger = realloc(out->Files, (out->FileCount + 1) * sizeof(BoxRecipeFile));
    if (!bigger) return -ERR_NO_MEMORY;
    out->Files = bigger;
    file.Name = copy_text(hex + RECIPE_HEX_DIGITS + 1, left - RECIPE_HEX_DIGITS - 1);
    if (!file.Name) return -ERR_NO_MEMORY;
    out->Files[out->FileCount++] = file;
    return 0;
}

static int read_once(char **field, const char *text, size_t length, bool may_be_empty)
{
    if (*field || (length == 0 && !may_be_empty)) return -ERR_INVALID_ARGUMENT;
    *field = copy_text(text, length);
    return *field ? 0 : -ERR_NO_MEMORY;
}

static int read_line(BoxRecipe *out, const char *line, size_t length, bool first)
{
    const char *rest;
    size_t      rest_length;

    if (first) return length == 8 && memcmp(line, "recipe 1", 8) == 0 ? 0 : -ERR_INVALID_ARGUMENT;
    if (memchr(line, '\0', length)) return -ERR_INVALID_ARGUMENT;
    if (line_kind(line, length, "recipe", &rest, &rest_length)) return -ERR_INVALID_ARGUMENT;
    if (line_kind(line, length, "by", &rest, &rest_length))
        return read_once(&out->By, rest, rest_length, false);
    if (line_kind(line, length, "context", &rest, &rest_length))
        return read_once(&out->Context, rest, rest_length, true);
    if (line_kind(line, length, "from", &rest, &rest_length)) return read_from(out, rest, rest_length);
    return 0;
}

int recipe_parse(const char *text, size_t length, BoxRecipe *out)
{
    if (!out || (!text && length != 0)) return -ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    const char *at  = text;
    const char *end = text + length;
    int         rc  = 0;
    bool        first = true;

    while (rc == 0 && at < end) {
        const char *line_end = memchr(at, '\n', (size_t)(end - at));
        if (!line_end) line_end = end;
        rc = read_line(out, at, (size_t)(line_end - at), first);
        first = false;
        at = line_end + 1;
    }
    if (rc == 0 && (first || !out->By || !out->Context || out->FileCount == 0))
        rc = -ERR_INVALID_ARGUMENT;
    if (rc != 0) {
        recipe_release(out);
        return rc;
    }
    out->Declared = 1;
    return 0;
}

int recipe_of_file(file_id_t program, BoxRecipe *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    char       *text;
    size_t      size;
    ProgramKind kind;
    int         rc = program_part_read(program, "recipe", SIZE_MAX, &text, &size, &kind);
    if (rc < 0) return rc;
    if (rc == 0 || !text) {
        out->Image = kind != PROGRAM_NONE;
        return 0;
    }
    rc = recipe_parse(text, size, out);
    free(text);
    out->Image = 1;
    return rc;
}

/* ---- writing ----------------------------------------------------------- */

typedef struct {
    char  *Bytes;
    size_t Used;
    size_t Capacity;
} RecipeText;

static int put(RecipeText *out, const char *text, size_t length)
{
    if (out->Capacity - out->Used < length) {
        size_t grown = out->Capacity ? out->Capacity * 2 : 256;
        while (grown - out->Used < length) grown *= 2;
        char *bigger = realloc(out->Bytes, grown);
        if (!bigger) return -ERR_NO_MEMORY;
        out->Bytes    = bigger;
        out->Capacity = grown;
    }
    memcpy(out->Bytes + out->Used, text, length);
    out->Used += length;
    return 0;
}

static int put_text(RecipeText *out, const char *text)
{
    return put(out, text, strlen(text));
}

static int put_from(RecipeText *out, const BoxRecipeFile *file)
{
    static const char digits[] = "0123456789abcdef";
    char line[1 + 20 + 1 + RECIPE_HEX_DIGITS + 1];
    char reversed[20];
    size_t at = 0, count = 0;
    file_id_t id = file->Id;

    line[at++] = '#';
    do {
        reversed[count++] = (char)('0' + id % 10);
        id /= 10;
    } while (id != 0);
    while (count > 0) line[at++] = reversed[--count];
    line[at++] = ' ';
    for (size_t i = 0; i < SHA256_BYTES; i++) {
        line[at++] = digits[file->Digest[i] >> 4];
        line[at++] = digits[file->Digest[i] & 15];
    }
    line[at++] = ' ';

    int rc = put_text(out, "from ");
    if (rc == 0) rc = put(out, line, at);
    for (const char *c = file->Name; rc == 0 && *c; c++) {
        char shown = (unsigned char)*c < 0x20 || *c == 0x7F ? '?' : *c;
        rc = put(out, &shown, 1);
    }
    return rc == 0 ? put(out, "\n", 1) : rc;
}

static bool one_line(const char *text)
{
    return !strchr(text, '\n') && !strchr(text, '\r');
}

int recipe_text(const BoxRecipe *recipe, char **text, size_t *length)
{
    if (!recipe || !text || !length || !recipe->By || !recipe->By[0] || !one_line(recipe->By) ||
        (recipe->Context && !one_line(recipe->Context)) || recipe->FileCount == 0)
        return -ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < recipe->FileCount; i++) {
        if (recipe->Files[i].Id == 0 || !recipe->Files[i].Name || !recipe->Files[i].Name[0])
            return -ERR_INVALID_ARGUMENT;
    }

    RecipeText out = { NULL, 0, 0 };
    const char *context = recipe->Context ? recipe->Context : "";
    int rc = put_text(&out, "recipe 1\nby ");
    if (rc == 0) rc = put_text(&out, recipe->By);
    if (rc == 0) rc = put_text(&out, context[0] ? "\ncontext " : "\ncontext");
    if (rc == 0) rc = put_text(&out, context);
    if (rc == 0) rc = put(&out, "\n", 1);
    for (size_t i = 0; rc == 0 && i < recipe->FileCount; i++) rc = put_from(&out, &recipe->Files[i]);
    if (rc != 0) {
        free(out.Bytes);
        return rc;
    }
    *text   = out.Bytes;
    *length = out.Used;
    return 0;
}
