/*
 * ask.c — the questions the shell puts to the person while it fills a Form.
 *
 * One look for all of them: what is being asked, the choices numbered, a
 * short prompt. An empty answer, or no one to answer, ends the question and
 * the caller starts nothing.
 */

#include "ask.h"
#include "box/print.h"
#include "box/string.h"
#include "box/memory.h"
#include "box/file.h"

static bool IsBlank(char c)
{
    return c == ' ' || c == '\t';
}

static char Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool SameWord(const char *typed, const char *word)
{
    while (*typed && *word) {
        if (Lower(*typed++) != *word++) return false;
    }
    return *typed == '\0' && *word == '\0';
}

AskResult AskLine(const char *prompt, char **text)
{
    *text = NULL;
    print(prompt);
    io_flush();

    char *line = NULL;
    int   length = readline_grow(&line);
    if (length < 0) return ASK_NO_ONE;

    char *start = line;
    while (IsBlank(*start)) start++;
    size_t end = strlen(start);
    while (end > 0 && IsBlank(start[end - 1])) end--;
    if (end == 0) {
        free(line);
        return ASK_EMPTY;
    }
    start[end] = '\0';
    memmove(line, start, end + 1);
    *text = line;
    return ASK_ANSWERED;
}

/* The tags a file wears, in brackets, one comma and a blank between. */
static void PrintTags(const char *tags)
{
    print("  (");
    while (*tags) {
        const char *comma = strchr(tags, ',');
        size_t      length = comma ? (size_t)(comma - tags) : strlen(tags);
        print_bytes(tags, length);
        if (!comma) break;
        print(", ");
        tags = comma + 1;
    }
    print(")");
}

static void PrintChoice(size_t number, file_id_t id)
{
    FileMetadata metadata;
    if (FileMetadataRead(id, &metadata) != 0) {
        printf("  %zu  file #%llu", number, (unsigned long long)id);
    } else {
        printf("  %zu  %s", number, metadata.Filename ? metadata.Filename : "");
        if (metadata.Tags) PrintTags(metadata.Tags);
        FileMetadataRelease(&metadata);
    }
    println("");
}

/* The number typed, 1 to `count`, or 0 when the answer is not one. */
static size_t ChoiceNumber(const char *answer, size_t count)
{
    size_t number = 0;
    for (const char *at = answer; *at; at++) {
        if (*at < '0' || *at > '9') return 0;
        number = number * 10 + (size_t)(*at - '0');
        if (number > count) return 0;
    }
    return number;
}

AskResult AskWhich(const char *word, const file_id_t *ids, size_t count, file_id_t *chosen)
{
    printf("%s could be any of these:\n", word);
    for (size_t i = 0; i < count; i++)
        PrintChoice(i + 1, ids[i]);

    for (;;) {
        char     *answer;
        AskResult result = AskLine("which one? (a number) ", &answer);
        if (result != ASK_ANSWERED) return result;

        size_t number = ChoiceNumber(answer, count);
        free(answer);
        if (number != 0) {
            *chosen = ids[number - 1];
            return ASK_ANSWERED;
        }
        printf("that is not one of the numbers above, 1 to %zu\n", count);
    }
}

AskResult AskSure(size_t count, bool *yes)
{
    printf("all is the whole volume here: %zu %s. ", count, count == 1 ? "file" : "files");
    for (;;) {
        char     *answer;
        AskResult result = AskLine("sure? (yes or no) ", &answer);
        if (result != ASK_ANSWERED) return result;

        bool said_yes = SameWord(answer, "yes") || SameWord(answer, "y");
        bool said_no  = SameWord(answer, "no") || SameWord(answer, "n");
        free(answer);
        if (said_yes || said_no) {
            *yes = said_yes;
            return ASK_ANSWERED;
        }
        print("please answer yes or no. ");
    }
}

void AskSayCancelled(const char *who, AskResult result)
{
    if (result == ASK_NO_ONE)
        printf("%s: there is no one to ask, so nothing was started\n", who);
    else
        printf("%s: no answer given, so nothing was started\n", who);
}
