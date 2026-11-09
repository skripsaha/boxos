/* pack_stand — what tools/pack made of each program, held to the program.
 *
 *     tools/pack_stand.sh
 *
 * Given pairs of files — an ELF and the box pack made of it — the stand reads
 * the box with the one reader, whole (its seal, every part's hash, the zeros
 * between them, its doors, thread template and fixups), and the ELF with a
 * reading of its own, written here from the System V ABI and sharing nothing
 * with pack: every loadable segment must be the box's part of its kind, at its
 * page, byte for byte, its zeros counted — the R segment up to the end of its
 * own sections, the Nameplate and the Form after them left to their parts;
 * the entry the door `start`; PT_TLS
 * the thread template; the Nameplate, the Form and the Recipe the parts of
 * those names; and for a static PIE every relocation a fixup and every fixup a
 * relocation, the word it names holding the address it means at base 0. Each
 * box holds nothing else. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box_reader.h"
#include "box_host.h"

static int g_passed;
static int g_failed;

static void Check(bool ok, const char *path, const char *what)
{
    if (ok) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL %s: %s\n", path, what);
}

static uint8_t *ReadWhole(const char *path, uint64_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *bytes = malloc(length > 0 ? (size_t)length : 1);
    if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    *size = (uint64_t)length;
    return bytes;
}

static uint16_t U16(const uint8_t *at) { return (uint16_t)(at[0] | at[1] << 8); }
static uint32_t U32(const uint8_t *at) { return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24; }
static uint64_t U64(const uint8_t *at) { return (uint64_t)U32(at) | (uint64_t)U32(at + 4) << 32; }

/* ── the ELF, read here ──────────────────────────────────────────────────── */

typedef struct {
    uint64_t Offset, Virt, FileBytes, MemoryBytes;
    uint32_t Flags;
} Segment;

typedef struct {
    uint64_t Place, Addend;
} Relocation;

typedef struct {
    const uint8_t *File;
    uint64_t       Size;
    uint16_t       Type;
    uint64_t       Entry;
    Segment        Loads[8];
    unsigned       LoadCount;
    bool           Thread;
    Segment        Tls;
    uint64_t       TlsAlign;
    Relocation    *Relocations;
    uint64_t       RelocationCount;
    bool           Relr;        /* relocations from RELR: the word in the file is the address */
} Program;

static const uint8_t *Section(const Program *program, const char *want, uint32_t *type, uint64_t *size)
{
    const uint8_t *file    = program->File;
    uint64_t       table   = U64(file + 0x28);
    uint16_t       count   = U16(file + 0x3C), names = U16(file + 0x3E);
    const uint8_t *strings = file + U64(file + table + (uint64_t)names * 64 + 24);
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *header = file + table + (uint64_t)i * 64;
        if (strcmp((const char *)strings + U32(header), want) != 0) continue;
        *type = U32(header + 4);
        *size = U64(header + 32);
        return file + U64(header + 24);
    }
    return NULL;
}

static int RelocationOrder(const void *a, const void *b)
{
    uint64_t x = ((const Relocation *)a)->Place, y = ((const Relocation *)b)->Place;
    return x < y ? -1 : x > y;
}

static void ReadProgram(Program *program)
{
    const uint8_t *file = program->File;
    program->Type  = U16(file + 0x10);
    program->Entry = U64(file + 0x18);
    for (uint16_t i = 0; i < U16(file + 0x38); i++) {
        const uint8_t *header = file + U64(file + 0x20) + (uint64_t)i * 56;
        Segment segment = { U64(header + 8), U64(header + 16), U64(header + 32), U64(header + 40), U32(header + 4) };
        if (U32(header) == 1 && segment.MemoryBytes > 0 && (segment.Flags & 7) != 0 && program->LoadCount < 8)
            program->Loads[program->LoadCount++] = segment;
        if (U32(header) == 7) {
            program->Thread   = true;
            program->Tls      = segment;
            program->TlsAlign = U64(header + 48) ? U64(header + 48) : 1;
        }
    }

    uint32_t       type;
    uint64_t       size;
    const uint8_t *rela = Section(program, ".rela.dyn", &type, &size);
    uint64_t       room = 64;
    program->Relocations = malloc(room * sizeof(Relocation));
    if (rela) {
        for (uint64_t at = 0; at + 24 <= size; at += 24) {
            if (program->RelocationCount == room) program->Relocations = realloc(program->Relocations, (room *= 2) * sizeof(Relocation));
            program->Relocations[program->RelocationCount++] = (Relocation){ U64(rela + at), U64(rela + at + 16) };
        }
    }
    /* RELR, decoded as the gABI proposal gives it — not through the reader. */
    const uint8_t *relr = Section(program, ".relr.dyn", &type, &size);
    if (relr && size > 0) {
        uint64_t where = 0;
        program->Relr  = true;
        for (uint64_t at = 0; at + 8 <= size; at += 8) {
            uint64_t word = U64(relr + at);
            if ((word & 1) == 0) {
                if (program->RelocationCount == room) program->Relocations = realloc(program->Relocations, (room *= 2) * sizeof(Relocation));
                program->Relocations[program->RelocationCount++] = (Relocation){ word, 0 };
                where = word + 8;
            } else {
                for (unsigned bit = 1; bit < 64; bit++) {
                    if (!(word >> bit & 1)) continue;
                    if (program->RelocationCount == room) program->Relocations = realloc(program->Relocations, (room *= 2) * sizeof(Relocation));
                    program->Relocations[program->RelocationCount++] = (Relocation){ where + (bit - 1) * 8, 0 };
                }
                where += 63 * 8;
            }
        }
    }
    qsort(program->Relocations, program->RelocationCount, sizeof(Relocation), RelocationOrder);
}

/* Where the constants of the R segment end, from the page it starts on: the
 * end of its last allocated section with bytes that is not the Nameplate or
 * the Form, 0 when it holds nothing else. */
static uint64_t ConstantsEnd(const Program *program, const Segment *segment)
{
    const uint8_t *file    = program->File;
    uint64_t       table   = U64(file + 0x28);
    uint16_t       count   = U16(file + 0x3C), names = U16(file + 0x3E);
    const char    *strings = (const char *)file + U64(file + table + (uint64_t)names * 64 + 24);
    uint64_t       page    = segment->Virt - segment->Virt % 4096, end = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *header = file + table + (uint64_t)i * 64;
        const char    *name   = strings + U32(header);
        uint64_t       addr   = U64(header + 16), size = U64(header + 32);
        if (!(U64(header + 8) & 2) || U32(header + 4) == 8 || size == 0 || addr < segment->Virt ||
            addr >= segment->Virt + segment->MemoryBytes || strcmp(name, ".nameplate") == 0 ||
            strcmp(name, ".boxos.form") == 0)
            continue;
        if (addr + size - page > end) end = addr + size - page;
    }
    return end;
}

/* The word a relocation at `place` names, as the box must hold it. */
static uint64_t WantedWord(const Program *program, const Relocation *relocation, const Segment *segment)
{
    if (!program->Relr) return relocation->Addend;
    return U64(program->File + segment->Offset + (relocation->Place - segment->Virt));
}

/* ── one pair ────────────────────────────────────────────────────────────── */

static void CheckPair(const char *elf_path, const char *box_path, const BoxHasher *hasher)
{
    Program program = { 0 };
    uint64_t box_size;
    uint8_t *box = ReadWhole(box_path, &box_size);
    program.File = ReadWhole(elf_path, &program.Size);
    if (!box || !program.File) {
        Check(false, elf_path, "the program or its box cannot be read");
        free(box);
        free((void *)program.File);
        return;
    }
    ReadProgram(&program);

    /* The box, whole, by the one reader. */
    BoxHead  head;
    BoxTable table;
    BoxPart  part;
    bool     read = BoxReadHead(box, box_size, box_size, &head) == BOX_OK &&
                    BoxRead(box, box_size, &head, &table) == BOX_OK && BoxSealHolds(&table, hasher) &&
                    BoxGapsZero(box, &table) == BOX_OK;
    for (uint32_t i = 0; read && i < head.PartCount; i++)
        read = BoxPartAt(&table, i, &part) && BoxPartHolds(&part, box + part.Offset, hasher);
    Check(read, box_path, "the box is read whole: its seal, every hash, the zeros between its parts");
    if (!read) goto done;

    unsigned expected = 1;   /* doors */

    /* Every loadable segment is the part of its kind, at its page, byte for byte. */
    for (unsigned i = 0; i < program.LoadCount; i++) {
        const Segment *segment = &program.Loads[i];
        BoxKind kind = (segment->Flags & 1) ? BOX_KIND_CODE : (segment->Flags & 2) ? BOX_KIND_DATA : BOX_KIND_CONST;
        uint64_t lead   = segment->Virt % 4096;
        uint64_t memory = lead + segment->MemoryBytes;
        uint64_t size   = kind == BOX_KIND_DATA ? lead + segment->FileBytes : memory;
        if (kind == BOX_KIND_CONST) memory = size = ConstantsEnd(&program, segment);
        if (memory == 0) {
            Check(!BoxPartOfKind(&table, kind, &part), elf_path, "no constants when its R segment holds none");
            continue;
        }
        bool     same = BoxPartOfKind(&table, kind, &part) && part.Place == segment->Virt - lead &&
                        part.Memory == memory && part.Size == size;
        for (uint64_t at = 0; same && at < part.Size; at++) {
            uint8_t want = 0;
            if (at >= lead && at - lead < segment->FileBytes) want = program.File[segment->Offset + at - lead];
            same = box[part.Offset + at] == want;
            if (!same) {
                /* A byte of a word a relocation names: held below, as the word. */
                uint64_t virt = part.Place + at;
                for (uint64_t r = 0; r < program.RelocationCount; r++)
                    if (virt >= program.Relocations[r].Place && virt < program.Relocations[r].Place + 8) same = true;
            }
        }
        char what[96];
        snprintf(what, sizeof(what), "its %s segment is the part %s, byte for byte", kind == BOX_KIND_CODE ? "R+X" : kind == BOX_KIND_DATA ? "R+W" : "R", BoxKindWord(kind));
        Check(same, elf_path, what);
        expected++;
    }

    BoxDoors doors;
    uint64_t start = 0;
    Check(BoxPartOfKind(&table, BOX_KIND_DOORS, &part) &&
              BoxDoorsRead(box + part.Offset, part.Size, &table, &doors) == BOX_OK && doors.Count == 1 &&
              BoxDoorFind(&doors, "start", &start) && start == program.Entry,
          elf_path, "its entry is the one door, start");

    if (program.Thread && program.Tls.MemoryBytes > 0) {
        BoxThread thread;
        Check(BoxPartOfKind(&table, BOX_KIND_THREAD, &part) &&
                  BoxThreadRead(box + part.Offset, part.Size, &thread) == BOX_OK &&
                  thread.Align == program.TlsAlign && thread.TotalBytes == program.Tls.MemoryBytes &&
                  thread.InitBytes == program.Tls.FileBytes &&
                  memcmp(thread.Init, program.File + program.Tls.Offset, thread.InitBytes) == 0,
              elf_path, "its PT_TLS is the thread template");
        expected++;
    } else {
        Check(!BoxPartOfKind(&table, BOX_KIND_THREAD, &part), elf_path, "no thread template without PT_TLS");
    }

    if (program.Type == 3) {
        bool same = BoxPartOfKind(&table, BOX_KIND_FIXUPS, &part) &&
                    BoxFixupsCheck(box + part.Offset, part.Size, &table) == BOX_OK;
        BoxFixupWalk walk;
        uint64_t     place, seen = 0;
        if (same) BoxFixupBegin(box + part.Offset, part.Size, &walk);
        while (same && BoxFixupNext(&walk, &place)) {
            same = seen < program.RelocationCount && place == program.Relocations[seen].Place;
            for (unsigned i = 0; same && i < program.LoadCount; i++) {
                const Segment *segment = &program.Loads[i];
                if (place < segment->Virt || place - segment->Virt >= segment->FileBytes) continue;
                BoxKind kind = (segment->Flags & 1) ? BOX_KIND_CODE : (segment->Flags & 2) ? BOX_KIND_DATA : BOX_KIND_CONST;
                BoxPart holder;
                BoxPartOfKind(&table, kind, &holder);
                same = BoxU64(box + holder.Offset + (place - holder.Place)) ==
                       WantedWord(&program, &program.Relocations[seen], segment);
            }
            seen++;
        }
        Check(same && seen == program.RelocationCount, elf_path,
              "every relocation is a fixup, the word it names the address it means at base 0");
        expected++;
    } else {
        Check(!BoxPartOfKind(&table, BOX_KIND_FIXUPS, &part), elf_path, "no fixups for a program at a fixed address");
    }

    static const char *const carried[3][2] = { { ".nameplate", "names" }, { ".boxos.form", "form" },
                                               { ".boxos.recipe", "recipe" } };
    for (unsigned i = 0; i < 3; i++) {
        uint32_t       type;
        uint64_t       size;
        const uint8_t *bytes = Section(&program, carried[i][0], &type, &size);
        /* The Form and the Recipe are text: the zeros ending their C string stay behind. */
        if (bytes && type != 8 && i > 0)
            while (size > 0 && bytes[size - 1] == 0) size--;
        bool           there = bytes && type != 8 && size > 0;
        bool           found = BoxPartFind(&table, (const uint8_t *)carried[i][1], strlen(carried[i][1]), &part);
        char           what[96];
        snprintf(what, sizeof(what), "its %s section is the part %s", carried[i][0], carried[i][1]);
        Check(there ? found && part.Size == size && memcmp(box + part.Offset, bytes, size) == 0 &&
                          part.Flags == (i == 1 ? BOX_FLAG_LOAD_AT_START : 0u)
                    : !found,
              elf_path, what);
        if (there) expected++;
    }
    Check(head.PartCount == expected, elf_path, "the box holds nothing else");

done:
    free(program.Relocations);
    free((void *)program.File);
    free(box);
}

int main(int argc, char **argv)
{
    if (argc % 2 != 1) {
        printf("pack: harness failed — give pairs: PROGRAM.elf PROGRAM.box ...\n");
        return 2;
    }
    if (!BoxHostHashTrue()) {
        printf("pack: harness failed — SHA-256 of \"abc\" is not FIPS 180-4's\n");
        return 2;
    }
    BoxHostHash state  = { 0 };
    BoxHasher   hasher = BoxHostHasher(&state);
    for (int i = 1; i + 1 < argc; i += 2) CheckPair(argv[i], argv[i + 1], &hasher);
    BoxHostHashFree(&state);
    printf("pack: %d passed, %d failed — %d programs\n", g_passed, g_failed, (argc - 1) / 2);
    return g_failed == 0 ? 0 : 1;
}
