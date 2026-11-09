/*
 * pack — a program as the tree links it, an ELF, made a box (src/include/box_format.h).
 *
 *     tools/pack PROGRAM.elf PROGRAM.box
 *
 * The ELF stays on the build machine, for the debugger; the box is what goes
 * on the volume. What goes where:
 *
 *   PT_LOAD R+X        code     at its address, MUST_UNDERSTAND
 *   PT_LOAD R          const    its own sections — the Nameplate, the Form and the settings,
 *                               last in it, are parts of their own and are left out of it
 *   PT_LOAD R+W        data     its zeros past the file's bytes as memory only
 *   PT_LOAD, no rights —          the linker's tables for a dynamic linker
 *   PT_TLS             thread   the template every thread's block is made from
 *   e_entry            doors    the door `start`
 *   .nameplate         names    the program's names for its functions
 *   .boxos.form        form     loaded at start, read-only; the text, its C string's zeros dropped
 *   .boxos.settings    settings loaded at start, read-only; the text, every setting read
 *   .boxos.recipe      recipe   the text, its C string's zeros dropped
 *
 * An executable linked at a fixed address (ET_EXEC) keeps its addresses: its
 * box has no fixups and is loaded where it was linked. A static PIE (ET_DYN)
 * linked at 0 can go anywhere, and its box always has fixups to say so: its
 * R_X86_64_RELATIVE relocations (.rela.dyn) and its RELR (.relr.dyn), none
 * when it has none, each word they name set
 * to the address it means at base 0 — and anything else a dynamic linker
 * would be asked for is refused, because there is none.
 *
 * The box is written by the one pen (box_pen.h), which reads back what it
 * wrote with the one reader: a box pack makes is a box the kernel accepts.
 * Every refusal is one line on stderr naming the file, and exit status 1.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box_pen.h"
#include "box_host.h"
#include "settings_reader.h"

#define ET_EXEC          2u
#define ET_DYN           3u
#define EM_X86_64        62u
#define PT_LOAD          1u
#define PT_DYNAMIC       2u
#define PT_INTERP        3u
#define PT_TLS           7u
#define PF_X             1u
#define PF_W             2u
#define PF_R             4u
#define SHT_RELA         4u
#define SHF_ALLOC        2u
#define SHT_NOBITS       8u
#define SHT_REL          9u
#define SHT_RELR         19u
#define DT_NULL          0u
#define DT_NEEDED        1u
#define R_X86_64_RELATIVE 8u
#define PAGE             4096ull

typedef struct {
    const char    *Path;
    const uint8_t *File;
    uint64_t       Size;
} Elf;

/* Each segment that becomes a part: its address, and its bytes as they lie in
 * memory from the page it starts on — pack's own copy, so the fixups can set
 * the words they name. */
typedef struct {
    bool     Present;
    uint64_t Place;
    uint8_t *Bytes;
    uint64_t Size;
    uint64_t Memory;
} Loadable;

static const char *g_path;

static void Refuse(const char *why)
{
    fprintf(stderr, "pack: %s: %s\n", g_path, why);
    exit(1);
}

static uint16_t U16(const uint8_t *at) { return (uint16_t)(at[0] | at[1] << 8); }
static uint32_t U32(const uint8_t *at) { return BoxU32(at); }
static uint64_t U64(const uint8_t *at) { return BoxU64(at); }

/* `count` bytes from `offset` of the file, or a refusal saying what they were. */
static const uint8_t *Span(const Elf *elf, uint64_t offset, uint64_t count, const char *what)
{
    if (offset > elf->Size || count > elf->Size - offset) {
        char why[160];
        snprintf(why, sizeof(why), "%s runs past the end of the file", what);
        Refuse(why);
    }
    return elf->File + offset;
}

/* ── segments ────────────────────────────────────────────────────────────── */

typedef struct {
    Loadable Code, Const, Data;
    bool     Thread;
    uint64_t ThreadAlign, ThreadMemory, ThreadOffset, ThreadSize, ThreadPlace;
    bool     Dynamic;
    uint64_t DynamicOffset, DynamicSize;
} Segments;

static void TakeLoad(const Elf *elf, const uint8_t *header, Loadable *into, const char *kind)
{
    uint64_t offset = U64(header + 8), virt = U64(header + 16);
    uint64_t filesz = U64(header + 32), memsz = U64(header + 40);
    char     why[160];

    if (into->Present) {
        snprintf(why, sizeof(why), "two %s segments — the linker script makes one of each", kind);
        Refuse(why);
    }
    if (filesz > memsz) Refuse("a segment has more bytes in the file than in memory");
    const uint8_t *bytes = Span(elf, offset, filesz, "a segment's bytes");

    uint64_t lead = virt % PAGE;
    into->Present = true;
    into->Place   = virt - lead;
    into->Memory  = lead + memsz;
    into->Size    = lead + filesz;
    if (into->Memory < memsz) Refuse("a segment's size wraps past 2^64");
    into->Bytes = calloc(1, into->Memory ? into->Memory : 1);
    if (!into->Bytes) Refuse("no memory for a segment");
    memcpy(into->Bytes + lead, bytes, filesz);
}

static void ReadSegments(const Elf *elf, Segments *out)
{
    const uint8_t *head  = elf->File;
    uint64_t       at    = U64(head + 0x20);
    uint16_t       size  = U16(head + 0x36), count = U16(head + 0x38);

    memset(out, 0, sizeof(*out));
    if (size != 56) Refuse("its program headers are not 56 bytes each");
    const uint8_t *table = Span(elf, at, (uint64_t)count * 56, "the program header table");

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *header = table + (uint64_t)i * 56;
        uint32_t       type   = U32(header), flags = U32(header + 4);

        if (type == PT_INTERP) Refuse("it asks for a program interpreter; a box is never linked at start");
        if (type == PT_DYNAMIC) {
            out->Dynamic       = true;
            out->DynamicOffset = U64(header + 8);
            out->DynamicSize   = U64(header + 32);
        }
        if (type == PT_TLS) {
            out->Thread       = true;
            out->ThreadOffset = U64(header + 8);
            out->ThreadPlace  = U64(header + 16);
            out->ThreadSize   = U64(header + 32);
            out->ThreadMemory = U64(header + 40);
            out->ThreadAlign  = U64(header + 48) ? U64(header + 48) : 1;
        }
        if (type != PT_LOAD || U64(header + 40) == 0) continue;
        /* A segment with no rights is no part of the program: the linker's
         * own tables for a dynamic linker (see user.ld), the relocations among
         * them read from the file below. */
        if ((flags & (PF_R | PF_W | PF_X)) == 0) continue;
        if ((flags & PF_X) && (flags & PF_W)) Refuse("a segment is writable and executable at once");
        if (flags & PF_X)      TakeLoad(elf, header, &out->Code, "R+X");
        else if (flags & PF_W) TakeLoad(elf, header, &out->Data, "R+W");
        else                   TakeLoad(elf, header, &out->Const, "R");
    }
    if (!out->Code.Present) Refuse("it has no executable segment");

    /* Code and const are the image as it is: their zeros past the file's
     * bytes, if any, go into the box as bytes. Only data has memory the file
     * does not hold. */
    out->Code.Size  = out->Code.Memory;
    out->Const.Size = out->Const.Memory;
}

/* ── sections ────────────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *Header;
    const char    *Name;
} Section;

static uint16_t SectionCount(const Elf *elf)
{
    if (U64(elf->File + 0x28) == 0) return 0;
    if (U16(elf->File + 0x3A) != 64) Refuse("its section headers are not 64 bytes each");
    return U16(elf->File + 0x3C);
}

static bool SectionAt(const Elf *elf, uint16_t index, Section *out)
{
    uint16_t       count   = SectionCount(elf);
    const uint8_t *table   = Span(elf, U64(elf->File + 0x28), (uint64_t)count * 64, "the section table");
    uint16_t       strings = U16(elf->File + 0x3E);
    if (index >= count) return false;
    if (strings >= count) Refuse("its section names are not among its sections");

    const uint8_t *names_header = table + (uint64_t)strings * 64;
    uint64_t       names_size   = U64(names_header + 32);
    const char    *names = (const char *)Span(elf, U64(names_header + 24), names_size, "the section names");
    uint32_t       name  = U32(table + (uint64_t)index * 64);
    if (name >= names_size || memchr(names + name, '\0', names_size - name) == NULL)
        Refuse("a section's name runs past the section names");
    out->Header = table + (uint64_t)index * 64;
    out->Name   = names + name;
    return true;
}

static bool SectionNamed(const Elf *elf, const char *name, const uint8_t **bytes, uint64_t *size)
{
    Section section;
    for (uint16_t i = 0; SectionAt(elf, i, &section); i++) {
        if (strcmp(section.Name, name) != 0) continue;
        *size = U64(section.Header + 32);
        if (U32(section.Header + 4) == SHT_NOBITS || *size == 0) return false;
        *bytes = Span(elf, U64(section.Header + 24), *size, name);
        return true;
    }
    return false;
}

/* The sections a box carries as parts of their own, not in its constants. */
static bool CarriedApart(const char *name)
{
    return strcmp(name, ".nameplate") == 0 || strcmp(name, ".boxos.form") == 0 ||
           strcmp(name, ".boxos.settings") == 0;
}

/* The constants without the Nameplate and the Form: the linker script puts
 * them last in the read-only segment, and the box carries them as the parts
 * names and form — a copy of each among the constants would be a second one
 * nothing reads. Const ends where its own last section ends. */
static void LeaveOutCarried(const Elf *elf, Loadable *constants)
{
    Section  section;
    uint64_t end = 0, carried = UINT64_MAX;
    if (!constants->Present) return;
    for (uint16_t i = 0; SectionAt(elf, i, &section); i++) {
        uint64_t address = U64(section.Header + 16), size = U64(section.Header + 32);
        if ((U64(section.Header + 8) & SHF_ALLOC) == 0 || U32(section.Header + 4) == SHT_NOBITS || size == 0 ||
            address < constants->Place || address - constants->Place >= constants->Memory)
            continue;
        if (CarriedApart(section.Name)) {
            if (address < carried) carried = address;
        } else if (address + size - constants->Place > end) {
            end = address + size - constants->Place;
        }
    }
    if (carried == UINT64_MAX) return;
    if (carried - constants->Place < end)
        Refuse("its Nameplate, its Form or its settings lie among its constants; the linker script puts them last");
    constants->Size = constants->Memory = end;
    constants->Present = end > 0;
}

/* ── fixups ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t *Places;
    uint64_t  Count;
    uint64_t  Room;
} Places;

static void PlaceAdd(Places *places, uint64_t place)
{
    if (places->Count == places->Room) {
        places->Room   = places->Room ? places->Room * 2 : 256;
        places->Places = realloc(places->Places, places->Room * sizeof(uint64_t));
        if (!places->Places) Refuse("no memory for the fixups");
    }
    places->Places[places->Count++] = place;
}

static int PlaceOrder(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* The loadable whose bytes in the file hold the word at `place`. */
static Loadable *WordHolder(Segments *segments, uint64_t place)
{
    Loadable *all[3] = { &segments->Code, &segments->Const, &segments->Data };
    for (unsigned i = 0; i < 3; i++) {
        Loadable *part = all[i];
        if (part->Present && place >= part->Place && part->Size >= 8 && place - part->Place <= part->Size - 8)
            return part;
    }
    return NULL;
}

static void ReadFixups(const Elf *elf, Segments *segments, uint16_t type, Places *places)
{
    Section section;
    for (uint16_t i = 0; SectionAt(elf, i, &section); i++) {
        uint32_t sh_type = U32(section.Header + 4);
        uint64_t size    = U64(section.Header + 32);
        if (sh_type == SHT_REL && size != 0) Refuse("it carries REL relocations; pack takes RELA and RELR");
        if (sh_type != SHT_RELA && sh_type != SHT_RELR) continue;
        if (size == 0) continue;
        if (type == ET_EXEC) Refuse("it is linked at a fixed address and still carries relocations");
        const uint8_t *bytes = Span(elf, U64(section.Header + 24), size, section.Name);

        if (sh_type == SHT_RELA) {
            if (size % 24 != 0) Refuse("a RELA section is not whole entries");
            for (uint64_t at = 0; at < size; at += 24) {
                uint64_t  place = U64(bytes + at), info = U64(bytes + at + 8), addend = U64(bytes + at + 16);
                if (info != R_X86_64_RELATIVE)
                    Refuse("it carries a relocation other than R_X86_64_RELATIVE; a box is never linked at start");
                Loadable *holder = WordHolder(segments, place);
                if (!holder) Refuse("a relocation names a word outside the image's bytes");
                BoxPut64(holder->Bytes + (place - holder->Place), addend);
                PlaceAdd(places, place);
            }
        } else {
            BoxFixupWalk walk;
            uint64_t     place;
            if (size % 8 != 0) Refuse("a RELR section is not whole words");
            BoxFixupBegin(bytes, size, &walk);
            while (BoxFixupNext(&walk, &place)) {
                if (!WordHolder(segments, place)) Refuse("a relocation names a word outside the image's bytes");
                PlaceAdd(places, place);
            }
        }
    }

    qsort(places->Places, places->Count, sizeof(uint64_t), PlaceOrder);
    for (uint64_t i = 0; i < places->Count; i++) {
        if (i > 0 && places->Places[i] == places->Places[i - 1]) Refuse("two relocations name one word");
        if (segments->Thread && places->Places[i] + 8 > segments->ThreadPlace &&
            places->Places[i] < segments->ThreadPlace + segments->ThreadSize)
            Refuse("a word of its thread template holds an address; every thread's block is copied from the "
                   "template as it is, with no base added");
    }
}

static void CheckDynamic(const Elf *elf, const Segments *segments)
{
    if (!segments->Dynamic) return;
    const uint8_t *dynamic = Span(elf, segments->DynamicOffset, segments->DynamicSize, "the dynamic section");
    for (uint64_t at = 0; at + 16 <= segments->DynamicSize; at += 16) {
        uint64_t tag = U64(dynamic + at);
        if (tag == DT_NULL) break;
        if (tag == DT_NEEDED) Refuse("it needs a shared library; a box is never linked at start");
    }
}

/* ── the box ─────────────────────────────────────────────────────────────── */

/* Every setting the declaration says, read as a program will read it: a line
 * a program would pass over is a setting it would never have, and the build
 * says so instead. */
static void CheckSettings(const uint8_t *bytes, uint64_t size)
{
    SettingsDeclared declared;
    Setting          setting;
    SettingsVerdict  verdict;
    char             why[300];
    if (SettingsDeclaredBegin(bytes, size, &declared) != SETTINGS_OK)
        Refuse("its settings do not begin with the line \"settings 1\"");
    while (SettingsDeclaredNext(&declared, &setting, &verdict)) {
        if (verdict == SETTINGS_OK) continue;
        snprintf(why, sizeof(why), "its settings, line %u: the setting %.*s %s", setting.Line,
                 (int)setting.Name.Length, (const char *)setting.Name.At, SettingsVerdictText(verdict));
        Refuse(why);
    }
}

static void Add(BoxPenPart *parts, uint32_t *count, const char *name, const uint8_t *bytes, uint64_t size,
                uint64_t memory, uint64_t place, uint32_t flags)
{
    parts[(*count)++] = (BoxPenPart){ name, bytes, size, memory, place, flags, BOX_PART_VERSION };
}

static void AddLoadable(BoxPenPart *parts, uint32_t *count, const char *name, const Loadable *part)
{
    if (part->Present)
        Add(parts, count, name, part->Bytes, part->Size, part->Memory, part->Place, BOX_FLAG_MUST_UNDERSTAND);
}

static uint8_t *ReadWhole(const char *path, uint64_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    uint8_t *bytes = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        long length = ftell(file);
        if (length >= 0 && fseek(file, 0, SEEK_SET) == 0) {
            bytes = malloc(length ? (size_t)length : 1);
            if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
                free(bytes);
                bytes = NULL;
            }
            *size = (uint64_t)length;
        }
    }
    fclose(file);
    return bytes;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: pack PROGRAM.elf PROGRAM.box\n");
        return 2;
    }
    g_path = argv[1];
    if (!BoxHostHashTrue()) Refuse("the host's SHA-256 is not FIPS 180-4's");

    Elf elf = { argv[1], NULL, 0 };
    elf.File = ReadWhole(argv[1], &elf.Size);
    if (!elf.File) Refuse("cannot be read");
    if (elf.Size < 64 || memcmp(elf.File, "\x7F" "ELF", 4) != 0) Refuse("it is not an ELF file");
    if (elf.File[4] != 2 || elf.File[5] != 1) Refuse("it is not a 64-bit little-endian ELF");
    if (U16(elf.File + 0x12) != EM_X86_64) Refuse("it is not for x86-64");
    uint16_t type = U16(elf.File + 0x10);
    if (type != ET_EXEC && type != ET_DYN) Refuse("it is not an executable or a static PIE");

    Segments segments;
    Places   places = { 0 };
    ReadSegments(&elf, &segments);
    LeaveOutCarried(&elf, &segments.Const);
    CheckDynamic(&elf, &segments);
    ReadFixups(&elf, &segments, type, &places);

    BoxPenPart parts[16];
    uint32_t   count = 0;
    AddLoadable(parts, &count, "code", &segments.Code);
    AddLoadable(parts, &count, "const", &segments.Const);
    AddLoadable(parts, &count, "data", &segments.Data);

    BoxPenDoor doors[1] = { { BOX_DOOR_START, U64(elf.File + 0x18) } };
    uint64_t   doors_size;
    BoxPenDoors(doors, 1, NULL, &doors_size);
    uint8_t *doors_bytes = malloc(doors_size);
    BoxPenDoors(doors, 1, doors_bytes, &doors_size);
    Add(parts, &count, "doors", doors_bytes, doors_size, doors_size, 0, BOX_FLAG_MUST_UNDERSTAND);

    /* The fixups part is what says the box can go anywhere — a static PIE
     * carries one even when nothing in it needs fixing. */
    uint8_t *fixups_bytes = NULL;
    if (type == ET_DYN) {
        uint64_t fixups_size;
        BoxPenFixups(places.Places, places.Count, NULL, &fixups_size);
        fixups_bytes = malloc(fixups_size ? fixups_size : 1);
        BoxPenFixups(places.Places, places.Count, fixups_bytes, &fixups_size);
        Add(parts, &count, "fixups", fixups_bytes, fixups_size, fixups_size, 0, BOX_FLAG_MUST_UNDERSTAND);
    }

    uint8_t *thread_bytes = NULL;
    if (segments.Thread && segments.ThreadMemory > 0) {
        uint64_t       thread_size;
        const uint8_t *init = Span(&elf, segments.ThreadOffset, segments.ThreadSize, "the thread template");
        BoxPenThread(segments.ThreadAlign, init, segments.ThreadSize, segments.ThreadMemory, NULL, &thread_size);
        thread_bytes = malloc(thread_size);
        BoxPenThread(segments.ThreadAlign, init, segments.ThreadSize, segments.ThreadMemory, thread_bytes,
                     &thread_size);
        Add(parts, &count, "thread", thread_bytes, thread_size, thread_size, 0, BOX_FLAG_MUST_UNDERSTAND);
    }

    /* The Form and the Recipe are parts written as text: the section holds the
     * text as a C string, and the zeros that end it are not the text's. */
    static const struct { const char *Section, *Part; BoxKind Text; uint32_t Flags; } carried[] = {
        { ".nameplate",      "names",    BOX_KIND_UNKNOWN,  0 },
        { ".boxos.form",     "form",     BOX_KIND_FORM,     BOX_FLAG_LOAD_AT_START },
        { ".boxos.settings", "settings", BOX_KIND_SETTINGS, BOX_FLAG_LOAD_AT_START },
        { ".boxos.recipe",   "recipe",   BOX_KIND_RECIPE,   0 },
    };
    for (unsigned i = 0; i < sizeof(carried) / sizeof(carried[0]); i++) {
        const uint8_t *bytes;
        uint64_t       size;
        BoxText        text;
        if (!SectionNamed(&elf, carried[i].Section, &bytes, &size))
            continue;
        if (carried[i].Text != BOX_KIND_UNKNOWN) {
            while (size > 0 && bytes[size - 1] == 0) size--;
            if (size == 0)
                continue;
            if (BoxTextBegin(bytes, size, carried[i].Text, &text) != BOX_OK) {
                char why[160];
                snprintf(why, sizeof(why), "its %s is not text as a box keeps its part %s",
                         carried[i].Section, carried[i].Part);
                Refuse(why);
            }
            if (carried[i].Text == BOX_KIND_SETTINGS) CheckSettings(bytes, size);
        }
        Add(parts, &count, carried[i].Part, bytes, size, size, 0, carried[i].Flags);
    }

    BoxHostHash state  = { 0 };
    BoxHasher   hasher = BoxHostHasher(&state);
    uint64_t    bytes;
    uint32_t    refused = BOX_NO_PART;
    BoxVerdict  verdict = BoxPenMeasure(parts, count, &bytes);
    uint8_t    *box     = verdict == BOX_OK ? malloc(bytes) : NULL;
    if (verdict == BOX_OK) verdict = box ? BoxPenWrite(parts, count, box, bytes, &hasher, &refused) : BOX_NO_PARTS;
    if (verdict != BOX_OK) {
        char why[300];
        if (refused != BOX_NO_PART && refused < count)
            snprintf(why, sizeof(why), "the box would not be read: %s (part %s)", BoxVerdictText(verdict),
                     parts[refused].Name);
        else
            snprintf(why, sizeof(why), "the box would not be read: %s", BoxVerdictText(verdict));
        Refuse(why);
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(box, 1, bytes, out) != bytes || fclose(out) != 0) {
        g_path = argv[2];
        Refuse("cannot be written");
    }

    free(box);
    free(fixups_bytes);
    free(thread_bytes);
    free(doors_bytes);
    free(places.Places);
    free(segments.Code.Bytes);
    free(segments.Const.Bytes);
    free(segments.Data.Bytes);
    free((void *)elf.File);
    BoxHostHashFree(&state);
    return 0;
}
