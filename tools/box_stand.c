/* box_stand — the one reader and the one pen of a box (src/include/box_reader.h,
 * box_pen.h) on the host, held to exact outcomes.
 *
 *     tools/box_stand.sh
 *
 * A sample box of every kind of part the standard has, and one it does not, is
 * written with the pen and read back: every part found where the table says,
 * by halves and by kind, its hash and the seal holding, the doors, the thread
 * template and the fixups read as written; the same parts given in another
 * order make the same bytes. Then the box is broken one way at a time — in its
 * header, its table, its names, its parts, its alignment, its doors, thread
 * and fixups — or written from parts the standard does not allow, and each
 * time the reader is held to its exact verdict and to the part it names. Every
 * file is handed over in a buffer of exactly its length, the start of it in a
 * buffer of exactly the start, so a read one byte past what a reader may read
 * is caught by the address sanitizer rather than passing.
 *
 * SHA-256 is BoxLib's (sha256.c, built beside this file), and is held to the
 * FIPS 180-4 vector before anything else is believed. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box_pen.h"
#include "box_needs.h"
#include "box_host.h"

static int g_passed;
static int g_failed;

static void Check(bool ok, const char *label)
{
    if (ok) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n", label);
}

static BoxHostHash g_hash;
static BoxHasher   g_hasher;

/* ── the sample box ──────────────────────────────────────────────────────── */

#define SAMPLE_ROOM 24u

typedef struct {
    BoxPenPart Parts[SAMPLE_ROOM];
    uint32_t   Count;
    uint8_t    Code[0x300];
    uint8_t    Const[0x40];
    uint8_t    Data[0x20];
    uint8_t    Doors[128];
    uint8_t    Fixups[64];
    uint8_t    Thread[64];
    uint8_t    Names[16];
    uint8_t    Icon[16];
} Sample;

static const uint64_t g_fixups[] = { 0x1000, 0x2000, 0x2008, 0x2010, 0x2018 };

static void Add(Sample *sample, const char *name, const void *bytes, uint64_t size, uint64_t memory,
                uint64_t place, uint32_t flags)
{
    sample->Parts[sample->Count++] = (BoxPenPart){ name, bytes, size, memory, place, flags,
                                                   BOX_PART_VERSION };
}

static BoxPenPart *PartNamed(Sample *sample, const char *name)
{
    for (uint32_t i = 0; i < sample->Count; i++)
        if (strcmp(sample->Parts[i].Name, name) == 0) return &sample->Parts[i];
    return NULL;
}

static void Remove(Sample *sample, const char *name)
{
    BoxPenPart *part = PartNamed(sample, name);
    uint32_t    at   = (uint32_t)(part - sample->Parts);
    memmove(part, part + 1, (sample->Count - at - 1) * sizeof(*part));
    sample->Count--;
}

/* Every kind of part: code at the base, const and data on the pages after it
 * — the data with a tail of zeros — two doors, fixups in const and data, a
 * thread template, the descriptions, and a part of the program's own. Listed
 * out of order on purpose: the pen puts them in it. */
static void SampleMake(Sample *sample)
{
    memset(sample, 0, sizeof(*sample));
    for (unsigned i = 0; i < sizeof(sample->Code); i++) sample->Code[i] = (uint8_t)(0x90 + i % 7);
    for (unsigned i = 0; i < sizeof(sample->Const); i++) sample->Const[i] = (uint8_t)(i * 3 + 1);
    for (unsigned i = 0; i < sizeof(sample->Data); i++) sample->Data[i] = (uint8_t)(0xD0 + i);
    memcpy(sample->Names, "nameplate bytes", 16);
    memset(sample->Icon, 0x7F, sizeof(sample->Icon));

    BoxPenDoor doors[2] = { { "start", 0x10 }, { "service", 0x2F0 } };
    uint64_t   doors_bytes, fixups_bytes, thread_bytes;
    BoxPenDoors(doors, 2, NULL, &doors_bytes);
    BoxPenDoors(doors, 2, sample->Doors, &doors_bytes);
    BoxPenFixups(g_fixups, 5, sample->Fixups, &fixups_bytes);
    BoxPenThread(16, (const uint8_t *)"tls-init", 8, 64, sample->Thread, &thread_bytes);

    const uint32_t must = BOX_FLAG_MUST_UNDERSTAND;
    Add(sample, "thread", sample->Thread, thread_bytes, thread_bytes, 0, must);
    Add(sample, "paint:brushes", "brushes", 7, 7, 0, 0);
    Add(sample, "data", sample->Data, 0x20, 0x3000, 0x2000, must);
    Add(sample, "about:ru", "about 1\nname Edit\n", 18, 18, 0, 0);
    Add(sample, "code", sample->Code, 0x300, 0x300, 0, must);
    Add(sample, "icon:64", sample->Icon, 16, 16, 0, 0);
    Add(sample, "form", "form 1\n", 7, 7, 0, BOX_FLAG_LOAD_AT_START);
    Add(sample, "doors", sample->Doors, doors_bytes, doors_bytes, 0, must);
    Add(sample, "fixups", sample->Fixups, fixups_bytes, fixups_bytes, 0, must);
    Add(sample, "about", "about 1\nname Edit\n", 18, 18, 0, 0);
    Add(sample, "const", sample->Const, 0x40, 0x40, 0x1000, must);
    Add(sample, "needs", "needs 1\n", 8, 8, 0, must);
    Add(sample, "hints", "hints 1\nstack 1 MiB\n", 20, 20, 0, 0);
    Add(sample, "life", "life 1\ntask\n", 12, 12, 0, 0);
    Add(sample, "names", sample->Names, 16, 16, 0, 0);
}

/* The box of a sample's parts, in a buffer of exactly its length; *verdict
 * and *refused what the pen said of it. */
static uint8_t *Write(Sample *sample, uint64_t *bytes, BoxVerdict *verdict, const char **refused)
{
    uint8_t   *out;
    uint32_t   index = BOX_NO_PART;
    BoxPenPart parts[SAMPLE_ROOM];

    memcpy(parts, sample->Parts, sizeof(parts));
    *refused = "";
    *verdict = BoxPenMeasure(parts, sample->Count, bytes);
    if (*verdict != BOX_OK) return NULL;
    out = malloc(*bytes);
    if (!out) {
        printf("box: harness failed — no memory for a box of %llu bytes\n", (unsigned long long)*bytes);
        exit(2);
    }
    *verdict = BoxPenWrite(parts, sample->Count, out, *bytes, &g_hasher, &index);
    if (index != BOX_NO_PART) *refused = parts[index].Name;
    return out;
}

/* ── reading ─────────────────────────────────────────────────────────────── */

/* The file, from a buffer of exactly its length, read as a loader reads it:
 * the header from its first bytes, then the table and names from a buffer of
 * exactly that start. *refused names the part a refusal is about. */
static BoxVerdict Read(const uint8_t *file, uint64_t bytes, char *refused, size_t room)
{
    uint8_t   *exact = malloc(bytes ? bytes : 1);
    BoxHead    head;
    BoxTable   box;
    BoxVerdict verdict;

    refused[0] = '\0';
    memcpy(exact, file, bytes);
    verdict = BoxReadHead(exact, bytes < BOX_HEADER_BYTES ? bytes : BOX_HEADER_BYTES, bytes, &head);
    if (verdict == BOX_OK) {
        uint8_t *prefix = malloc(head.PrefixBytes);
        memcpy(prefix, exact, head.PrefixBytes);
        verdict = BoxRead(prefix, head.PrefixBytes, &head, &box);
        if (verdict == BOX_OK) {
            box.Prefix = exact;
            verdict    = BoxGapsZero(exact, &box);
        }
        if (verdict != BOX_OK && box.RefusedName)
            snprintf(refused, room, "%.*s", box.RefusedNameLength, (const char *)box.RefusedName);
        free(prefix);
    }
    free(exact);
    return verdict;
}

static void Expect(const char *label, const uint8_t *file, uint64_t bytes, BoxVerdict want,
                   const char *want_part)
{
    char       part[300];
    BoxVerdict got = Read(file, bytes, part, sizeof(part));
    if (got == want && strcmp(part, want_part) == 0) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n  got:  %s (part '%s')\n  want: %s (part '%s')\n", label, BoxVerdictText(got),
           part, BoxVerdictText(want), want_part);
}

static void ExpectWritten(const char *label, Sample *sample, BoxVerdict want, const char *want_part)
{
    uint64_t    bytes;
    BoxVerdict  got;
    const char *part;
    uint8_t    *box = Write(sample, &bytes, &got, &part);
    free(box);
    if (got == want && strcmp(part, want_part) == 0) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n  got:  %s (part '%s')\n  want: %s (part '%s')\n", label, BoxVerdictText(got),
           part, BoxVerdictText(want), want_part);
}

/* ── the sample, broken in its bytes ─────────────────────────────────────── */

static uint8_t *g_box;
static uint64_t g_bytes;
static uint8_t *g_copy;

static uint8_t *Fresh(void)
{
    memcpy(g_copy, g_box, g_bytes);
    return g_copy;
}

static BoxTable SampleTable(void)
{
    static BoxHead head;
    BoxTable       box;
    BoxReadHead(g_box, g_bytes, g_bytes, &head);
    BoxRead(g_box, g_bytes, &head, &box);
    return box;
}

static uint8_t *EntryIn(uint8_t *file, const char *name)
{
    BoxTable box = SampleTable();
    BoxPart  part;
    if (!BoxPartFind(&box, (const uint8_t *)name, strlen(name), &part)) {
        printf("box: harness failed — the sample has no part %s\n", name);
        exit(2);
    }
    return file + box.Head.TableAt + (uint64_t)part.Index * BOX_ENTRY_BYTES;
}

static uint64_t OffsetOf(const char *name)
{
    return BoxU64(EntryIn(g_box, name) + BOX_ENTRY_OFFSET);
}

static void TestHeader(void)
{
    uint8_t *f;
    Expect("an empty file", g_box, 0, BOX_NOT_BOX, "");
    Expect("seven bytes", g_box, 7, BOX_NOT_BOX, "");
    f = Fresh(); f[3] = 'x';
    Expect("the magic", f, g_bytes, BOX_NOT_BOX, "");
    f = Fresh(); f[4] = 0x0A;
    Expect("a CR a text transfer took", f, g_bytes, BOX_NOT_BOX, "");
    Expect("a header cut short", g_box, 60, BOX_TRUNCATED, "");
    f = Fresh(); BoxPut16(f + BOX_AT_VERSION, 2);
    Expect("version 2", f, g_bytes, BOX_VERSION_UNKNOWN, "");
    f = Fresh(); BoxPut16(f + BOX_AT_HEADER_SIZE, 72);
    Expect("a header shorter than the header", f, g_bytes, BOX_HEADER_SIZE, "");
    f = Fresh(); BoxPut16(f + BOX_AT_HEADER_SIZE, 84);
    Expect("a header of a length not a multiple of eight", f, g_bytes, BOX_HEADER_SIZE, "");
    f = Fresh(); BoxPut16(f + BOX_AT_HEADER_SIZE, 88);
    Expect("a header longer than where the table starts", f, g_bytes, BOX_TABLE_MISPLACED, "");
    Expect("the file cut by one byte", g_box, g_bytes - 1, BOX_FILE_SIZE, "");
    f = Fresh(); BoxPut64(f + BOX_AT_FILE_SIZE, g_bytes + 1);
    Expect("a length the file has not", f, g_bytes, BOX_FILE_SIZE, "");
    f = Fresh(); BoxPut32(f + BOX_AT_PART_COUNT, 0);
    Expect("no parts", f, g_bytes, BOX_NO_PARTS, "");
    f = Fresh(); BoxPut32(f + BOX_AT_PART_COUNT, 0xFFFFFFFFu);
    Expect("more parts than the file has room for", f, g_bytes, BOX_TRUNCATED, "");
    f = Fresh(); BoxPut64(f + BOX_AT_TABLE, 0xFFFFFFFFFFFFFFF0ull);
    Expect("a table at the end of 2^64", f, g_bytes, BOX_TABLE_MISPLACED, "");
    f = Fresh(); BoxPut64(f + BOX_AT_NAMES, BoxU64(f + BOX_AT_NAMES) + 8);
    Expect("names that do not follow the table", f, g_bytes, BOX_NAMES_MISPLACED, "");
    f = Fresh(); BoxPut64(f + BOX_AT_NAMES_SIZE, 0xFFFFFFFFFFFFFFFFull);
    Expect("names whose end wraps past 2^64", f, g_bytes, BOX_TRUNCATED, "");
}

static void TestTable(void)
{
    uint8_t *f;
    f = Fresh(); BoxPut32(EntryIn(f, "data") + BOX_ENTRY_RESERVED, 1);
    Expect("an entry not zero where it must be", f, g_bytes, BOX_ENTRY_NOT_ZERO, "data");
    f = Fresh(); BoxPut32(EntryIn(f, "const") + BOX_ENTRY_FLAGS, BOX_FLAG_MUST_UNDERSTAND | 0x8u);
    Expect("a mark nobody knows", f, g_bytes, BOX_FLAGS_UNKNOWN, "const");

    f = Fresh();
    uint8_t *code = EntryIn(f, "code");
    BoxPut32(code + BOX_ENTRY_NAME_AT, BoxU32(code + BOX_ENTRY_NAME_AT) + 1);
    Expect("a name not where the names go on", f, g_bytes, BOX_NAME_MISPLACED, "");
    f = Fresh(); BoxPut16(EntryIn(f, "about") + BOX_ENTRY_NAME_LEN, 0);
    Expect("a name of no bytes", f, g_bytes, BOX_NAME_MISPLACED, "");
    f = Fresh(); BoxPut16(EntryIn(f, "thread") + BOX_ENTRY_NAME_LEN, 0xFFFF);
    Expect("a name past the end of the names", f, g_bytes, BOX_NAME_MISPLACED, "");

    BoxTable box = SampleTable();
    BoxPart  part;
    BoxPartFind(&box, (const uint8_t *)"life", 4, &part);
    uint64_t life = (uint64_t)(part.Name - g_box);
    f = Fresh(); f[life + 1] = ' ';
    Expect("a blank in a name", f, g_bytes, BOX_NAME_SPELLING, "l fe");
    f = Fresh(); f[life + 1] = ',';
    Expect("a comma in a name", f, g_bytes, BOX_NAME_SPELLING, "l,fe");
    f = Fresh(); f[life] = 'z';
    Expect("a name out of order", f, g_bytes, BOX_NAME_ORDER, "names");
    f = Fresh(); f[life + 3] = 0x01;
    Expect("a control byte in a name", f, g_bytes, BOX_NAME_SPELLING, "lif\x01");

    f = Fresh(); BoxPut64(f + BOX_AT_NAMES_SIZE, BoxU64(f + BOX_AT_NAMES_SIZE) + 2);
    Expect("names longer than the names of the parts", f, g_bytes, BOX_NAMES_LEFT_OVER, "");
    f = Fresh(); BoxPut64(EntryIn(f, "life") + BOX_ENTRY_OFFSET, OffsetOf("life") + 8);
    Expect("a part past where it goes", f, g_bytes, BOX_PART_MISPLACED, "life");
    f = Fresh(); BoxPut64(EntryIn(f, "code") + BOX_ENTRY_OFFSET, OffsetOf("code") - BOX_LOADABLE_ALIGN);
    Expect("code before its page", f, g_bytes, BOX_PART_MISPLACED, "code");
    f = Fresh(); BoxPut64(EntryIn(f, "data") + BOX_ENTRY_SIZE, 0xFFFFFFFFFFFFFF00ull);
    Expect("a part whose end wraps past 2^64", f, g_bytes, BOX_PART_OUTSIDE, "data");
    f = Fresh();
    uint8_t *thread = EntryIn(f, "thread");
    BoxPut64(thread + BOX_ENTRY_SIZE, BoxU64(thread + BOX_ENTRY_SIZE) - 1);
    BoxPut64(thread + BOX_ENTRY_MEMORY, BoxU64(thread + BOX_ENTRY_MEMORY) - 1);
    Expect("a file going on past its last part", f, g_bytes, BOX_FILE_LEFT_OVER, "");
}

static void TestPartRules(void)
{
    uint8_t *f;
    f = Fresh(); BoxPut64(EntryIn(f, "data") + BOX_ENTRY_MEMORY, 0x10);
    Expect("data smaller in memory than in the file", f, g_bytes, BOX_PART_MEMORY, "data");
    f = Fresh(); BoxPut64(EntryIn(f, "const") + BOX_ENTRY_MEMORY, 0x2000);
    Expect("const larger in memory than in the file", f, g_bytes, BOX_PART_MEMORY, "const");
    f = Fresh(); BoxPut64(EntryIn(f, "form") + BOX_ENTRY_MEMORY, 64);
    Expect("a description larger in memory", f, g_bytes, BOX_PART_MEMORY, "form");
    f = Fresh(); BoxPut64(EntryIn(f, "code") + BOX_ENTRY_PLACE, 0x800);
    Expect("code off its page", f, g_bytes, BOX_PART_PLACE, "code");
    f = Fresh(); BoxPut64(EntryIn(f, "data") + BOX_ENTRY_PLACE, BOX_PLACE_LIMIT - 0x1000);
    Expect("data past the user half", f, g_bytes, BOX_PART_PLACE, "data");
    f = Fresh(); BoxPut64(EntryIn(f, "life") + BOX_ENTRY_PLACE, 0x1000);
    Expect("a place for a part that is not loaded", f, g_bytes, BOX_PART_PLACE, "life");
    f = Fresh(); BoxPut64(EntryIn(f, "const") + BOX_ENTRY_PLACE, 0x0);
    Expect("const on the page of the code", f, g_bytes, BOX_LOADABLE_OVERLAP, "const");
    f = Fresh(); BoxPut64(EntryIn(f, "data") + BOX_ENTRY_PLACE, 0x1000);
    Expect("data on the page of const", f, g_bytes, BOX_LOADABLE_OVERLAP, "data");
    f = Fresh(); BoxPut32(EntryIn(f, "fixups") + BOX_ENTRY_FLAGS, 0);
    Expect("fixups that need not be understood", f, g_bytes, BOX_PART_FLAGS, "fixups");
    f = Fresh(); BoxPut32(EntryIn(f, "code") + BOX_ENTRY_FLAGS,
                          BOX_FLAG_MUST_UNDERSTAND | BOX_FLAG_LOAD_AT_START);
    Expect("code marked to be loaded at start", f, g_bytes, BOX_PART_FLAGS, "code");
    f = Fresh(); BoxPut32(EntryIn(f, "hints") + BOX_ENTRY_FLAGS, BOX_FLAG_COMPRESSED);
    Expect("hints compressed", f, g_bytes, BOX_PART_FLAGS, "hints");
    f = Fresh(); BoxPut32(EntryIn(f, "names") + BOX_ENTRY_FLAGS, BOX_FLAG_LOAD_AT_START);
    Expect("names loaded at start", f, g_bytes, BOX_PART_FLAGS, "names");
    f = Fresh(); BoxPut32(EntryIn(f, "form") + BOX_ENTRY_FLAGS,
                          BOX_FLAG_LOAD_AT_START | BOX_FLAG_COMPRESSED);
    Expect("a part loaded at start and compressed", f, g_bytes, BOX_PART_FLAGS, "form");
    f = Fresh(); BoxPut32(EntryIn(f, "about:ru") + BOX_ENTRY_FLAGS, BOX_FLAG_COMPRESSED);
    Expect("a description compressed", f, g_bytes, BOX_OK, "");
    f = Fresh(); BoxPut32(EntryIn(f, "paint:brushes") + BOX_ENTRY_FLAGS, BOX_FLAG_MUST_UNDERSTAND);
    Expect("a part nobody here knows, to be understood", f, g_bytes, BOX_PART_UNKNOWN_REQUIRED,
           "paint:brushes");
    f = Fresh(); BoxPut16(EntryIn(f, "needs") + BOX_ENTRY_VERSION, 2);
    Expect("needs in a version nobody here knows", f, g_bytes, BOX_PART_VERSION_REQUIRED, "needs");
    f = Fresh(); BoxPut16(EntryIn(f, "life") + BOX_ENTRY_VERSION, 2);
    Expect("life in a later version, passed over", f, g_bytes, BOX_OK, "");
}

static void TestHashes(void)
{
    BoxTable box = SampleTable();
    BoxPart  part;
    uint8_t *f;

    Check(BoxSealHolds(&box, &g_hasher), "the seal of the sample holds");
    bool all = true;
    for (uint32_t i = 0; i < box.Head.PartCount; i++) {
        BoxPartAt(&box, i, &part);
        all = all && BoxPartHolds(&part, g_box + part.Offset, &g_hasher);
    }
    Check(all, "every part of the sample holds to its hash");

    f = Fresh();
    BoxPartOfKind(&box, BOX_KIND_CODE, &part);
    f[part.Offset + 5] ^= 0x40;
    box.Prefix = f;
    Check(!BoxPartHolds(&part, f + part.Offset, &g_hasher) && BoxSealHolds(&box, &g_hasher),
          "a byte of code changed: its hash differs, the seal still holds");
    f = Fresh();
    f[box.Head.TableAt + 3 * BOX_ENTRY_BYTES + BOX_ENTRY_HASH] ^= 1;
    Check(!BoxSealHolds(&box, &g_hasher), "a hash in the table changed: the seal differs");
    f = Fresh();
    f[box.Head.NamesAt + 2] ^= 0x20;
    Check(!BoxSealHolds(&box, &g_hasher), "a name changed: the seal differs");
    f = Fresh();
    f[OffsetOf("code") - 1] = 1;
    Expect("a byte between the parts not zero", f, g_bytes, BOX_GAP_NOT_ZERO, "code");
}

/* ── the parts with a layout of their own ────────────────────────────────── */

/* Each read from a copy of exactly its bytes, so a read past them is caught. */
static uint8_t *Exact(const uint8_t *bytes, uint64_t size)
{
    uint8_t *copy = malloc(size ? size : 1);
    memcpy(copy, bytes, size);
    return copy;
}

static BoxVerdict DoorsOf(const uint8_t *bytes, uint64_t size, const BoxTable *box, BoxDoors *doors)
{
    uint8_t   *copy    = Exact(bytes, size);
    BoxVerdict verdict = BoxDoorsRead(copy, size, box, doors);
    free(copy);
    return verdict;
}

static BoxVerdict ThreadOf(const uint8_t *bytes, uint64_t size)
{
    BoxThread  thread;
    uint8_t   *copy    = Exact(bytes, size);
    BoxVerdict verdict = BoxThreadRead(copy, size, &thread);
    free(copy);
    return verdict;
}

static BoxVerdict FixupsOf(const uint8_t *bytes, uint64_t size, const BoxTable *box)
{
    uint8_t   *copy    = Exact(bytes, size);
    BoxVerdict verdict = BoxFixupsCheck(copy, size, box);
    free(copy);
    return verdict;
}

static void TestDoors(void)
{
    BoxTable box = SampleTable();
    BoxDoors doors;
    uint64_t offset = 0;

    Check(BoxDoorsRead(g_box + OffsetOf("doors"), BoxU64(EntryIn(g_box, "doors") + BOX_ENTRY_SIZE), &box,
                       &doors) == BOX_OK &&
              doors.Count == 2 && BoxDoorFind(&doors, "start", &offset) && offset == 0x10 &&
              BoxDoorFind(&doors, "service", &offset) && offset == 0x2F0 &&
              !BoxDoorFind(&doors, "stop", &offset),
          "the doors read as written");

    uint8_t    bytes[128];
    uint64_t   size;
    BoxPenDoor one[1] = { { "help", 0x10 } };
    BoxPenDoors(one, 1, bytes, &size);
    Check(DoorsOf(bytes, size, &box, &doors) == BOX_NO_START, "doors without start");
    BoxPenDoor out[1] = { { "start", 0x300 } };
    BoxPenDoors(out, 1, bytes, &size);
    Check(DoorsOf(bytes, size, &box, &doors) == BOX_DOOR_OUTSIDE_CODE,
          "a door one past the code's bytes");
    BoxPenDoor twice[2] = { { "start", 0x10 }, { "start", 0x20 } };
    BoxPenDoors(twice, 2, bytes, &size);
    Check(DoorsOf(bytes, size, &box, &doors) == BOX_DOORS_LAYOUT, "a door named twice");
    BoxPenDoor fine[1] = { { "start", 0x2FF } };
    BoxPenDoors(fine, 1, bytes, &size);
    Check(DoorsOf(bytes, size, &box, &doors) == BOX_OK, "a door on the code's last byte");
    BoxPut32(bytes, 0x10000000u);
    Check(DoorsOf(bytes, size, &box, &doors) == BOX_DOORS_LAYOUT, "more doors than bytes");
    BoxPenDoors(fine, 1, bytes, &size);
    Check(DoorsOf(bytes, size + 1, &box, &doors) == BOX_DOORS_LAYOUT, "bytes past the door names");
    BoxPut16(bytes + 8 + 14, 1);
    Check(DoorsOf(bytes, size, &box, &doors) == BOX_DOORS_LAYOUT, "a door not zero where it must be");
    Check(DoorsOf(bytes, 4, &box, &doors) == BOX_DOORS_LAYOUT, "doors shorter than their head");
}

static void TestThread(void)
{
    uint8_t   bytes[64];
    uint64_t  size;
    BoxThread thread;

    BoxPenThread(16, (const uint8_t *)"tls-init", 8, 64, bytes, &size);
    Check(BoxThreadRead(bytes, size, &thread) == BOX_OK && thread.Align == 16 && thread.InitBytes == 8 &&
              thread.TotalBytes == 64 && memcmp(thread.Init, "tls-init", 8) == 0,
          "the thread template reads as written");
    BoxPenThread(24, (const uint8_t *)"tls-init", 8, 64, bytes, &size);
    Check(ThreadOf(bytes, size) == BOX_THREAD_LAYOUT, "an alignment not a power of two");
    BoxPenThread(8192, (const uint8_t *)"tls-init", 8, 64, bytes, &size);
    Check(ThreadOf(bytes, size) == BOX_THREAD_LAYOUT, "an alignment past a page");
    BoxPenThread(16, (const uint8_t *)"tls-init", 8, 4, bytes, &size);
    Check(ThreadOf(bytes, size) == BOX_THREAD_LAYOUT, "a block smaller than its bytes");
    BoxPenThread(16, (const uint8_t *)"tls-init", 8, 64, bytes, &size);
    Check(ThreadOf(bytes, size - 1) == BOX_THREAD_LAYOUT, "init bytes cut short");
    BoxPut64(bytes + 24, 5);
    Check(ThreadOf(bytes, size) == BOX_THREAD_LAYOUT, "a template not zero where it must be");
}

static bool Walks(const uint8_t *bytes, uint64_t size, const uint64_t *want, uint64_t count)
{
    BoxFixupWalk walk;
    uint64_t     offset, seen = 0;
    BoxFixupBegin(bytes, size, &walk);
    while (BoxFixupNext(&walk, &offset)) {
        if (seen >= count || offset != want[seen]) return false;
        seen++;
    }
    return seen == count;
}

static void TestFixups(void)
{
    BoxTable box = SampleTable();
    uint8_t  bytes[4096];
    uint64_t size;

    /* The encoding as RELR writers make it, to the word. */
    static const uint64_t places[] = { 0x2000, 0x2008, 0x2010, 0x3000 };
    BoxPenFixups(places, 4, bytes, &size);
    Check(size == 24 && BoxU64(bytes) == 0x2000 && BoxU64(bytes + 8) == 7 && BoxU64(bytes + 16) == 0x3000,
          "four places in three words: a place, a bitmap of two, a place");
    Check(Walks(bytes, size, places, 4), "the walk gives back the four places");

    Check(BoxFixupsCheck(g_box + OffsetOf("fixups"), BoxU64(EntryIn(g_box, "fixups") + BOX_ENTRY_SIZE),
                         &box) == BOX_OK &&
              Walks(g_box + OffsetOf("fixups"), BoxU64(EntryIn(g_box, "fixups") + BOX_ENTRY_SIZE), g_fixups, 5),
          "the sample's fixups read as written");

    /* Many places, every gap the encoding has a case for, back through the walk. */
    static uint64_t many[2000];
    uint64_t        at = 0x1000, state = 12345;
    for (unsigned i = 0; i < 2000; i++) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        at += 8 * (1 + (state >> 33) % (i % 3 == 0 ? 90 : 9));
        many[i] = at;
    }
    static uint8_t encoded[2000 * 8];
    Check(BoxPenFixups(many, 2000, encoded, &size) && Walks(encoded, size, many, 2000) && size < 2000 * 8,
          "two thousand places through the encoding and back, in fewer words");

    static const uint64_t falling[] = { 0x2008, 0x2000 };
    static const uint64_t odd[]     = { 0x2004 };
    Check(!BoxPenFixups(falling, 2, bytes, &size), "the pen refuses places that fall");
    Check(!BoxPenFixups(odd, 1, bytes, &size), "the pen refuses a place off a word");

    BoxPut64(bytes, 0x2000);
    Check(FixupsOf(bytes, 12, &box) == BOX_FIXUPS_LAYOUT, "fixups of half a word");
    BoxPut64(bytes, 7);
    Check(FixupsOf(bytes, 8, &box) == BOX_FIXUPS_LAYOUT, "a bitmap before any place");
    BoxPut64(bytes, 0x2020);
    Check(FixupsOf(bytes, 8, &box) == BOX_FIXUP_OUTSIDE, "a place in data's zeros");
    BoxPut64(bytes, 0x1040);
    Check(FixupsOf(bytes, 8, &box) == BOX_FIXUP_OUTSIDE, "a place between const and data");
    BoxPut64(bytes, 0x2004);
    Check(FixupsOf(bytes, 8, &box) == BOX_FIXUP_OUTSIDE, "a place off a word");
    BoxPut64(bytes, 0x8000000000000000ull);
    Check(FixupsOf(bytes, 8, &box) == BOX_FIXUP_OUTSIDE, "a place past the user half");
    BoxPut64(bytes, 0x2008);
    BoxPut64(bytes + 8, 0x2000);
    Check(FixupsOf(bytes, 16, &box) == BOX_FIXUP_ORDER, "places that fall");
    BoxPut64(bytes, 0x2000);
    BoxPut64(bytes + 8, ~0ull);
    Check(FixupsOf(bytes, 16, &box) == BOX_FIXUP_OUTSIDE, "a bitmap running past data's bytes");
    BoxPut64(bytes, 0x2018);
    Check(FixupsOf(bytes, 8, &box) == BOX_OK, "a place on data's last word");
}

/* ── the sample, written from parts the standard does not allow ─────────── */

static void TestWritten(void)
{
    Sample sample;

    SampleMake(&sample);
    PartNamed(&sample, "code")->Name = "code:x";
    ExpectWritten("code with a value", &sample, BOX_NAME_NOT_STANDARD, "code:x");
    SampleMake(&sample);
    PartNamed(&sample, "paint:brushes")->Name = "life";
    ExpectWritten("two parts of one name", &sample, BOX_NAME_ORDER, "life");
    SampleMake(&sample);
    PartNamed(&sample, "paint:brushes")->Name = "about:";
    ExpectWritten("a name with an empty value", &sample, BOX_NAME_SPELLING, "about:");
    SampleMake(&sample);
    PartNamed(&sample, "paint:brushes")->Name = "paint:...";
    ExpectWritten("a name that is a pattern", &sample, BOX_NAME_SPELLING, "paint:...");
    SampleMake(&sample);
    Remove(&sample, "code");
    Remove(&sample, "fixups");
    PartNamed(&sample, "doors")->Size = 8;
    PartNamed(&sample, "doors")->Memory = 8;
    ExpectWritten("no code", &sample, BOX_NO_CODE, "");
    SampleMake(&sample);
    Remove(&sample, "doors");
    ExpectWritten("no doors", &sample, BOX_NO_DOORS, "");
    SampleMake(&sample);
    PartNamed(&sample, "code")->Size = 0;
    PartNamed(&sample, "code")->Memory = 0;
    ExpectWritten("code of no bytes", &sample, BOX_PART_MEMORY, "code");
    SampleMake(&sample);
    PartNamed(&sample, "needs")->Flags = 0;
    ExpectWritten("needs that need not be understood", &sample, BOX_PART_FLAGS, "needs");
    SampleMake(&sample);
    PartNamed(&sample, "thread")->Size -= 1;
    PartNamed(&sample, "thread")->Memory -= 1;
    ExpectWritten("a thread template cut short", &sample, BOX_THREAD_LAYOUT, "thread");
    SampleMake(&sample);
    PartNamed(&sample, "code")->Bytes = sample.Code;
    PartNamed(&sample, "code")->Size = 0x10;
    PartNamed(&sample, "code")->Memory = 0x10;
    ExpectWritten("a door past code cut short", &sample, BOX_DOOR_OUTSIDE_CODE, "doors");
    SampleMake(&sample);
    PartNamed(&sample, "const")->Size = 0x8;
    PartNamed(&sample, "const")->Memory = 0x8;
    ExpectWritten("a fixup on const's one word", &sample, BOX_OK, "");
    SampleMake(&sample);
    PartNamed(&sample, "const")->Size = 0x4;
    PartNamed(&sample, "const")->Memory = 0x4;
    ExpectWritten("a fixup past const cut to half a word", &sample, BOX_FIXUP_OUTSIDE, "fixups");
    SampleMake(&sample);
    static const uint64_t tail[] = { 0x1008 };
    uint64_t tail_bytes;
    BoxPenFixups(tail, 1, sample.Fixups, &tail_bytes);
    PartNamed(&sample, "fixups")->Size = tail_bytes;
    PartNamed(&sample, "fixups")->Memory = tail_bytes;
    PartNamed(&sample, "const")->Size = 0xC;
    PartNamed(&sample, "const")->Memory = 0xC;
    ExpectWritten("a fixup on the last four bytes of const", &sample, BOX_FIXUP_OUTSIDE, "fixups");
    SampleMake(&sample);
    Remove(&sample, "fixups");
    Remove(&sample, "thread");
    Remove(&sample, "names");
    ExpectWritten("a box with no fixups, thread or names", &sample, BOX_OK, "");
}

/* ── the sample, whole ───────────────────────────────────────────────────── */

static void TestSample(void)
{
    BoxTable box = SampleTable();
    BoxPart  part;
    char     label[300];

    static const char *const order[] = { "about", "about:ru", "code", "const", "data", "doors", "fixups",
                                         "form", "hints", "icon:64", "life", "names", "needs",
                                         "paint:brushes", "thread" };
    bool in_order = box.Head.PartCount == 15;
    for (uint32_t i = 0; in_order && i < 15; i++)
        in_order = BoxPartAt(&box, i, &part) && BoxSpanIs(part.Name, part.NameLength, order[i]);
    Check(in_order, "the parts are in the table in the order of their names");

    for (uint32_t i = 0; i < 15; i++) {
        snprintf(label, sizeof(label), "part %s found by halves", order[i]);
        Check(BoxPartFind(&box, (const uint8_t *)order[i], strlen(order[i]), &part) && part.Index == i, label);
    }
    Check(!BoxPartFind(&box, (const uint8_t *)"icon:128", 8, &part) &&
              !BoxPartFind(&box, (const uint8_t *)"about:en", 8, &part) &&
              !BoxPartFind(&box, (const uint8_t *)"aardvark", 8, &part) &&
              !BoxPartFind(&box, (const uint8_t *)"zebra", 5, &part),
          "names the box has not are not found");

    Check(BoxPartOfKind(&box, BOX_KIND_DATA, &part) && part.Place == 0x2000 && part.Size == 0x20 &&
              part.Memory == 0x3000 && part.Offset % BOX_LOADABLE_ALIGN == 0 &&
              memcmp(g_box + part.Offset, "\xD0\xD1\xD2", 3) == 0,
          "data: where the table says, on a page of the file, with its zeros counted");
    Check(BoxPartOfKind(&box, BOX_KIND_ABOUT, &part) && BoxSpanIs(part.Name, part.NameLength, "about") &&
              box.Kinds[BOX_KIND_ICON] == BOX_NO_PART &&
              BoxPartFind(&box, (const uint8_t *)"icon:64", 7, &part) && part.Kind == BOX_KIND_ICON,
          "kinds: about bare, icon only with a size, found by name");
    Check(BoxPartFind(&box, (const uint8_t *)"paint:brushes", 13, &part) && part.Kind == BOX_KIND_UNKNOWN &&
              BoxPartFind(&box, (const uint8_t *)"form", 4, &part) &&
              part.Flags == BOX_FLAG_LOAD_AT_START,
          "a part of the program's own is passed over; form is loaded at start");

    Sample   sample;
    uint64_t bytes;
    BoxVerdict verdict;
    const char *refused;
    SampleMake(&sample);
    for (uint32_t i = 0; i < sample.Count / 2; i++) {
        BoxPenPart held = sample.Parts[i];
        sample.Parts[i] = sample.Parts[sample.Count - 1 - i];
        sample.Parts[sample.Count - 1 - i] = held;
    }
    uint8_t *again = Write(&sample, &bytes, &verdict, &refused);
    Check(verdict == BOX_OK && bytes == g_bytes && memcmp(again, g_box, bytes) == 0,
          "the same parts in another order make the same bytes");
    free(again);
}

static void TestNames(void)
{
    static const char *const rising[] = { "a", "about", "about:en", "about:ru", "code", "const", "icon:128",
                                          "icon:64", "z", "\xC3\xA9t\xC3\xA9" };
    bool ordered = true;
    for (unsigned i = 0; i + 1 < sizeof(rising) / sizeof(rising[0]); i++)
        ordered = ordered && BoxNameOrder((const uint8_t *)rising[i], strlen(rising[i]),
                                          (const uint8_t *)rising[i + 1], strlen(rising[i + 1])) < 0;
    Check(ordered, "names rise as tags rise: by key, bare first, then by value");

    static const char *const spelled[] = { "code", "icon:64", "paint:brushes:big", "\xC3\xA9t\xC3\xA9" };
    static const char *const not_spelled[] = { "", ":ru", "about:", " code", "co de", "co,de", "a...b",
                                               "tab\there", "del\x7F" };
    bool all = true;
    for (unsigned i = 0; i < sizeof(spelled) / sizeof(spelled[0]); i++)
        all = all && BoxNameSpelled((const uint8_t *)spelled[i], strlen(spelled[i]));
    for (unsigned i = 0; i < sizeof(not_spelled) / sizeof(not_spelled[0]); i++)
        all = all && !BoxNameSpelled((const uint8_t *)not_spelled[i], strlen(not_spelled[i]));
    char longest[BOX_NAME_SIDE_MAX + 2];
    memset(longest, 'k', sizeof(longest));
    all = all && BoxNameSpelled((const uint8_t *)longest, BOX_NAME_SIDE_MAX) &&
          !BoxNameSpelled((const uint8_t *)longest, BOX_NAME_SIDE_MAX + 1);
    Check(all, "names are spelled as tags, each side at most 255 bytes");
}

static uint64_t CountFacts(const char *text, BoxKind kind, BoxVerdict *verdict)
{
    BoxText        reading;
    const uint8_t *line;
    uint64_t       length, facts = 0;
    uint8_t       *exact = Exact((const uint8_t *)text, strlen(text));
    *verdict = BoxTextBegin(exact, strlen(text), kind, &reading);
    while (*verdict == BOX_OK && BoxTextLine(&reading, &line, &length)) facts++;
    free(exact);
    return facts;
}

static void TestTextParts(void)
{
    BoxVerdict verdict;
    Check(CountFacts("needs 1\ncpu avx2\ncpu bmi2\n", BOX_KIND_NEEDS, &verdict) == 2 && verdict == BOX_OK,
          "needs: two facts after its first line");
    Check(CountFacts("\n  \nneeds 1\n\n   cpu avx2   \n\ncpu bmi2", BOX_KIND_NEEDS, &verdict) == 2 && verdict == BOX_OK,
          "blank lines and blanks around words are nothing; the last newline may be left off");
    Check(CountFacts("needs 1", BOX_KIND_NEEDS, &verdict) == 0 && verdict == BOX_OK, "a part of its first line alone");
    CountFacts("hints 1\n", BOX_KIND_NEEDS, &verdict);
    Check(verdict == BOX_TEXT_LAYOUT, "a first line of another kind");
    CountFacts("needs 2\n", BOX_KIND_NEEDS, &verdict);
    Check(verdict == BOX_TEXT_LAYOUT, "a first line of another version");
    CountFacts("needs 1 more\n", BOX_KIND_NEEDS, &verdict);
    Check(verdict == BOX_TEXT_LAYOUT, "a first line of three words");
    CountFacts("", BOX_KIND_NEEDS, &verdict);
    Check(verdict == BOX_TEXT_LAYOUT, "no first line");
    CountFacts("needs 1\ncpu\tavx2\n", BOX_KIND_NEEDS, &verdict);
    Check(verdict == BOX_TEXT_LAYOUT, "a tab in a text part");
    CountFacts("needs 1\r\ncpu avx2\r\n", BOX_KIND_NEEDS, &verdict);
    Check(verdict == BOX_TEXT_LAYOUT, "a carriage return in a text part");

    const uint8_t *line = (const uint8_t *)"  cpu   sse4.1 ", *word;
    uint64_t       at = 0, length;
    Check(BoxTextWord(line, 15, &at, &word, &length) && length == 3 && memcmp(word, "cpu", 3) == 0 &&
              BoxTextWord(line, 15, &at, &word, &length) && length == 6 && memcmp(word, "sse4.1", 6) == 0 &&
              !BoxTextWord(line, 15, &at, &word, &length),
          "a line's words between blanks");

    bool words = true;
    for (unsigned a = 0; a < BOX_NEEDS_CPU_COUNT; a++) {
        const BoxNeedsCpu *need = &BoxNeedsCpuWords[a];
        words = words && BoxNeedsCpuFind((const uint8_t *)need->Word, strlen(need->Word)) == need &&
                need->Bit < 32 && need->Register <= BOX_NEEDS_EDX;
        for (unsigned b = 0; b < a; b++)
            words = words && strcmp(need->Word, BoxNeedsCpuWords[b].Word) != 0 &&
                    !(need->Leaf == BoxNeedsCpuWords[b].Leaf && need->Subleaf == BoxNeedsCpuWords[b].Subleaf &&
                      need->Register == BoxNeedsCpuWords[b].Register && need->Bit == BoxNeedsCpuWords[b].Bit);
    }
    Check(words, "every cpu word of needs is found as itself, once, and names a bit of its own");
    Check(BoxNeedsCpuFind((const uint8_t *)"avx", 3)->State == BOX_NEEDS_STATE_AVX &&
              BoxNeedsCpuFind((const uint8_t *)"avx512bw", 8)->State == BOX_NEEDS_STATE_AVX512 &&
              BoxNeedsCpuFind((const uint8_t *)"popcnt", 6)->State == 0 &&
              !BoxNeedsCpuFind((const uint8_t *)"avx2x", 5) && !BoxNeedsCpuFind((const uint8_t *)"avx", 2),
          "the words that need saved state say which; a word not in the list is not found");
}

static void TestTexts(void)
{
    const char *fallback = BoxVerdictText(BOX_VERDICT_COUNT);
    for (int a = 1; a < BOX_VERDICT_COUNT; a++) {
        const char *text = BoxVerdictText((BoxVerdict)a);
        bool        ok   = text[0] && strcmp(text, fallback) != 0;
        for (int b = 1; ok && b < a; b++) ok = strcmp(text, BoxVerdictText((BoxVerdict)b)) != 0;
        if (!ok) {
            char label[64];
            snprintf(label, sizeof(label), "verdict %d has words of its own", a);
            Check(false, label);
        } else {
            g_passed++;
        }
    }
}

int main(void)
{
    if (!BoxHostHashTrue()) {
        printf("box: harness failed — SHA-256 of \"abc\" is not FIPS 180-4's\n");
        return 2;
    }
    g_hasher = BoxHostHasher(&g_hash);

    Sample     sample;
    BoxVerdict verdict;
    const char *refused;
    SampleMake(&sample);
    g_box = Write(&sample, &g_bytes, &verdict, &refused);
    if (verdict != BOX_OK) {
        printf("box: harness failed — the sample was not written: %s (part '%s')\n",
               BoxVerdictText(verdict), refused);
        return 2;
    }
    g_copy = malloc(g_bytes);
    Expect("the sample", g_box, g_bytes, BOX_OK, "");

    TestSample();
    TestNames();
    TestHeader();
    TestTable();
    TestPartRules();
    TestHashes();
    TestDoors();
    TestThread();
    TestFixups();
    TestTextParts();
    TestWritten();
    TestTexts();

    free(g_copy);
    free(g_box);
    BoxHostHashFree(&g_hash);
    printf("box: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
