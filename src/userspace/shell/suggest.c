/*
 * suggest.c — the names close to a command that was not found.
 *
 * Closeness is the edit distance with a swap of two neighbours counted as one
 * step: at most 2, and at most 1 for a word of three letters or fewer, which
 * is near to everything otherwise. Only the closest names are kept, three at
 * most. A program's name is its file's name up to the last dot, as the volume
 * stamps it.
 */

#include "suggest.h"
#include "program.h"
#include "shell.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"

#define SUGGEST_NAME_MAX  48
#define SUGGEST_SHOWN     3
#define SUGGEST_SHORT     3

extern const ShellCommand g_commands[];

typedef struct {
    char   Names[SUGGEST_SHOWN][SUGGEST_NAME_MAX];
    size_t Count;
    size_t Distance;
    size_t Limit;
} Closest;

static size_t Smallest(size_t a, size_t b, size_t c)
{
    size_t low = a < b ? a : b;
    return low < c ? low : c;
}

/* Edit distance of two names with a swap of neighbours as one step; both are
 * shorter than SUGGEST_NAME_MAX. */
static size_t Distance(const char *a, const char *b)
{
    size_t length_a = strlen(a);
    size_t length_b = strlen(b);
    size_t before[SUGGEST_NAME_MAX + 1], last[SUGGEST_NAME_MAX + 1], row[SUGGEST_NAME_MAX + 1];

    for (size_t j = 0; j <= length_b; j++) last[j] = j;
    for (size_t i = 1; i <= length_a; i++) {
        row[0] = i;
        for (size_t j = 1; j <= length_b; j++) {
            size_t same = a[i - 1] == b[j - 1] ? 0 : 1;
            row[j] = Smallest(last[j] + 1, row[j - 1] + 1, last[j - 1] + same);
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && before[j - 2] + 1 < row[j])
                row[j] = before[j - 2] + 1;
        }
        memcpy(before, last, (length_b + 1) * sizeof(size_t));
        memcpy(last, row, (length_b + 1) * sizeof(size_t));
    }
    return last[length_b];
}

static void Consider(Closest *closest, const char *typed, const char *name, size_t length)
{
    if (length == 0 || length >= SUGGEST_NAME_MAX) return;

    char stem[SUGGEST_NAME_MAX];
    memcpy(stem, name, length);
    stem[length] = '\0';

    size_t distance = Distance(typed, stem);
    if (distance == 0 || distance > closest->Limit || distance > closest->Distance) return;
    if (distance < closest->Distance) {
        closest->Distance = distance;
        closest->Count = 0;
    }
    for (size_t i = 0; i < closest->Count; i++) {
        if (strcmp(closest->Names[i], stem) == 0) return;
    }
    if (closest->Count < SUGGEST_SHOWN) memcpy(closest->Names[closest->Count++], stem, length + 1);
}

static size_t StemLength(const char *filename)
{
    size_t length = strlen(filename);
    for (size_t i = length; i > 0; i--) {
        if (filename[i - 1] == '.') return i - 1;
    }
    return length;
}

static void ConsiderProgramsWearing(Closest *closest, const char *typed, const char *tag)
{
    file_id_t *ids = NULL;
    int        count = query_all_everywhere(tag, &ids);
    if (count <= 0) return;

    for (int i = 0; i < count; i++) {
        char *name = ProgramRunnableName(ids[i]);
        if (!name) continue;
        Consider(closest, typed, name, StemLength(name));
        free(name);
    }
    free(ids);
}

static size_t Append(char *out, size_t at, size_t size, const char *text)
{
    size_t length = strlen(text);
    if (at + length >= size) length = size - at - 1;
    memcpy(out + at, text, length);
    return at + length;
}

static void Say(const Closest *closest, char *out, size_t size)
{
    size_t at = Append(out, 0, size, "did you mean ");
    for (size_t i = 0; i < closest->Count; i++) {
        if (i > 0) at = Append(out, at, size, i + 1 == closest->Count ? " or " : ", ");
        at = Append(out, at, size, closest->Names[i]);
    }
    at = Append(out, at, size, "?");
    out[at] = '\0';
}

bool SuggestFor(const char *typed, char *out, size_t size)
{
    size_t length = strlen(typed);
    if (length == 0 || length >= SUGGEST_NAME_MAX || size == 0) return false;

    Closest closest;
    closest.Count    = 0;
    closest.Distance = (size_t)-1;
    closest.Limit    = length <= SUGGEST_SHORT ? 1 : 2;

    for (int i = 0; g_commands[i].name != NULL; i++)
        Consider(&closest, typed, g_commands[i].name, strlen(g_commands[i].name));
    ConsiderProgramsWearing(&closest, typed, "app");
    ConsiderProgramsWearing(&closest, typed, "utility");

    if (closest.Count == 0) return false;
    Say(&closest, out, size);
    return true;
}
