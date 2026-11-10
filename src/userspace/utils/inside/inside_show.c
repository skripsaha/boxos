/*
 * inside_show.c — a program file said for a person (inside_show.h).
 *
 * Built twice from this one source: into the utility inside, against BoxLib,
 * and into tools/inside on the build machine (INSIDE_HOST), against the
 * host's C library. All either gives it is memory and a few string functions.
 */

#ifdef INSIDE_HOST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#else
#include "box/defs.h"
#include "box/memory.h"
#include "box/string.h"
#endif

#include "inside_show.h"
#include "nameplate_format.h"
#include "settings_reader.h"

/* How much of a part is held at a time while its hash is taken. */
#define INSIDE_CHUNK       (64u * 1024u)
/* The column a part's name is set in; a longer name runs on past it. */
#define INSIDE_NAME_COLUMN 16u
/* The column a part's size is set in, right-aligned. */
#define INSIDE_SIZE_COLUMN 10u

/* ---- saying ------------------------------------------------------------- */

static void Say(const InsideFile *file, const char *text)
{
    file->Say(file->Context, text, strlen(text));
}

static void SaySpan(const InsideFile *file, const uint8_t *bytes, uint64_t length)
{
    file->Say(file->Context, (const char *)bytes, length);
}

static void SayBlanks(const InsideFile *file, uint64_t count)
{
    static const char blanks[] = "                ";
    while (count > 0) {
        uint64_t piece = count < sizeof(blanks) - 1 ? count : sizeof(blanks) - 1;
        file->Say(file->Context, blanks, piece);
        count -= piece;
    }
}

/* `number` in decimal, right-aligned in `width` columns (0: as it is). */
static void SayNumber(const InsideFile *file, uint64_t number, unsigned width)
{
    char     digits[20];
    unsigned at = sizeof(digits);
    do {
        digits[--at] = (char)('0' + number % 10u);
        number /= 10u;
    } while (number != 0);
    if (width > sizeof(digits) - at) SayBlanks(file, width - (sizeof(digits) - at));
    file->Say(file->Context, digits + at, sizeof(digits) - at);
}

/* A whole number with its sign. */
static void SaySigned(const InsideFile *file, int64_t number)
{
    if (number < 0) Say(file, "-");
    SayNumber(file, number < 0 ? 0u - (uint64_t)number : (uint64_t)number, 0);
}

/* A number and what it counts: "1 place", "2 places". */
static void SayCount(const InsideFile *file, uint64_t number, const char *one, const char *many)
{
    SayNumber(file, number, 0);
    Say(file, " ");
    Say(file, number == 1 ? one : many);
}

static void SayHex(const InsideFile *file, uint64_t number)
{
    static const char hex[] = "0123456789abcdef";
    char     digits[18];
    unsigned at = sizeof(digits);
    do {
        digits[--at] = hex[number & 0xFu];
        number >>= 4;
    } while (number != 0);
    digits[--at] = 'x';
    digits[--at] = '0';
    file->Say(file->Context, digits + at, sizeof(digits) - at);
}

static void SayDigest(const InsideFile *file, const uint8_t *digest)
{
    static const char hex[] = "0123456789abcdef";
    char text[2 * BOX_HASH_BYTES];
    for (unsigned i = 0; i < BOX_HASH_BYTES; i++) {
        text[2 * i]     = hex[digest[i] >> 4];
        text[2 * i + 1] = hex[digest[i] & 0xFu];
    }
    file->Say(file->Context, text, sizeof(text));
}

/* The start of a line under a part. */
static void SayUnder(const InsideFile *file)
{
    SayBlanks(file, 6);
}

/* ---- the bytes ---------------------------------------------------------- */

/* Whether the part's bytes hold to its hash, read a chunk at a time into
 * `chunk`. *read false when they could not be read. */
static bool PartHolds(const InsideFile *file, const BoxPart *part, uint8_t *chunk, bool *read)
{
    uint8_t digest[BOX_HASH_BYTES];
    file->Hasher->Begin(file->Hasher->State);
    for (uint64_t at = 0; at < part->Size;) {
        uint64_t piece = part->Size - at < INSIDE_CHUNK ? part->Size - at : INSIDE_CHUNK;
        if (!file->Read(file->Context, part->Offset + at, chunk, piece)) {
            *read = false;
            return false;
        }
        file->Hasher->Add(file->Hasher->State, chunk, piece);
        at += piece;
    }
    file->Hasher->End(file->Hasher->State, digest);
    *read = true;
    return BoxSameHash(digest, part->Hash);
}

/* Whether the file's bytes from `from` to `to` are all zero. */
static bool GapZero(const InsideFile *file, uint64_t from, uint64_t to, uint8_t *chunk, bool *read)
{
    *read = true;
    while (from < to) {
        uint64_t piece = to - from < INSIDE_CHUNK ? to - from : INSIDE_CHUNK;
        if (!file->Read(file->Context, from, chunk, piece)) {
            *read = false;
            return false;
        }
        for (uint64_t i = 0; i < piece; i++)
            if (chunk[i] != 0) return false;
        from += piece;
    }
    return true;
}

/* ---- what a part is and says --------------------------------------------- */

static bool KindIsText(BoxKind kind)
{
    return kind == BOX_KIND_NEEDS || kind == BOX_KIND_HINTS || kind == BOX_KIND_LIFE ||
           kind == BOX_KIND_FORM || kind == BOX_KIND_SETTINGS || kind == BOX_KIND_RECIPE ||
           kind == BOX_KIND_ABOUT || kind == BOX_KIND_WORKS || kind == BOX_KIND_ASKS;
}

static void SayLoadable(const InsideFile *file, const BoxPart *part)
{
    static const char *const rights[] = { "run and read", "read", "read and written" };
    SayUnder(file);
    Say(file, "at ");
    SayHex(file, part->Place);
    Say(file, " from the base, ");
    Say(file, rights[part->Kind - BOX_KIND_CODE]);
    if (part->Memory > part->Size) {
        Say(file, ", and ");
        SayCount(file, part->Memory - part->Size, "byte", "bytes");
        Say(file, " of zeros after it");
    }
    Say(file, "\n");
}

/* Each line of a part written as text, past its first. */
static bool SayText(const InsideFile *file, const BoxPart *part, const uint8_t *bytes)
{
    BoxText        text;
    const uint8_t *line;
    uint64_t       length;
    if (BoxTextBegin(bytes, part->Size, part->Kind, &text) != BOX_OK) return false;
    while (BoxTextLine(&text, &line, &length)) {
        SayUnder(file);
        SaySpan(file, line, length);
        Say(file, "\n");
    }
    return true;
}

/* A setting as a person reads it, indented as deep as it stands in the
 * tree; a thing that only holds others by what it holds. */
static void SaySetting(const InsideFile *file, const Setting *setting)
{
    SettingValue value;
    SayUnder(file);
    for (uint32_t depth = 0; depth < setting->Depth; depth++) Say(file, "  ");
    Say(file, "setting ");
    SaySpan(file, setting->Name.At, setting->Name.Length);
    if (setting->Kind == SETTING_GROUP) {
        Say(file, ": holds what stands under it — ");
        SaySpan(file, setting->What.At, setting->What.Length);
        Say(file, "\n");
        return;
    }
    switch (setting->Kind) {
    case SETTING_ON_OR_OFF: Say(file, ": on or off, "); break;
    case SETTING_NUMBER:
        Say(file, ": a whole number from ");
        SaySigned(file, setting->From);
        Say(file, " to ");
        SaySigned(file, setting->To);
        if (setting->Unit.Length != 0) {
            Say(file, " ");
            SaySpan(file, setting->Unit.At, setting->Unit.Length);
        }
        Say(file, setting->Off ? ", or off, " : ", ");
        break;
    case SETTING_WORD: Say(file, ": a word, "); break;
    case SETTING_TEXT: Say(file, ": text, "); break;
    case SETTING_ONE_OF: {
        SettingSpan     choice;
        uint64_t        at = 0;
        bool            quoted, first = true;
        SettingsVerdict ignored;
        Say(file, ": one of ");
        while (SettingWord(setting->Choices, &at, &choice, &quoted, &ignored)) {
            if (!first) Say(file, ", ");
            SaySpan(file, choice.At, choice.Length);
            first = false;
        }
        Say(file, "; ");
        break;
    }
    case SETTING_COLOR: Say(file, ": a color, "); break;
    default: break;
    }
    if (setting->Kind == SETTING_TEXT) {
        if (setting->Default.Length == 0) {
            Say(file, "empty");
        } else {
            Say(file, "\"");
            SaySpan(file, setting->Default.At, setting->Default.Length);
            Say(file, "\"");
        }
    } else {
        SettingValueRead(setting, setting->Default, &value);
        SaySpan(file, value.Text.At, value.Text.Length);
    }
    Say(file, setting->NextStart ? " by default, from the next start — " : " by default — ");
    SaySpan(file, setting->What.At, setting->What.Length);
    Say(file, "\n");
}

/* Each setting a declaration says, as a person reads it. A setting of a kind
 * this reader does not know is named and passed over, as a program passes
 * it over, and so is all it holds; any other line it cannot read means the
 * part is not written as a declaration is. */
static bool SaySettings(const InsideFile *file, const BoxPart *part, const uint8_t *bytes)
{
    SettingsDeclared declared;
    Setting          setting;
    SettingsVerdict  verdict;
    bool             written = true;
    if (SettingsDeclaredBegin(bytes, part->Size, &declared) != SETTINGS_OK) return false;
    while (SettingsDeclaredNext(&declared, &setting, &verdict)) {
        if (verdict == SETTINGS_OK) {
            SaySetting(file, &setting);
            continue;
        }
        SayUnder(file);
        Say(file, "line ");
        SayNumber(file, setting.Line, 0);
        Say(file, setting.Name.Length != 0 ? ": the setting " : ": the line");
        SaySpan(file, setting.Name.At, setting.Name.Length);
        Say(file, " ");
        Say(file, SettingsVerdictText(verdict));
        Say(file, "\n");
        if (verdict != SETTING_KIND_UNKNOWN && verdict != SETTING_INSIDE_UNTAKEN) written = false;
    }
    return written;
}

/* How the program lives, said as whoever starts it will act on it. */
static bool SayLife(const InsideFile *file, const BoxPart *part, const uint8_t *bytes)
{
    static const char *const ways[] = {
        "a task: whoever starts it waits for its end",
        "it stays: it lives on by itself, and whoever starts it does not wait",
        "it serves others for as long as the machine runs",
    };
    BoxLife  life;
    uint64_t unknown;
    if (BoxLifeRead(bytes, part->Size, &life, &unknown) != BOX_OK) return false;
    SayUnder(file);
    Say(file, ways[life.Kind]);
    Say(file, life.Rise ? "; raised again when it falls\n" : "\n");
    if (unknown != 0) {
        SayUnder(file);
        Say(file, "line ");
        SayNumber(file, unknown, 0);
        Say(file, ": a word this reader does not know, passed over\n");
    }
    return true;
}

static bool SayDoors(const InsideFile *file, const BoxTable *table, const BoxPart *part,
                     const uint8_t *bytes)
{
    BoxDoors       doors;
    const uint8_t *name;
    uint16_t       length;
    uint64_t       offset;
    if (BoxDoorsRead(bytes, part->Size, table, &doors) != BOX_OK) return false;
    for (uint32_t i = 0; BoxDoorAt(&doors, i, &name, &length, &offset); i++) {
        SayUnder(file);
        Say(file, "door ");
        SaySpan(file, name, length);
        Say(file, " at ");
        SayHex(file, offset);
        Say(file, "\n");
    }
    return true;
}

static bool SayThread(const InsideFile *file, const BoxPart *part, const uint8_t *bytes)
{
    BoxThread thread;
    if (BoxThreadRead(bytes, part->Size, &thread) != BOX_OK) return false;
    SayUnder(file);
    Say(file, "each strand keeps ");
    SayCount(file, thread.TotalBytes, "byte", "bytes");
    if (thread.InitBytes == 0) {
        Say(file, " of its own, all zeros at first, aligned to ");
    } else {
        Say(file, " of its own, the first ");
        SayNumber(file, thread.InitBytes, 0);
        Say(file, " of them from this template, aligned to ");
    }
    SayNumber(file, thread.Align, 0);
    Say(file, "\n");
    return true;
}

static bool SayFixups(const InsideFile *file, const BoxTable *table, const BoxPart *part,
                      const uint8_t *bytes)
{
    BoxFixupWalk walk;
    uint64_t     offset, count = 0;
    if (BoxFixupsCheck(bytes, part->Size, table) != BOX_OK) return false;
    BoxFixupBegin(bytes, part->Size, &walk);
    while (BoxFixupNext(&walk, &offset)) count++;
    SayUnder(file);
    SayCount(file, count, "place", "places");
    Say(file, " the base is added to: the kernel places the box where it chooses\n");
    return true;
}

static bool SayNames(const InsideFile *file, const BoxPart *part, const uint8_t *bytes)
{
    const NameplateHeader *header = (const NameplateHeader *)(const void *)bytes;
    if (part->Size < sizeof(NameplateHeader) || !NameplateHeaderValid(header, part->Size)) return false;
    SayUnder(file);
    Say(file, "the names of ");
    SayCount(file, header->EntryCount, "function", "functions");
    Say(file, ", for a fault and a stack trace to say\n");
    return true;
}

/* How many of a part's bytes its description reads. */
static uint64_t DescribedBytes(const BoxPart *part)
{
    if (part->Kind == BOX_KIND_NAMES)
        return part->Size < sizeof(NameplateHeader) ? part->Size : sizeof(NameplateHeader);
    if (part->Kind == BOX_KIND_FIXUPS || part->Kind == BOX_KIND_DOORS || part->Kind == BOX_KIND_THREAD ||
        KindIsText(part->Kind))
        return part->Size;
    return 0;
}

/* The lines under a part that say what it is. False when it is not written as
 * a part of its kind is; *read false when it could not be read. */
static bool Describe(const InsideFile *file, const BoxTable *table, const BoxPart *part, bool *read)
{
    *read = true;
    if (part->Kind == BOX_KIND_UNKNOWN) {
        bool bare;
        SayUnder(file);
        Say(file, BoxKindOf(part->Name, part->NameLength, &bare) != BOX_KIND_UNKNOWN
                      ? "of a later version of its kind than this reader knows\n"
                      : "of a kind this reader does not know\n");
        return true;
    }
    if (BoxKindLoadable(part->Kind)) {
        SayLoadable(file, part);
        return true;
    }
    if (part->Flags & BOX_FLAG_COMPRESSED) {
        SayUnder(file);
        Say(file, "kept compressed: what it says is not read here\n");
        return true;
    }

    uint64_t need = DescribedBytes(part);
    if (need == 0) return true;
    uint8_t *bytes = (uint8_t *)malloc((size_t)need);
    if (!bytes || !file->Read(file->Context, part->Offset, bytes, need)) {
        free(bytes);
        *read = false;
        return false;
    }
    bool written;
    switch (part->Kind) {
    case BOX_KIND_DOORS:  written = SayDoors(file, table, part, bytes); break;
    case BOX_KIND_THREAD: written = SayThread(file, part, bytes); break;
    case BOX_KIND_FIXUPS: written = SayFixups(file, table, part, bytes); break;
    case BOX_KIND_NAMES:  written = SayNames(file, part, bytes); break;
    case BOX_KIND_SETTINGS: written = SaySettings(file, part, bytes); break;
    case BOX_KIND_LIFE:   written = SayLife(file, part, bytes); break;
    default:              written = SayText(file, part, bytes); break;
    }
    free(bytes);
    return written;
}

/* ---- the box ------------------------------------------------------------ */

static void SayFlags(const InsideFile *file, uint32_t flags)
{
    if (flags & BOX_FLAG_MUST_UNDERSTAND) Say(file, ", must be understood");
    if (flags & BOX_FLAG_LOAD_AT_START)   Say(file, ", put into memory at start");
    if (flags & BOX_FLAG_COMPRESSED)      Say(file, ", compressed");
}

/* One part: its line, what it is, and whether it holds. False when something
 * of it does not; *read false when it could not be read. */
static bool ShowPart(const InsideFile *file, const BoxTable *table, const BoxPart *part, uint8_t *chunk,
                     bool *read)
{
    bool holds = PartHolds(file, part, chunk, read);
    if (!*read) return false;

    Say(file, "  ");
    SaySpan(file, part->Name, part->NameLength);
    SayBlanks(file, part->NameLength < INSIDE_NAME_COLUMN ? INSIDE_NAME_COLUMN - part->NameLength : 1);
    SayNumber(file, part->Size, INSIDE_SIZE_COLUMN);
    Say(file, part->Size == 1 ? " byte" : " bytes");
    Say(file, holds ? ", holds to its hash" : ", DAMAGED: its hash differs");
    SayFlags(file, part->Flags);
    Say(file, "\n");

    bool written = Describe(file, table, part, read);
    if (!*read) return false;
    if (!written) {
        SayUnder(file);
        Say(file, "NOT WRITTEN as a part of its kind is\n");
    }
    return holds && written;
}

static InsideVerdict ShowParts(const InsideFile *file, BoxTable *table, bool whole)
{
    uint8_t *chunk = (uint8_t *)malloc(INSIDE_CHUNK);
    if (!chunk) return INSIDE_UNREAD;

    bool     read = true;
    uint64_t from = table->Head.PrefixBytes;
    for (uint32_t i = 0; i < table->Head.PartCount && read; i++) {
        BoxPart part;
        BoxPartAt(table, i, &part);
        if (!GapZero(file, from, part.Offset, chunk, &read)) {
            if (!read) break;
            Say(file, "  NOT ZERO: bytes lie between the parts before ");
            SaySpan(file, part.Name, part.NameLength);
            Say(file, "\n");
            whole = false;
        }
        if (!ShowPart(file, table, &part, chunk, &read)) whole = false;
        from = part.Offset + part.Size;
    }
    free(chunk);
    if (!read) {
        Say(file, "  the file could not be read to its end\n");
        return INSIDE_UNREAD;
    }
    Say(file, whole ? "  whole: the seal holds, every part holds to its hash, and only zeros lie between them\n"
                    : "  NOT WHOLE: what is marked above does not hold\n");
    return whole ? INSIDE_WHOLE : INSIDE_DAMAGED;
}

/* A box this reader cannot read at all, said with why. */
static InsideVerdict Unreadable(const InsideFile *file, BoxVerdict verdict, const BoxTable *table)
{
    Say(file, file->Name);
    Say(file, ": a box this reader cannot read — ");
    Say(file, BoxVerdictText(verdict));
    if (table && table->RefusedName) {
        Say(file, " (part ");
        SaySpan(file, table->RefusedName, table->RefusedNameLength);
        Say(file, ")");
    }
    Say(file, "\n");
    return INSIDE_DAMAGED;
}

static InsideVerdict ShowBox(const InsideFile *file, const uint8_t *first, uint64_t look)
{
    BoxHead    head;
    BoxVerdict verdict = BoxReadHead(first, look, file->Size, &head);
    if (verdict != BOX_OK) return Unreadable(file, verdict, NULL);

    uint8_t *prefix = (uint8_t *)malloc((size_t)head.PrefixBytes);
    if (!prefix || !file->Read(file->Context, 0, prefix, head.PrefixBytes)) {
        free(prefix);
        Say(file, file->Name);
        Say(file, ": the head of the box could not be read\n");
        return INSIDE_UNREAD;
    }
    BoxTable table;
    verdict = BoxRead(prefix, head.PrefixBytes, &head, &table);
    if (verdict != BOX_OK) {
        InsideVerdict said = Unreadable(file, verdict, &table);
        free(prefix);
        return said;
    }

    Say(file, file->Name);
    Say(file, ": a box, format ");
    SayNumber(file, head.Version, 0);
    Say(file, ", ");
    SayCount(file, file->Size, "byte", "bytes");
    Say(file, ", ");
    SayNumber(file, head.PartCount, 0);
    Say(file, head.PartCount == 1 ? " part\n" : " parts\n");

    bool sealed = BoxSealHolds(&table, file->Hasher);
    Say(file, "  seal ");
    SayDigest(file, prefix + BOX_AT_SEAL);
    Say(file, sealed ? " — the table and its names hold to it\n"
                     : " — DAMAGED: the table or its names were changed after it was sealed\n");

    InsideVerdict shown = ShowParts(file, &table, sealed);
    free(prefix);
    return shown;
}

InsideVerdict InsideShow(const InsideFile *file)
{
    static const uint8_t magic[8] = { BOX_MAGIC_0, BOX_MAGIC_1, BOX_MAGIC_2, BOX_MAGIC_3,
                                      BOX_MAGIC_4, BOX_MAGIC_5, BOX_MAGIC_6, BOX_MAGIC_7 };
    uint8_t  first[BOX_HEADER_BYTES];
    uint64_t look = file->Size < sizeof(first) ? file->Size : sizeof(first);
    if (!file->Read(file->Context, 0, first, look)) {
        Say(file, file->Name);
        Say(file, ": could not be read\n");
        return INSIDE_UNREAD;
    }
    if (look >= sizeof(magic) && memcmp(first, magic, sizeof(magic)) == 0) return ShowBox(file, first, look);

    Say(file, file->Name);
    if (look >= 4 && memcmp(first, "\x7f" "ELF", 4) == 0)
        Say(file, ": an ELF image, not a box\n");
    else
        Say(file, ": no program — neither a box nor an ELF image\n");
    return INSIDE_NO_BOX;
}
