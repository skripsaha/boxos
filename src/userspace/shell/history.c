/*
 * history.c — the lines typed at the prompt, kept on the volume.
 *
 * The file `history` holds a line for each command, as it was typed: the Use
 * Context it was typed in, a tab, the command, a newline. The editor types no
 * tab, so the first one ends the context. The file is read whole when the
 * shell starts, and each line typed afterwards is added to its end in one
 * write the volume lays after whatever another shell added meanwhile
 * (file_append), so two shells never write over each other's lines. A file the
 * person erased is made again with the next line.
 */

#include "history.h"
#include "box/file.h"
#include "box/print.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/error.h"

#define HISTORY_NAME "history"
#define HISTORY_TAGS "shell,whisper"

typedef struct {
    char *Line;
    char *Context;
} Entry;

static struct {
    Entry    *Entries;
    size_t    Count;
    size_t    Room;
    file_id_t File;     /* 0 until it is found or made */
    bool      Told;     /* the volume would not keep a line, and the person was told */
} g_history;

/* What the editor types: one cell a byte, no tab, no line end. */
static bool Typed(const char *text, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] >= 0x7F) return false;
    }
    return true;
}

static char *Copy(const char *text, size_t n)
{
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    copy[n] = '\0';
    return copy;
}

static bool Same(const char *a, const char *b, size_t b_len)
{
    return strlen(a) == b_len && memcmp(a, b, b_len) == 0;
}

/* The newest line already says this. */
static bool IsNewest(const char *line, size_t line_len, const char *context, size_t context_len)
{
    if (g_history.Count == 0) return false;
    const Entry *newest = &g_history.Entries[g_history.Count - 1];
    return Same(newest->Line, line, line_len) && Same(newest->Context, context, context_len);
}

static void Remember(const char *line, size_t line_len, const char *context, size_t context_len)
{
    if (line_len == 0 || IsNewest(line, line_len, context, context_len)) return;
    if (g_history.Count == g_history.Room) {
        size_t room = g_history.Room ? g_history.Room * 2 : 64;
        Entry *more = realloc(g_history.Entries, room * sizeof(Entry));
        if (!more) return;
        g_history.Entries = more;
        g_history.Room    = room;
    }
    char *kept_line    = Copy(line, line_len);
    char *kept_context = Copy(context, context_len);
    if (!kept_line || !kept_context) {
        free(kept_line);
        free(kept_context);
        return;
    }
    g_history.Entries[g_history.Count].Line    = kept_line;
    g_history.Entries[g_history.Count].Context = kept_context;
    g_history.Count++;
}

/* The file, on the whole volume: the one called history that wears both tags
 * and is not in the trash. 0 when there is none. */
static file_id_t Find(void)
{
    file_id_t *ids   = NULL;
    int        count = query_all_everywhere(HISTORY_TAGS, &ids);
    file_id_t  found = 0;
    for (int i = 0; i < count && found == 0; i++) {
        FileMetadata metadata;
        if (FileMetadataRead(ids[i], &metadata) != 0) continue;
        if (metadata.Filename && strcmp(metadata.Filename, HISTORY_NAME) == 0 &&
            !(metadata.Flags & FILE_FLAG_TRASHED))
            found = ids[i];
        FileMetadataRelease(&metadata);
    }
    free(ids);
    return found;
}

/* Each line of the file's text remembered: "context<tab>command". */
static void RememberText(const char *text, size_t length)
{
    size_t at = 0;
    while (at < length) {
        const char *start = text + at;
        const char *end   = memchr(start, '\n', length - at);
        size_t      n     = end ? (size_t)(end - start) : length - at;
        at += n + 1;

        const char *tab = memchr(start, '\t', n);
        const char *line = tab ? tab + 1 : start;
        size_t      line_len = (size_t)(start + n - line);
        size_t      context_len = tab ? (size_t)(tab - start) : 0;
        if (Typed(line, line_len) && Typed(start, context_len))
            Remember(line, line_len, start, context_len);
    }
}

void HistoryLoad(void)
{
    g_history.File = Find();
    if (g_history.File == 0) return;

    FileMetadata metadata;
    if (FileMetadataRead(g_history.File, &metadata) != 0) return;
    uint64_t size = metadata.Size;
    FileMetadataRelease(&metadata);
    if (size == 0) return;

    char *text = malloc((size_t)size);
    if (!text) return;
    size_t read = 0;
    while (read < size) {
        int64_t got = fread(g_history.File, read, text + read, (size_t)size - read);
        if (got <= 0) break;
        read += (size_t)got;
    }
    RememberText(text, read);
    free(text);
}

/* The file found, or made when there is none — outside the Use Context, so it
 * wears only its own tags. False when the volume has none to give. */
static bool Reach(void)
{
    if (g_history.File != 0) return true;
    g_history.File = Find();
    if (g_history.File == 0) {
        int64_t made = create_everywhere(HISTORY_NAME, HISTORY_TAGS);
        if (made > 0) g_history.File = (file_id_t)made;
    }
    return g_history.File != 0;
}

/* One record added to the end of the file. The file known may have been
 * erased since: then it is found or made again, and the record goes there. */
static void Write(const char *record, size_t length)
{
    if (!Reach()) return;
    int64_t wrote = file_append(g_history.File, record, length);
    if (wrote < 0) {
        g_history.File = 0;
        if (!Reach()) return;
        wrote = file_append(g_history.File, record, length);
    }
    if (wrote == (int64_t)length || g_history.Told) return;

    g_history.Told = true;
    if (wrote < 0)
        printf("history: the volume would not keep the line (error %d); "
               "this shell remembers it until it ends\n", (int)box_errno_of((int)wrote));
    else
        println("history: the volume kept only part of the line; "
                "this shell remembers it whole until it ends");
}

void HistoryKeep(const char *line, const char *context)
{
    if (!context) context = "";
    size_t line_len    = strlen(line);
    size_t context_len = strlen(context);
    if (line_len == 0 || !Typed(line, line_len) || !Typed(context, context_len)) return;
    if (IsNewest(line, line_len, context, context_len)) return;
    Remember(line, line_len, context, context_len);

    size_t length = context_len + 1 + line_len + 1;
    char  *record = malloc(length);
    if (!record) return;
    memcpy(record, context, context_len);
    record[context_len] = '\t';
    memcpy(record + context_len + 1, line, line_len);
    record[length - 1] = '\n';
    Write(record, length);
    free(record);
}

const char *HistoryLine(size_t back, const char **context)
{
    if (back >= g_history.Count) return NULL;
    const Entry *entry = &g_history.Entries[g_history.Count - 1 - back];
    if (context) *context = entry->Context;
    return entry->Line;
}
