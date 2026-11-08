/* elf_stand — the kernel's ELF reader (src/kernel/core/process/elf_image.c) on
 * the host, held to exact verdicts.
 *
 *     tools/elf_stand.sh
 *
 * Every case starts from one small image the reader must accept — an R+X
 * segment and an R+W one with a tail of zeros, a PT_TLS beside them, the shape
 * of every program this tree builds — breaks one thing in it, and holds the
 * reader to the verdict and the program header it names. Each image is handed
 * over in a buffer of exactly its length, so a read one byte past the file is
 * caught by the address sanitizer rather than passing. Then every ELF named on
 * the command line — the programs the tree has built — must be accepted whole.
 *
 * The ELF layout here is the stand's own, written from the System V ABI and
 * not taken from the reader, so the two can disagree. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf_image.h"

#define ZONE_START 0xC000ull
#define ZONE_END   0x10000000ull

typedef struct __attribute__((packed)) {
    uint8_t  Ident[16];
    uint16_t Type;
    uint16_t Machine;
    uint32_t Version;
    uint64_t Entry;
    uint64_t HeadersAt;
    uint64_t SectionsAt;
    uint32_t Flags;
    uint16_t HeaderSize;
    uint16_t EntrySize;
    uint16_t Count;
    uint16_t SectionEntrySize;
    uint16_t SectionCount;
    uint16_t SectionNames;
} StandElf;

typedef struct __attribute__((packed)) {
    uint32_t Type;
    uint32_t Flags;
    uint64_t Offset;
    uint64_t Virt;
    uint64_t Phys;
    uint64_t FileBytes;
    uint64_t MemoryBytes;
    uint64_t Align;
} StandSegment;

enum { PT_LOAD = 1, PT_INTERP = 3, PT_NOTE = 4, PT_TLS = 7 };
#define PT_GNU_STACK 0x6474E551u
enum { PF_X = 1, PF_W = 2, PF_R = 4 };

#define IMAGE_ROOM 0x4000u

static uint8_t  g_image[IMAGE_ROOM];
static uint64_t g_bytes;
static int      g_passed;
static int      g_failed;

static StandElf *Head(void)
{
    return (StandElf *)g_image;
}

static StandSegment *Segment(unsigned index)
{
    return (StandSegment *)(g_image + Head()->HeadersAt + index * sizeof(StandSegment));
}

/* The image every case starts from: code at the start of the zone, data on the
 * next page with zeros after its bytes, the TLS template inside the data. */
static void Reset(void)
{
    memset(g_image, 0, sizeof(g_image));
    StandElf *head = Head();
    memcpy(head->Ident, "\x7F" "ELF", 4);
    head->Ident[4]   = 2;
    head->Ident[5]   = 1;
    head->Ident[6]   = 1;
    head->Type       = 2;
    head->Machine    = 62;
    head->Version    = 1;
    head->Entry      = 0xC000;
    head->HeadersAt  = sizeof(StandElf);
    head->HeaderSize = sizeof(StandElf);
    head->EntrySize  = sizeof(StandSegment);
    head->Count      = 3;

    *Segment(0) = (StandSegment){ PT_LOAD, PF_R | PF_X, 0x1000, 0xC000, 0xC000, 0x100, 0x100, 0x1000 };
    *Segment(1) = (StandSegment){ PT_LOAD, PF_R | PF_W, 0x2000, 0xD000, 0xD000, 0x10, 0x2000, 0x1000 };
    *Segment(2) = (StandSegment){ PT_TLS, PF_R, 0x2008, 0xD008, 0xD008, 0, 8, 8 };
    for (unsigned i = 0; i < 0x100; i++)
        g_image[0x1000 + i] = (uint8_t)(0x90 + (i & 7));
    memset(g_image + 0x2000, 0xDA, 0x10);
    g_bytes = 0x3000;
}

/* The image in a buffer of exactly its length, read; the caller frees it. */
static ElfImageVerdict Read(ElfImage *out, uint8_t **copy)
{
    *copy = malloc(g_bytes ? g_bytes : 1);
    if (!*copy) {
        printf("elf: harness failed — no memory for a copy of %llu bytes\n",
               (unsigned long long)g_bytes);
        exit(2);
    }
    memcpy(*copy, g_image, g_bytes);
    return ElfImageRead(*copy, g_bytes, ZONE_START, ZONE_END, out);
}

static void Expect(const char *label, ElfImageVerdict want, unsigned want_header)
{
    ElfImage        image;
    uint8_t        *copy;
    ElfImageVerdict got = Read(&image, &copy);
    free(copy);

    if (got == want && (want == ELF_IMAGE_OK || image.Header == want_header)) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s\n  got:  %s (header %u)\n  want: %s (header %u)\n", label,
           ElfImageVerdictText(got), (unsigned)image.Header, ElfImageVerdictText(want), want_header);
}

#define WHOLE ELF_IMAGE_NO_HEADER

static void TestAccepted(void)
{
    ElfImage        image;
    ElfImageSegment segment;
    uint8_t        *copy;
    uint16_t        at = 0;

    Reset();
    ElfImageVerdict got = Read(&image, &copy);
    bool ok = got == ELF_IMAGE_OK && image.Entry == 0xC000 && image.HeaderCount == 3 &&
              image.Header == WHOLE;

    ok = ok && ElfImageNextSegment(&image, &at, &segment) && at == 1 &&
         segment.Virt == 0xC000 && segment.FileOffset == 0x1000 && segment.FileBytes == 0x100 &&
         segment.MemoryBytes == 0x100 && !segment.Writable && segment.Executable;
    ok = ok && ElfImageNextSegment(&image, &at, &segment) && at == 2 &&
         segment.Virt == 0xD000 && segment.FileOffset == 0x2000 && segment.FileBytes == 0x10 &&
         segment.MemoryBytes == 0x2000 && segment.Writable && !segment.Executable;
    ok = ok && !ElfImageNextSegment(&image, &at, &segment) && at == 3;
    free(copy);

    if (ok) {
        g_passed++;
    } else {
        g_failed++;
        printf("FAIL the image every case starts from is not read as it is (%s)\n",
               ElfImageVerdictText(got));
    }
}

static void TestHeader(void)
{
    Reset(); g_bytes = 0;                Expect("an empty file", ELF_IMAGE_NOT_ELF, WHOLE);
    Reset(); g_bytes = 3;                Expect("three bytes", ELF_IMAGE_NOT_ELF, WHOLE);
    Reset(); g_image[1] = 'e';           Expect("magic", ELF_IMAGE_NOT_ELF, WHOLE);
    Reset(); g_bytes = 63;               Expect("a header one byte short", ELF_IMAGE_TRUNCATED, WHOLE);
    Reset(); Head()->Ident[4] = 1;       Expect("32-bit", ELF_IMAGE_NOT_64_BIT, WHOLE);
    Reset(); Head()->Ident[5] = 2;       Expect("big-endian", ELF_IMAGE_NOT_LITTLE_ENDIAN, WHOLE);
    Reset(); Head()->Type = 3;           Expect("ET_DYN", ELF_IMAGE_NOT_EXECUTABLE, WHOLE);
    Reset(); Head()->Type = 1;           Expect("ET_REL", ELF_IMAGE_NOT_EXECUTABLE, WHOLE);
    Reset(); Head()->Machine = 3;        Expect("i386", ELF_IMAGE_NOT_X86_64, WHOLE);
    Reset(); Head()->EntrySize = 32;     Expect("32-byte program headers", ELF_IMAGE_HEADER_SIZE, WHOLE);
    Reset(); Head()->EntrySize = 64;     Expect("64-byte program headers", ELF_IMAGE_HEADER_SIZE, WHOLE);
    Reset(); Head()->Count = 0;          Expect("no program headers", ELF_IMAGE_NO_HEADERS, WHOLE);
    Reset(); Head()->Count = 0xFFFF;
    Expect("PN_XNUM", ELF_IMAGE_HEADER_COUNT_ELSEWHERE, WHOLE);
}

/* The table of program headers held to the file, the wrap of the old loader
 * first: e_phoff near 2^64 plus e_phnum*56 came back round to a small number
 * and passed. */
static void TestHeaderTable(void)
{
    Reset(); Head()->HeadersAt = UINT64_MAX - 8;
    Expect("a table whose end wraps past 2^64", ELF_IMAGE_HEADERS_OUTSIDE_FILE, WHOLE);
    Reset(); Head()->HeadersAt = 0x3000;
    Expect("a table starting at the end of the file", ELF_IMAGE_HEADERS_OUTSIDE_FILE, WHOLE);
    Reset(); Head()->HeadersAt = 0x3000 - 3 * sizeof(StandSegment) + 1;
    Expect("a table one byte past the end", ELF_IMAGE_HEADERS_OUTSIDE_FILE, WHOLE);
    Reset(); Head()->Count = 300;
    Expect("a count of headers the file has no room for", ELF_IMAGE_HEADERS_OUTSIDE_FILE, WHOLE);

    Reset();
    StandSegment moved[3];
    memcpy(moved, Segment(0), sizeof(moved));
    memset(Segment(0), 0, sizeof(moved));
    Head()->HeadersAt = 0x3000 - sizeof(moved);
    memcpy(Segment(0), moved, sizeof(moved));
    Expect("a table ending on the last byte of the file", ELF_IMAGE_OK, WHOLE);
}

static void TestSegmentInFile(void)
{
    Reset(); Segment(0)->Offset = UINT64_MAX - 0xF;
    Expect("segment bytes whose end wraps past 2^64", ELF_IMAGE_SEGMENT_OUTSIDE_FILE, 0);
    Reset(); Segment(1)->Offset = 0x3000 - 0xF;
    Expect("segment bytes one past the end", ELF_IMAGE_SEGMENT_OUTSIDE_FILE, 1);
    Reset(); Segment(1)->Offset = 0x3000 - 0x10;
    Expect("segment bytes ending on the last byte", ELF_IMAGE_OK, WHOLE);
    Reset(); Segment(1)->Offset = 0x3001; Segment(1)->FileBytes = 0;
    Expect("an empty file part past the end", ELF_IMAGE_SEGMENT_OUTSIDE_FILE, 1);
    Reset(); Segment(0)->FileBytes = 0x101;
    Expect("more in the file than in memory", ELF_IMAGE_SEGMENT_FILE_OVER_MEMORY, 0);
    Reset(); Segment(1)->MemoryBytes = 0;
    Expect("file bytes with no memory", ELF_IMAGE_SEGMENT_FILE_OVER_MEMORY, 1);
}

/* The segment held to the zone, the memory-size wrap of the old loader first:
 * memsz + the page offset + 4095 came back round to one page, and the copy of
 * the file part then ran past it into whatever the kernel had there. */
static void TestSegmentInZone(void)
{
    Reset(); Segment(1)->Virt = 0xD800; Segment(1)->MemoryBytes = UINT64_MAX;
    Expect("a memory size that wraps the page count", ELF_IMAGE_SEGMENT_OUTSIDE_ZONE, 1);
    Reset(); Segment(0)->Virt = 0xB000; Head()->Entry = 0xB000;
    Expect("below the zone", ELF_IMAGE_SEGMENT_OUTSIDE_ZONE, 0);
    Reset(); Segment(1)->Virt = ZONE_END;
    Expect("at the end of the zone", ELF_IMAGE_SEGMENT_OUTSIDE_ZONE, 1);
    Reset(); Segment(1)->Virt = 0xFFFF800000000000ull;
    Expect("in the kernel's half", ELF_IMAGE_SEGMENT_OUTSIDE_ZONE, 1);
    Reset(); Segment(1)->Virt = 0x400000000000ull;
    Expect("over the ring slots", ELF_IMAGE_SEGMENT_OUTSIDE_ZONE, 1);
    Reset(); Segment(1)->MemoryBytes = ZONE_END - 0xD000;
    Expect("ending exactly where the zone ends", ELF_IMAGE_OK, WHOLE);
    Reset(); Segment(1)->MemoryBytes = ZONE_END - 0xD000 + 1;
    Expect("ending one byte past the zone", ELF_IMAGE_SEGMENT_OUTSIDE_ZONE, 1);
}

static void TestSegmentRights(void)
{
    Reset(); Segment(1)->Flags = PF_R | PF_W | PF_X;
    Expect("data writable and executable", ELF_IMAGE_SEGMENT_WRITABLE_EXECUTABLE, 1);
    Reset(); Segment(0)->Flags = PF_R | PF_W | PF_X;
    Expect("code writable and executable", ELF_IMAGE_SEGMENT_WRITABLE_EXECUTABLE, 0);
    Reset(); Segment(0)->Flags = PF_W | PF_X;
    Expect("writable and executable, not readable", ELF_IMAGE_SEGMENT_WRITABLE_EXECUTABLE, 0);
    Reset(); Segment(1)->Flags = PF_R;
    Expect("a read-only segment", ELF_IMAGE_OK, WHOLE);
}

static void TestSegmentOrder(void)
{
    Reset(); Segment(1)->Virt = 0xC100;
    Expect("data on the page of the code", ELF_IMAGE_SEGMENT_OUT_OF_ORDER, 1);
    Reset(); Segment(0)->MemoryBytes = 0x1001;
    Expect("code running onto the page of the data", ELF_IMAGE_SEGMENT_OUT_OF_ORDER, 1);
    Reset(); Segment(0)->MemoryBytes = 0x1000;
    Expect("code filling its page, data on the next", ELF_IMAGE_OK, WHOLE);
    Reset(); Segment(1)->Virt = 0xE000;
    Expect("a page left between them", ELF_IMAGE_OK, WHOLE);

    Reset();
    StandSegment code = *Segment(0);
    *Segment(0) = *Segment(1);
    *Segment(1) = code;
    Expect("data listed before the code below it", ELF_IMAGE_SEGMENT_OUT_OF_ORDER, 1);
}

static void TestOtherHeaders(void)
{
    Reset(); Segment(2)->Type = PT_INTERP;
    Expect("a program interpreter", ELF_IMAGE_INTERPRETER, 2);
    Reset(); *Segment(2) = (StandSegment){ PT_GNU_STACK, PF_R | PF_W | PF_X, 0, 0, 0, 0, 0, 16 };
    Expect("an executable stack", ELF_IMAGE_STACK_EXECUTABLE, 2);
    Reset(); *Segment(2) = (StandSegment){ PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0, 16 };
    Expect("a stack that is not executable", ELF_IMAGE_OK, WHOLE);
    Reset(); *Segment(2) = (StandSegment){ PT_NOTE, PF_R | PF_W | PF_X, UINT64_MAX, UINT64_MAX,
                                           0, UINT64_MAX, UINT64_MAX, 0 };
    Expect("a note that is no segment, whatever it says", ELF_IMAGE_OK, WHOLE);
}

static void TestNothingAndEntry(void)
{
    Reset(); Segment(0)->Type = PT_NOTE; Segment(1)->Type = PT_NOTE;
    Expect("no PT_LOAD", ELF_IMAGE_NOTHING_TO_LOAD, WHOLE);
    Reset(); Segment(0)->FileBytes = 0; Segment(0)->MemoryBytes = 0;
             Segment(1)->FileBytes = 0; Segment(1)->MemoryBytes = 0;
    Expect("only empty PT_LOADs", ELF_IMAGE_NOTHING_TO_LOAD, WHOLE);

    Reset(); Head()->Entry = 0xD000;
    Expect("the entry in the data", ELF_IMAGE_ENTRY_OUTSIDE_CODE, WHOLE);
    Reset(); Head()->Entry = 0xC100;
    Expect("the entry one past the code's bytes", ELF_IMAGE_ENTRY_OUTSIDE_CODE, WHOLE);
    Reset(); Head()->Entry = 0xC0FF;
    Expect("the entry on the code's last byte", ELF_IMAGE_OK, WHOLE);
    Reset(); Head()->Entry = 0;
    Expect("the entry at zero", ELF_IMAGE_ENTRY_OUTSIDE_CODE, WHOLE);
    Reset(); Segment(0)->MemoryBytes = 0x200; Head()->Entry = 0xC180;
    Expect("the entry in code's zeros past its bytes", ELF_IMAGE_ENTRY_OUTSIDE_CODE, WHOLE);
}

/* The first thing wrong is the one said, and the header it is in is named. */
static void TestFirstReasonNamed(void)
{
    Reset(); Segment(1)->Flags = PF_R | PF_W | PF_X; Segment(2)->Type = PT_INTERP;
    Expect("W+X before an interpreter", ELF_IMAGE_SEGMENT_WRITABLE_EXECUTABLE, 1);
    Reset(); Segment(0)->Type = PT_INTERP; Segment(1)->Flags = PF_R | PF_W | PF_X;
    Expect("an interpreter before W+X", ELF_IMAGE_INTERPRETER, 0);
}

static void TestTexts(void)
{
    const char *fallback = ElfImageVerdictText(ELF_IMAGE_VERDICT_COUNT);
    for (int a = 1; a < ELF_IMAGE_VERDICT_COUNT; a++) {
        const char *text = ElfImageVerdictText((ElfImageVerdict)a);
        bool ok = text && text[0] && strcmp(text, fallback) != 0;
        for (int b = 1; ok && b < a; b++)
            ok = strcmp(text, ElfImageVerdictText((ElfImageVerdict)b)) != 0;
        if (ok) {
            g_passed++;
        } else {
            g_failed++;
            printf("FAIL verdict %d has no words of its own\n", a);
        }
    }
}

/* A program this tree built, read as the kernel reads it. */
static void TestBuilt(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        g_failed++;
        printf("FAIL %s: cannot be opened\n", path);
        return;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc(size > 0 ? (size_t)size : 1);
    bool     read  = bytes && size > 0 && fread(bytes, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!read) {
        free(bytes);
        g_failed++;
        printf("FAIL %s: cannot be read\n", path);
        return;
    }

    ElfImage        image;
    ElfImageSegment segment;
    uint16_t        at       = 0;
    unsigned        segments = 0;
    ElfImageVerdict got = ElfImageRead(bytes, (uint64_t)size, ZONE_START, ZONE_END, &image);
    while (got == ELF_IMAGE_OK && ElfImageNextSegment(&image, &at, &segment))
        segments++;
    free(bytes);

    if (got == ELF_IMAGE_OK && segments > 0) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s: %s (header %u)\n", path, ElfImageVerdictText(got), (unsigned)image.Header);
}

int main(int argc, char **argv)
{
    TestAccepted();
    TestHeader();
    TestHeaderTable();
    TestSegmentInFile();
    TestSegmentInZone();
    TestSegmentRights();
    TestSegmentOrder();
    TestOtherHeaders();
    TestNothingAndEntry();
    TestFirstReasonNamed();
    TestTexts();
    for (int i = 1; i < argc; i++)
        TestBuilt(argv[i]);
    printf("elf: %d passed, %d failed — %d built programs among them\n",
           g_passed, g_failed, argc - 1);
    return g_failed == 0 ? 0 : 1;
}
