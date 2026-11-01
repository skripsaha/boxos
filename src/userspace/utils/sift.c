#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"
#include "box/color.h"

#include "heading.h"

BOX_FORM("takes things \"the files to sift\"\n"
         "takes optional words \"the fewest characters a text has\"\n"
         "gives things\n");

#define SIFT_CHUNK    65536u     /* bytes asked of the volume at a time */
#define SIFT_SHORTEST 4u         /* the fewest characters a text has, unless said */

#define INK_OFFSET COLOR_SLATE
#define INK_WIDE   COLOR_AMBER

/*
 * Text is found in two spellings at once:
 *
 *   narrow  UTF-8, which ASCII is part of: printable ASCII, the tab, and every
 *           well-formed sequence of a character that is seen when written;
 *   wide    UTF-16 little-endian, as UEFI and Windows write it: code units
 *           0x0020-0x007e, the tab and 0x00a0-0x00ff, at even offsets and at
 *           odd ones.
 *
 * Wide text is held to the first 256 characters: a unit of any other page is
 * two bytes of anything, and UTF-8 or a table of numbers read two bytes at a
 * time would be found as text. Held so, a wide text has a zero every other
 * byte and a narrow text none, so no two texts of two characters or more are
 * ever open at once, and each is said as it ends, in the order they began.
 */

typedef struct {
    uint64_t Start;      /* the offset of its first byte */
    uint64_t Length;     /* in characters */
    char    *Text;       /* UTF-8, NUL-ended */
    size_t   Used;
    size_t   Room;
    bool     Open;
} SiftRun;

typedef struct {
    uint64_t Shortest;
    unsigned Digits;
    uint64_t Found;
    bool     OutOfMemory;

    SiftRun  Narrow;
    uint8_t  Sequence[4];  /* a UTF-8 sequence under way */
    unsigned SequenceUsed;
    unsigned SequenceNeed;
    uint8_t  NextLow;      /* the bounds of its next byte */
    uint8_t  NextHigh;
    uint32_t Point;
    uint64_t SequenceAt;

    SiftRun  Wide[2];      /* units at even offsets, at odd ones */
    uint8_t  Previous;
} Sifter;

/* A count as a person writes one, in decimal. */
static bool sift_count(const char *word, uint64_t *out)
{
    if (*word == '\0') return false;

    uint64_t value = 0;
    for (; *word; word++) {
        if (*word < '0' || *word > '9') return false;
        unsigned digit = (unsigned)(*word - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

static bool sift_shortest(const char *const *words, size_t count, uint64_t *shortest)
{
    *shortest = SIFT_SHORTEST;
    if (count == 0) return true;

    if (count > 1) {
        println("sift takes one number, the fewest characters a text has: sift hello.elf 8");
        return false;
    }
    if (!sift_count(words[0], shortest)) {
        printf("'%s' is no file here, nor a number of characters: sift hello.elf 8\n", words[0]);
        return false;
    }
    if (*shortest < 2) {
        println("a text is 2 characters or more: one character is any printable byte");
        return false;
    }
    return true;
}

/* A code point past ASCII that is seen when written: not a C1 control, not a
 * noncharacter, and none of the invisible marks that steer text without being
 * any — zero widths, direction marks, the line and paragraph separators,
 * the byte order mark. */
static bool sift_seen(uint32_t point)
{
    if (point < 0xa0) return false;
    if (point >= 0xfdd0 && point <= 0xfdef) return false;
    if ((point & 0xfffe) == 0xfffe) return false;
    if (point >= 0x200b && point <= 0x200f) return false;
    if (point >= 0x2028 && point <= 0x202e) return false;
    if (point >= 0x2060 && point <= 0x206f) return false;
    return point != 0xfeff;
}

/* A character onto a text, which begins with it when none is open. */
static void sift_put(Sifter *sifter, SiftRun *run, uint64_t at, const uint8_t *bytes, size_t count)
{
    if (!run->Open) {
        run->Open   = true;
        run->Start  = at;
        run->Length = 0;
        run->Used   = 0;
    }
    if (run->Used + count + 1 > run->Room) {
        size_t room = run->Room ? run->Room : 64;
        while (run->Used + count + 1 > room) room *= 2;
        char *grown = realloc(run->Text, room);
        if (!grown) {
            sifter->OutOfMemory = true;
            return;
        }
        run->Text = grown;
        run->Room = room;
    }
    memcpy(run->Text + run->Used, bytes, count);
    run->Used += count;
    run->Length++;
}

/* A text ends; said when it is long enough. */
static void sift_end(Sifter *sifter, SiftRun *run)
{
    if (!run->Open) return;
    run->Open = false;
    if (run->Length < sifter->Shortest) return;

    run->Text[run->Used] = '\0';
    printf("%color%0*llx%color  %color%s%color%s\n", INK_OFFSET, (int)sifter->Digits,
           (unsigned long long)run->Start, COLOR_DEFAULT, INK_WIDE,
           run == &sifter->Narrow ? "     " : "u16  ", COLOR_DEFAULT, run->Text);
    sifter->Found++;
}

/* Begin a UTF-8 sequence of Need bytes whose second lies in Low..High. */
static void sift_begin(Sifter *sifter, uint8_t lead, uint64_t at, unsigned need,
                       uint8_t low, uint8_t high)
{
    sifter->Sequence[0]  = lead;
    sifter->SequenceUsed = 1;
    sifter->SequenceNeed = need;
    sifter->SequenceAt   = at;
    sifter->NextLow      = low;
    sifter->NextHigh     = high;
    sifter->Point        = lead & (0x7fu >> need);
}

static void sift_narrow(Sifter *sifter, uint8_t byte, uint64_t at)
{
    if (sifter->SequenceNeed != 0) {
        if (byte >= sifter->NextLow && byte <= sifter->NextHigh) {
            sifter->Sequence[sifter->SequenceUsed++] = byte;
            sifter->Point    = (sifter->Point << 6) | (byte & 0x3fu);
            sifter->NextLow  = 0x80;
            sifter->NextHigh = 0xbf;
            if (sifter->SequenceUsed < sifter->SequenceNeed) return;

            sifter->SequenceNeed = 0;
            if (sift_seen(sifter->Point))
                sift_put(sifter, &sifter->Narrow, sifter->SequenceAt, sifter->Sequence, sifter->SequenceUsed);
            else
                sift_end(sifter, &sifter->Narrow);
            return;
        }
        /* The sequence is broken; the byte that broke it may begin a text. */
        sifter->SequenceNeed = 0;
        sift_end(sifter, &sifter->Narrow);
    }

    if (byte == '\t' || (byte >= 0x20 && byte < 0x7f))      sift_put(sifter, &sifter->Narrow, at, &byte, 1);
    else if (byte >= 0xc2 && byte <= 0xdf)                  sift_begin(sifter, byte, at, 2, 0x80, 0xbf);
    else if (byte == 0xe0)                                  sift_begin(sifter, byte, at, 3, 0xa0, 0xbf);
    else if (byte == 0xed)                                  sift_begin(sifter, byte, at, 3, 0x80, 0x9f);
    else if (byte >= 0xe1 && byte <= 0xef)                  sift_begin(sifter, byte, at, 3, 0x80, 0xbf);
    else if (byte == 0xf0)                                  sift_begin(sifter, byte, at, 4, 0x90, 0xbf);
    else if (byte >= 0xf1 && byte <= 0xf3)                  sift_begin(sifter, byte, at, 4, 0x80, 0xbf);
    else if (byte == 0xf4)                                  sift_begin(sifter, byte, at, 4, 0x80, 0x8f);
    else                                                    sift_end(sifter, &sifter->Narrow);
}

/* The UTF-16 unit Low, High that begins at `at`. */
static void sift_wide(Sifter *sifter, uint8_t low, uint8_t high, uint64_t at)
{
    SiftRun *run = &sifter->Wide[at & 1];

    if (high != 0 || !(low == '\t' || (low >= 0x20 && low < 0x7f) || low >= 0xa0)) {
        sift_end(sifter, run);
        return;
    }
    uint8_t utf8[2] = { low, 0 };
    size_t  count   = 1;
    if (low >= 0x80) {
        utf8[0] = (uint8_t)(0xc0 | (low >> 6));
        utf8[1] = (uint8_t)(0x80 | (low & 0x3f));
        count   = 2;
    }
    sift_put(sifter, run, at, utf8, count);
}

static void sift_take(Sifter *sifter, uint64_t at, const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count && !sifter->OutOfMemory; i++) {
        uint64_t position = at + i;
        if (position > 0) sift_wide(sifter, sifter->Previous, bytes[i], position - 1);
        sift_narrow(sifter, bytes[i], position);
        sifter->Previous = bytes[i];
    }
}

/* One file under its heading. False when it could not be sifted whole. */
static bool sift_file(file_id_t id, uint64_t shortest, uint8_t *chunk)
{
    FileMetadata metadata;
    if (FileMetadataRead(id, &metadata) != 0) {
        println("Could not look at a file; it may have been erased");
        return false;
    }
    print_heading(&metadata);

    Sifter sifter;
    memset(&sifter, 0, sizeof(sifter));
    sifter.Shortest = shortest;
    sifter.Digits   = offset_digits(metadata.Size);

    uint64_t at  = 0;
    int64_t  got = 1;
    while (at < metadata.Size && !sifter.OutOfMemory) {
        uint64_t want = metadata.Size - at > SIFT_CHUNK ? SIFT_CHUNK : metadata.Size - at;
        got = fread(id, at, chunk, (size_t)want);
        if (got <= 0) break;
        sift_take(&sifter, at, chunk, (size_t)got);
        at += (uint64_t)got;
    }
    if (!sifter.OutOfMemory) {
        sift_end(&sifter, &sifter.Narrow);
        sift_end(&sifter, &sifter.Wide[0]);
        sift_end(&sifter, &sifter.Wide[1]);
    }

    bool sifted = true;
    if (sifter.OutOfMemory) {
        printf("no memory for a text at %llu that long; the rest of the file is not sifted\n",
               (unsigned long long)at);
        sifted = false;
    } else if (got < 0) {
        printf("Could not read the file at %llu\n", (unsigned long long)at);
        sifted = false;
    } else if (at < metadata.Size) {
        printf("the file was cut short while it was read: it ends at %llu\n", (unsigned long long)at);
    }
    if (sifted && sifter.Found == 0)
        printf("no text of %llu characters or more\n", (unsigned long long)shortest);

    free(sifter.Narrow.Text);
    free(sifter.Wide[0].Text);
    free(sifter.Wide[1].Text);
    FileMetadataRelease(&metadata);
    return sifted;
}

int main(void)
{
    const file_id_t   *files;
    size_t             file_count;
    const char *const *words;
    size_t             word_count;
    uint64_t           shortest;

    if (form_things(0, &files, &file_count) != 0 || form_words(1, &words, &word_count) != 0 ||
        !sift_shortest(words, word_count, &shortest)) {
        exit(1);
        return 1;
    }

    uint8_t   *chunk  = malloc(SIFT_CHUNK);
    file_id_t *sifted = malloc(file_count * sizeof(file_id_t));
    if (!chunk || !sifted) {
        println("sift: no memory to read the files into");
        exit(1);
        return 1;
    }

    size_t sifted_count = 0;
    for (size_t i = 0; i < file_count; i++) {
        if (i > 0) println("");
        if (sift_file(files[i], shortest, chunk)) sifted[sifted_count++] = files[i];
    }
    souvenir_things(sifted, sifted_count);

    int code = sifted_count == file_count ? 0 : 1;
    free(chunk);
    free(sifted);
    exit(code);
    return code;
}
