#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"
#include "box/color.h"
#include "box/vga.h"

#include "heading.h"

BOX_FORM("takes things \"the files to look at\"\n"
         "takes optional words \"from where and how many bytes\"\n"
         "gives things\n");

#define LOUPE_WIDTH 16u          /* bytes a line shows */
#define LOUPE_CHUNK 65536u       /* bytes asked of the volume at a time */

/* What a byte is, said by its colour, in both columns. */
#define INK_OFFSET  COLOR_SLATE
#define INK_ZERO    COLOR_DARK_GRAY
#define INK_TEXT    COLOR_LEAF
#define INK_BLANK   COLOR_TEAL
#define INK_CONTROL COLOR_AMBER
#define INK_HIGH    COLOR_VIOLET

/* The part of each file to show: from From, Count bytes. */
typedef struct {
    uint64_t From;
    uint64_t Count;
    bool     FromSaid;
    bool     CountSaid;
} LoupeRange;

/* A line on its way out: its text, and where each colour begins in it. The
 * longest is an offset of 16 digits, 16 cells, 16 characters and the gaps:
 * 87 bytes in 33 runs. */
#define LOUPE_LINE_MAX 96u
#define LOUPE_RUNS_MAX (2 * LOUPE_WIDTH + 1)

typedef struct {
    char   Text[LOUPE_LINE_MAX];
    size_t Used;
    size_t RunStart[LOUPE_RUNS_MAX];
    Color  RunInk[LOUPE_RUNS_MAX];
    size_t Runs;
} LoupeLine;

/* One file being shown: the line being gathered, and the last whole line
 * shown, which the lines equal to it are counted against. */
typedef struct {
    unsigned Digits;
    uint8_t  Row[LOUPE_WIDTH];
    uint64_t RowAt;
    unsigned RowFirst;
    unsigned RowLast;
    bool     RowOpen;
    uint8_t  Shown[LOUPE_WIDTH];
    uint64_t ShownAt;
    bool     ShownWhole;
    uint64_t Repeats;
} LoupeView;

static const char Hex[] = "0123456789abcdef";

/* A number as a person writes one: decimal, or hex after 0x. */
static bool loupe_number(const char *word, uint64_t *out)
{
    unsigned base = 10;
    if (word[0] == '0' && (word[1] == 'x' || word[1] == 'X')) {
        base = 16;
        word += 2;
    }
    if (*word == '\0') return false;

    uint64_t value = 0;
    for (; *word; word++) {
        unsigned digit;
        if (*word >= '0' && *word <= '9')                     digit = (unsigned)(*word - '0');
        else if (base == 16 && *word >= 'a' && *word <= 'f') digit = (unsigned)(*word - 'a' + 10);
        else if (base == 16 && *word >= 'A' && *word <= 'F') digit = (unsigned)(*word - 'A' + 10);
        else return false;
        if (value > (UINT64_MAX - digit) / base) return false;
        value = value * base + digit;
    }
    *out = value;
    return true;
}

/* The words after the files: `from N` and `for N`, each once, in any order. */
static bool loupe_range(const char *const *words, size_t count, LoupeRange *range)
{
    range->From      = 0;
    range->Count     = UINT64_MAX;
    range->FromSaid  = false;
    range->CountSaid = false;

    for (size_t i = 0; i < count; i += 2) {
        const char *word  = words[i];
        bool        from  = strcmp(word, "from") == 0;
        bool        count_word = strcmp(word, "for") == 0;
        uint64_t    value = 0;

        if (!from && !count_word) {
            printf("'%s' is no file here, nor from or for: loupe hello.elf from 0x40 for 64\n", word);
            return false;
        }
        if (from ? range->FromSaid : range->CountSaid) {
            printf("%s is said twice\n", word);
            return false;
        }
        if (i + 1 >= count || !loupe_number(words[i + 1], &value)) {
            printf("%s takes a number after it, in decimal or after 0x in hex: %s 0x40\n", word, word);
            return false;
        }
        if (from) {
            range->From     = value;
            range->FromSaid = true;
        } else {
            if (value == 0) {
                println("for 0 shows nothing: for takes 1 byte or more");
                return false;
            }
            range->Count     = value;
            range->CountSaid = true;
        }
    }
    return true;
}

static Color loupe_ink(uint8_t byte)
{
    if (byte == 0) return INK_ZERO;
    if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') return INK_BLANK;
    if (byte < 0x20 || byte == 0x7f) return INK_CONTROL;
    if (byte >= 0x80) return INK_HIGH;
    return INK_TEXT;
}

/* Text in a colour of its own. */
static void loupe_paint(LoupeLine *line, Color ink, const char *text, size_t length)
{
    if (line->Runs == 0 || line->RunInk[line->Runs - 1] != ink) {
        line->RunStart[line->Runs] = line->Used;
        line->RunInk[line->Runs]   = ink;
        line->Runs++;
    }
    memcpy(line->Text + line->Used, text, length);
    line->Used += length;
}

/* Spaces, which take whatever colour stands before them. */
static void loupe_gap(LoupeLine *line, size_t length)
{
    memset(line->Text + line->Used, ' ', length);
    line->Used += length;
}

/* The line in one batch: on the glass with no display one submit, on a lane
 * one frame a colour. */
static void loupe_emit(const LoupeLine *line)
{
    Color kept = get_color();

    vga_begin();
    for (size_t r = 0; r < line->Runs; r++) {
        size_t end = r + 1 < line->Runs ? line->RunStart[r + 1] : line->Used;
        set_color(line->RunInk[r]);
        print_bytes(line->Text + line->RunStart[r], end - line->RunStart[r]);
    }
    set_color(kept);
    vga_commit();
}

/* A line at `at`: the offset, the bytes First..Last of it in hex, and the
 * same bytes as text. A byte outside the range leaves its place empty, so a
 * column is always the same low digit of the offset. */
static void loupe_show(const LoupeView *view, uint64_t at, const uint8_t *row,
                       unsigned first, unsigned last)
{
    LoupeLine line;
    char      offset[16];

    line.Used = 0;
    line.Runs = 0;

    for (unsigned d = 0; d < view->Digits; d++)
        offset[d] = Hex[(at >> (4 * (view->Digits - 1 - d))) & 0xf];
    loupe_paint(&line, INK_OFFSET, offset, view->Digits);
    loupe_gap(&line, 2);

    for (unsigned i = 0; i < LOUPE_WIDTH; i++) {
        if (i == LOUPE_WIDTH / 2) loupe_gap(&line, 1);
        if (i >= first && i < last) {
            char cell[2] = { Hex[row[i] >> 4], Hex[row[i] & 0xf] };
            loupe_paint(&line, loupe_ink(row[i]), cell, 2);
        } else {
            loupe_gap(&line, 2);
        }
        loupe_gap(&line, 1);
    }
    loupe_gap(&line, 1 + first);

    for (unsigned i = first; i < last; i++) {
        char shown = row[i] >= 0x20 && row[i] < 0x7f ? (char)row[i] : '.';
        loupe_paint(&line, loupe_ink(row[i]), &shown, 1);
    }
    line.Text[line.Used++] = '\n';

    loupe_emit(&line);
}

/* The whole lines equal to the last one shown, said once a different line
 * comes or the range ends: one is shown as it is, more are counted. */
static void loupe_repeats(LoupeView *view)
{
    if (view->Repeats == 1) {
        loupe_show(view, view->ShownAt + LOUPE_WIDTH, view->Shown, 0, LOUPE_WIDTH);
    } else if (view->Repeats > 1) {
        printf("%color%*s  ... %llu more lines the same%color\n", INK_ZERO,
               (int)view->Digits, "", (unsigned long long)view->Repeats, COLOR_DEFAULT);
    }
    view->Repeats = 0;
}

static void loupe_row(LoupeView *view)
{
    bool whole = view->RowFirst == 0 && view->RowLast == LOUPE_WIDTH;

    view->RowOpen = false;
    if (whole && view->ShownWhole && memcmp(view->Row, view->Shown, LOUPE_WIDTH) == 0) {
        view->Repeats++;
        return;
    }

    loupe_repeats(view);
    loupe_show(view, view->RowAt, view->Row, view->RowFirst, view->RowLast);
    view->ShownWhole = whole;
    if (whole) {
        memcpy(view->Shown, view->Row, LOUPE_WIDTH);
        view->ShownAt = view->RowAt;
    }
}

/* Bytes read at `at`, into the lines they belong to. */
static void loupe_take(LoupeView *view, uint64_t at, const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint64_t position = at + i;
        unsigned column   = (unsigned)(position % LOUPE_WIDTH);

        if (!view->RowOpen) {
            view->RowOpen  = true;
            view->RowAt    = position - column;
            view->RowFirst = column;
        }
        view->Row[column] = bytes[i];
        view->RowLast     = column + 1;
        if (view->RowLast == LOUPE_WIDTH) loupe_row(view);
    }
}

/* One file under its heading. False when it could not be shown whole. */
static bool loupe_file(file_id_t id, const LoupeRange *range, uint8_t *chunk)
{
    FileMetadata metadata;
    if (FileMetadataRead(id, &metadata) != 0) {
        println("Could not look at a file; it may have been erased");
        return false;
    }
    print_heading(&metadata);

    uint64_t size = metadata.Size;
    bool     shown = true;

    if (size == 0) {
        printf("%colorthe file is empty%color\n", INK_ZERO, COLOR_DEFAULT);
    } else if (range->From >= size) {
        printf("from %llu is past its end: its last byte is %llu (0x%llx)\n",
               (unsigned long long)range->From, (unsigned long long)(size - 1),
               (unsigned long long)(size - 1));
        shown = false;
    } else {
        uint64_t  end = size - range->From > range->Count ? range->From + range->Count : size;
        uint64_t  at  = range->From;
        LoupeView view;

        memset(&view, 0, sizeof(view));
        view.Digits = offset_digits(end);

        int64_t got = 1;
        while (at < end) {
            uint64_t want = end - at > LOUPE_CHUNK ? LOUPE_CHUNK : end - at;
            got = fread(id, at, chunk, (size_t)want);
            if (got <= 0) break;
            loupe_take(&view, at, chunk, (size_t)got);
            at += (uint64_t)got;
        }
        if (view.RowOpen) loupe_row(&view);
        loupe_repeats(&view);

        if (got < 0) {
            printf("Could not read the file at %llu\n", (unsigned long long)at);
            shown = false;
        } else if (at < end) {
            printf("the file was cut short while it was read: it ends at %llu\n", (unsigned long long)at);
        }
    }

    FileMetadataRelease(&metadata);
    return shown;
}

int main(void)
{
    const file_id_t   *files;
    size_t             file_count;
    const char *const *words;
    size_t             word_count;
    LoupeRange         range;

    if (form_things(0, &files, &file_count) != 0 || form_words(1, &words, &word_count) != 0 ||
        !loupe_range(words, word_count, &range)) {
        exit(1);
        return 1;
    }

    uint8_t   *chunk = malloc(LOUPE_CHUNK);
    file_id_t *shown = malloc(file_count * sizeof(file_id_t));
    if (!chunk || !shown) {
        println("loupe: no memory to read the files into");
        exit(1);
        return 1;
    }

    size_t shown_count = 0;
    for (size_t i = 0; i < file_count; i++) {
        if (i > 0) println("");
        if (loupe_file(files[i], &range, chunk)) shown[shown_count++] = files[i];
    }
    souvenir_things(shown, shown_count);

    int code = shown_count == file_count ? 0 : 1;
    free(chunk);
    free(shown);
    exit(code);
    return code;
}
