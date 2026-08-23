/*
 * nameplate.c — build, verify and read the Nameplate table of a BoxOS image.
 *
 * The format and the reasoning behind it live in src/include/nameplate_format.h.
 * This is the half that runs on the build host.
 *
 * Usage:
 *   nameplate build  <pass1.elf> <out.o>   emit the table of pass1 as a
 *                                          relocatable object to link into
 *                                          pass 2
 *   nameplate verify <final.elf>           re-derive the table from the final
 *                                          binary and compare — this is what
 *                                          makes the two-pass link trustworthy
 *   nameplate list   <elf> [addr]          print the table, or resolve one
 *                                          address (the offline answer that
 *                                          x86_64-elf-nm used to give before
 *                                          images stopped carrying .symtab)
 *
 * Why two passes at all: the table names function addresses, and it cannot be
 * built before those addresses exist. Pass 1 links without it, pass 2 links
 * with it. The section is placed in the read-only region AFTER .text, so
 * adding it moves .data and .bss but not a single function — and `verify`
 * proves that rather than assuming it: if any address moved, the build fails
 * instead of shipping a table that names the wrong function.
 *
 * ELF structures are declared here rather than included: the build host is
 * macOS as often as Linux, and macOS has no <elf.h>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "nameplate_format.h"  /* shared with boxlib and the kernel (-I src/include) */

/* ====================================================================
 * ELF64, little-endian — only what this tool touches
 * ==================================================================== */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64Sym;

#define ET_REL          1
#define EM_X86_64       62
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1

#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

#define SHN_UNDEF       0
#define SHN_ABS         0xFFF1
#define SHN_COMMON      0xFFF2

#define STT_NOTYPE      0
#define STT_FUNC        2
#define ST_TYPE(info)   ((info) & 0xF)
#define ST_BIND(info)   ((info) >> 4)
#define STB_LOCAL       0
#define STB_GLOBAL      1

/* ====================================================================
 * Loaded image
 * ==================================================================== */

typedef struct {
    uint8_t *Bytes;
    size_t   Size;
    const char *Path;
} Image;

typedef struct {
    uint64_t    Address;
    uint64_t    Length;   /* 0 when the symbol carries no size */
    const char *Name;
    int         Typed;    /* STT_FUNC (1) beats a bare label (0) */
    int         Global;
} Symbol;

static void Fail(const char *What, const char *Detail)
{
    fprintf(stderr, "nameplate: %s%s%s\n", What,
            Detail ? ": " : "", Detail ? Detail : "");
    exit(1);
}

static int HostIsLittleEndian(void)
{
    const uint16_t One = 1;
    return *(const uint8_t *)&One == 1;
}

static void ImageLoad(Image *Img, const char *Path)
{
    FILE *F = fopen(Path, "rb");
    long   End;
    size_t Got;

    if (!F)
        Fail("cannot open", Path);
    if (fseek(F, 0, SEEK_END) != 0)
        Fail("cannot seek", Path);
    End = ftell(F);
    if (End < 0)
        Fail("cannot size", Path);
    if (fseek(F, 0, SEEK_SET) != 0)
        Fail("cannot rewind", Path);

    Img->Size  = (size_t)End;
    Img->Path  = Path;
    Img->Bytes = malloc(Img->Size ? Img->Size : 1);
    if (!Img->Bytes)
        Fail("out of memory reading", Path);

    Got = fread(Img->Bytes, 1, Img->Size, F);
    fclose(F);
    if (Got != Img->Size)
        Fail("short read of", Path);

    if (Img->Size < sizeof(Elf64Ehdr))
        Fail("not an ELF file", Path);
}

static const Elf64Ehdr *ImageHeader(const Image *Img)
{
    const Elf64Ehdr *Ehdr = (const Elf64Ehdr *)Img->Bytes;

    if (Ehdr->e_ident[0] != 0x7F || Ehdr->e_ident[1] != 'E' ||
        Ehdr->e_ident[2] != 'L'  || Ehdr->e_ident[3] != 'F')
        Fail("not an ELF file", Img->Path);
    if (Ehdr->e_ident[4] != ELFCLASS64)
        Fail("not ELF64", Img->Path);
    if (Ehdr->e_ident[5] != ELFDATA2LSB)
        Fail("not little-endian", Img->Path);
    if (Ehdr->e_shoff == 0 || Ehdr->e_shnum == 0)
        Fail("no section headers", Img->Path);
    if (Ehdr->e_shentsize != sizeof(Elf64Shdr))
        Fail("unexpected section header size", Img->Path);
    if (Ehdr->e_shoff + (uint64_t)Ehdr->e_shnum * sizeof(Elf64Shdr) > Img->Size)
        Fail("section headers past end of file", Img->Path);

    return Ehdr;
}

static const Elf64Shdr *ImageSections(const Image *Img)
{
    return (const Elf64Shdr *)(Img->Bytes + ImageHeader(Img)->e_shoff);
}

static void SectionCheck(const Image *Img, const Elf64Shdr *Shdr)
{
    /* NOBITS occupies no file space; everything else must be inside. */
    if (Shdr->sh_type == 8 /* SHT_NOBITS */)
        return;
    if (Shdr->sh_offset > Img->Size || Shdr->sh_size > Img->Size - Shdr->sh_offset)
        Fail("section extends past end of file", Img->Path);
}

static const char *SectionName(const Image *Img, const Elf64Shdr *Shdr)
{
    const Elf64Ehdr *Ehdr = ImageHeader(Img);
    const Elf64Shdr *Strs;

    if (Ehdr->e_shstrndx >= Ehdr->e_shnum)
        return "";
    Strs = &ImageSections(Img)[Ehdr->e_shstrndx];
    SectionCheck(Img, Strs);
    if (Shdr->sh_name >= Strs->sh_size)
        return "";
    return (const char *)(Img->Bytes + Strs->sh_offset + Shdr->sh_name);
}

static const Elf64Shdr *SectionByName(const Image *Img, const char *Want)
{
    const Elf64Ehdr *Ehdr = ImageHeader(Img);
    const Elf64Shdr *Sections = ImageSections(Img);
    uint16_t i;

    for (i = 0; i < Ehdr->e_shnum; i++) {
        if (strcmp(SectionName(Img, &Sections[i]), Want) == 0) {
            SectionCheck(Img, &Sections[i]);
            return &Sections[i];
        }
    }
    return NULL;
}

/* ====================================================================
 * Symbol harvest
 * ==================================================================== */

static int SymbolCompare(const void *A, const void *B)
{
    const Symbol *L = (const Symbol *)A;
    const Symbol *R = (const Symbol *)B;

    if (L->Address != R->Address)
        return L->Address < R->Address ? -1 : 1;
    /* Same address: the winner is the one that describes it best, and the
     * order must not depend on the link's symbol order — a table that
     * reshuffles between two identical builds is a table nobody can diff. */
    if (L->Typed != R->Typed)
        return R->Typed - L->Typed;
    if ((L->Length != 0) != (R->Length != 0))
        return (R->Length != 0) - (L->Length != 0);
    if (L->Global != R->Global)
        return R->Global - L->Global;
    return strcmp(L->Name, R->Name);
}

/*
 * Every symbol that can legitimately appear in a return address: functions,
 * plus untyped labels that live in an executable section. The second kind is
 * not pedantry — hand-written assembly (boxlib_start, the context switchers)
 * has no .type directive, and a backtrace that stops naming things the moment
 * it reaches assembly is a backtrace that fails exactly where it is needed.
 */
static size_t SymbolsHarvest(const Image *Img, Symbol **Out)
{
    const Elf64Ehdr *Ehdr = ImageHeader(Img);
    const Elf64Shdr *Sections = ImageSections(Img);
    const Elf64Shdr *Symtab = NULL;
    const Elf64Shdr *Strtab;
    const Elf64Sym  *Syms;
    const char      *Strs;
    size_t Count, i, Kept = 0, Unique = 0;
    Symbol *List;

    for (i = 0; i < Ehdr->e_shnum; i++) {
        if (Sections[i].sh_type == SHT_SYMTAB) {
            Symtab = &Sections[i];
            break;
        }
    }
    if (!Symtab)
        Fail("no .symtab — the image was stripped before its Nameplate was built",
             Img->Path);
    SectionCheck(Img, Symtab);
    if (Symtab->sh_entsize != sizeof(Elf64Sym))
        Fail("unexpected symbol size", Img->Path);
    if (Symtab->sh_link >= Ehdr->e_shnum)
        Fail("symtab links to no string table", Img->Path);

    Strtab = &Sections[Symtab->sh_link];
    SectionCheck(Img, Strtab);
    Syms  = (const Elf64Sym *)(Img->Bytes + Symtab->sh_offset);
    Strs  = (const char *)(Img->Bytes + Strtab->sh_offset);
    Count = (size_t)(Symtab->sh_size / sizeof(Elf64Sym));

    List = calloc(Count ? Count : 1, sizeof(Symbol));
    if (!List)
        Fail("out of memory harvesting symbols", Img->Path);

    for (i = 0; i < Count; i++) {
        const Elf64Sym *Sym = &Syms[i];
        unsigned Type = ST_TYPE(Sym->st_info);
        const char *Name;
        int Executable;

        if (Sym->st_shndx == SHN_UNDEF || Sym->st_shndx == SHN_ABS ||
            Sym->st_shndx == SHN_COMMON || Sym->st_shndx >= Ehdr->e_shnum)
            continue;
        if (Sym->st_value == 0)
            continue;
        if (Sym->st_name >= Strtab->sh_size)
            continue;

        Name = Strs + Sym->st_name;
        if (Name[0] == '\0')
            continue;

        Executable = (Sections[Sym->st_shndx].sh_flags & SHF_EXECINSTR) != 0;
        if (Type != STT_FUNC && !(Type == STT_NOTYPE && Executable))
            continue;
        if (!Executable)
            continue;

        /* The address must actually lie inside the section the symbol claims.
         *
         * A linker script computes addresses as well as placing code, and an
         * assignment like `_kernel_phys_start = . - KERNEL_VMA;` inherits
         * whatever output section is current — so it arrives here untyped, in
         * an executable section, holding a number that is nowhere near it. The
         * BoxOS kernel has two such symbols, sitting at 0x100000 while .text
         * begins at 0xffffffff80100000, and admitting them made the table
         * "span more than 4 GiB of code" for an image whose code is 386 KB.
         *
         * A nameplate names code. A symbol whose address is outside its own
         * section is not a code address; it is arithmetic that happened to be
         * written down in the middle of one. */
        {
            const Elf64Shdr *Sec = &Sections[Sym->st_shndx];
            if (Sym->st_value < Sec->sh_addr ||
                Sym->st_value > Sec->sh_addr + Sec->sh_size)
                continue;
        }

        List[Kept].Address = Sym->st_value;
        List[Kept].Length  = Sym->st_size;
        List[Kept].Name    = Name;
        List[Kept].Typed   = (Type == STT_FUNC);
        List[Kept].Global  = (ST_BIND(Sym->st_info) != STB_LOCAL);
        Kept++;
    }

    if (Kept == 0)
        Fail("no function symbols at all", Img->Path);

    qsort(List, Kept, sizeof(Symbol), SymbolCompare);

    /* One name per address: the sort already put the best candidate first. */
    for (i = 0; i < Kept; i++) {
        if (Unique > 0 && List[Unique - 1].Address == List[i].Address)
            continue;
        List[Unique++] = List[i];
    }

    *Out = List;
    return Unique;
}

/* ====================================================================
 * Table build
 * ==================================================================== */

typedef struct {
    uint8_t *Bytes;
    uint64_t Size;
} Blob;

static Blob TableBuild(const Symbol *Syms, size_t Count)
{
    NameplateHeader Header;
    uint64_t NameBytes = 0, Total;
    uint32_t *Offsets, *Sizes, *NameOffsets;
    char     *Names;
    size_t i;
    uint64_t Cursor = 0;
    Blob Out;

    if (Count > NAMEPLATE_MAX_ENTRIES)
        Fail("more symbols than the format admits", NULL);

    for (i = 0; i < Count; i++)
        NameBytes += strlen(Syms[i].Name) + 1;
    if (NameBytes > 0xFFFFFFFFull)
        Fail("name blob exceeds 4 GiB", NULL);

    Total = NAMEPLATE_TOTAL_BYTES(Count, NameBytes);
    Out.Bytes = calloc(1, (size_t)Total);
    Out.Size  = Total;
    if (!Out.Bytes)
        Fail("out of memory building the table", NULL);

    Header.Magic       = NAMEPLATE_MAGIC;
    Header.Version     = NAMEPLATE_VERSION;
    Header.EntryCount  = (uint32_t)Count;
    Header.NameBytes   = (uint32_t)NameBytes;
    Header.BaseAddress = Syms[0].Address;
    Header.TotalBytes  = Total;
    memcpy(Out.Bytes, &Header, sizeof(Header));

    Offsets     = (uint32_t *)(Out.Bytes + NAMEPLATE_OFFSETS_AT(Count));
    Sizes       = (uint32_t *)(Out.Bytes + NAMEPLATE_SIZES_AT(Count));
    NameOffsets = (uint32_t *)(Out.Bytes + NAMEPLATE_NAME_OFFSETS_AT(Count));
    Names       = (char *)(Out.Bytes + NAMEPLATE_NAMES_AT(Count));

    for (i = 0; i < Count; i++) {
        uint64_t Delta = Syms[i].Address - Header.BaseAddress;
        size_t   Len   = strlen(Syms[i].Name) + 1;

        if (Delta > 0xFFFFFFFFull)
            Fail("image spans more than 4 GiB of code", NULL);

        Offsets[i]     = (uint32_t)Delta;
        Sizes[i]       = Syms[i].Length > 0xFFFFFFFFull
                             ? 0xFFFFFFFFu : (uint32_t)Syms[i].Length;
        NameOffsets[i] = (uint32_t)Cursor;
        memcpy(Names + Cursor, Syms[i].Name, Len);
        Cursor += Len;
    }

    if (!NameplateHeaderValid((const NameplateHeader *)Out.Bytes, Out.Size))
        Fail("built a table its own validator rejects", NULL);

    return Out;
}

/* ====================================================================
 * Emit — a relocatable object holding nothing but the table
 * ==================================================================== */

static void ObjectWrite(const char *Path, const Blob *Table)
{
    static const char ShStr[] = "\0.nameplate\0.shstrtab";
    const uint64_t DataOff = sizeof(Elf64Ehdr);
    const uint64_t StrOff  = DataOff + Table->Size;
    const uint64_t ShOff   = (StrOff + sizeof(ShStr) + 7u) & ~7ull;
    Elf64Ehdr Ehdr;
    Elf64Shdr Sections[3];
    FILE *F;
    uint64_t Pad;

    memset(&Ehdr, 0, sizeof(Ehdr));
    Ehdr.e_ident[0] = 0x7F;
    Ehdr.e_ident[1] = 'E';
    Ehdr.e_ident[2] = 'L';
    Ehdr.e_ident[3] = 'F';
    Ehdr.e_ident[4] = ELFCLASS64;
    Ehdr.e_ident[5] = ELFDATA2LSB;
    Ehdr.e_ident[6] = EV_CURRENT;
    Ehdr.e_type      = ET_REL;
    Ehdr.e_machine   = EM_X86_64;
    Ehdr.e_version   = EV_CURRENT;
    Ehdr.e_shoff     = ShOff;
    Ehdr.e_ehsize    = sizeof(Elf64Ehdr);
    Ehdr.e_shentsize = sizeof(Elf64Shdr);
    Ehdr.e_shnum     = 3;
    Ehdr.e_shstrndx  = 2;

    memset(Sections, 0, sizeof(Sections));

    Sections[1].sh_name      = 1;                    /* ".nameplate" */
    Sections[1].sh_type      = SHT_PROGBITS;
    Sections[1].sh_flags     = SHF_ALLOC;            /* read-only, and MAPPED */
    Sections[1].sh_offset    = DataOff;
    Sections[1].sh_size      = Table->Size;
    Sections[1].sh_addralign = 8;

    Sections[2].sh_name      = 12;                   /* ".shstrtab" */
    Sections[2].sh_type      = SHT_STRTAB;
    Sections[2].sh_offset    = StrOff;
    Sections[2].sh_size      = sizeof(ShStr);
    Sections[2].sh_addralign = 1;

    F = fopen(Path, "wb");
    if (!F)
        Fail("cannot create", Path);
    if (fwrite(&Ehdr, sizeof(Ehdr), 1, F) != 1 ||
        fwrite(Table->Bytes, 1, (size_t)Table->Size, F) != Table->Size ||
        fwrite(ShStr, 1, sizeof(ShStr), F) != sizeof(ShStr))
        Fail("short write to", Path);

    for (Pad = StrOff + sizeof(ShStr); Pad < ShOff; Pad++)
        if (fputc(0, F) == EOF)
            Fail("short write to", Path);

    if (fwrite(Sections, sizeof(Elf64Shdr), 3, F) != 3)
        Fail("short write to", Path);
    if (fclose(F) != 0)
        Fail("cannot close", Path);
}

/* ====================================================================
 * Read back — the lookup, host side
 * ==================================================================== */

typedef struct {
    const NameplateHeader *Header;
    const uint32_t *Offsets;
    const uint32_t *Sizes;
    const uint32_t *NameOffsets;
    const char     *Names;
} Table;

static Table TableOpen(const Image *Img)
{
    const Elf64Shdr *Section = SectionByName(Img, NAMEPLATE_SECTION_NAME);
    const uint8_t *Base;
    uint32_t Count;
    Table T;

    if (!Section)
        Fail("no " NAMEPLATE_SECTION_NAME " section", Img->Path);

    Base = Img->Bytes + Section->sh_offset;
    if (Section->sh_size < sizeof(NameplateHeader))
        Fail("truncated Nameplate", Img->Path);
    if (!NameplateHeaderValid((const NameplateHeader *)Base, Section->sh_size))
        Fail("corrupt Nameplate", Img->Path);

    T.Header      = (const NameplateHeader *)Base;
    Count         = T.Header->EntryCount;
    T.Offsets     = (const uint32_t *)(Base + NAMEPLATE_OFFSETS_AT(Count));
    T.Sizes       = (const uint32_t *)(Base + NAMEPLATE_SIZES_AT(Count));
    T.NameOffsets = (const uint32_t *)(Base + NAMEPLATE_NAME_OFFSETS_AT(Count));
    T.Names       = (const char *)(Base + NAMEPLATE_NAMES_AT(Count));
    return T;
}

/* ====================================================================
 * Commands
 * ==================================================================== */

static int CommandBuild(const char *InPath, const char *OutPath)
{
    Image Img;
    Symbol *Syms = NULL;
    size_t Count;
    Blob Table;

    ImageLoad(&Img, InPath);
    Count = SymbolsHarvest(&Img, &Syms);
    Table = TableBuild(Syms, Count);
    ObjectWrite(OutPath, &Table);

    printf("nameplate: %s -> %s (%zu names, %llu bytes)\n",
           InPath, OutPath, Count, (unsigned long long)Table.Size);

    free(Table.Bytes);
    free(Syms);
    free(Img.Bytes);
    return 0;
}

/*
 * The guarantee of the two-pass link, checked rather than assumed: every name
 * the shipped table carries must still sit at the address the table claims,
 * according to the shipped binary's own symbol table.
 */
static int CommandVerify(const char *Path)
{
    Image Img;
    Symbol *Syms = NULL;
    size_t Count, i;
    Table T;
    uint32_t j;

    ImageLoad(&Img, Path);
    T     = TableOpen(&Img);
    Count = SymbolsHarvest(&Img, &Syms);

    if (Count != T.Header->EntryCount) {
        fprintf(stderr,
                "nameplate: %s carries %u names but its symbols say %zu — the "
                "two link passes disagree\n",
                Path, T.Header->EntryCount, Count);
        return 1;
    }

    for (i = 0; i < Count; i++) {
        uint64_t Claimed = T.Header->BaseAddress + T.Offsets[i];
        const char *Name;

        if (T.NameOffsets[i] >= T.Header->NameBytes)
            Fail("name offset outside the blob", Path);
        Name = T.Names + T.NameOffsets[i];

        if (Claimed != Syms[i].Address || strcmp(Name, Syms[i].Name) != 0) {
            fprintf(stderr,
                    "nameplate: %s entry %zu says %s@0x%llx, the binary says "
                    "%s@0x%llx — a function moved between link passes\n",
                    Path, i, Name, (unsigned long long)Claimed,
                    Syms[i].Name, (unsigned long long)Syms[i].Address);
            return 1;
        }
    }

    /* Ascending and unique, which is what the runtime binary search assumes
     * and what no reader can afford to re-check on a crash path. */
    for (j = 1; j < T.Header->EntryCount; j++) {
        if (T.Offsets[j] <= T.Offsets[j - 1]) {
            fprintf(stderr, "nameplate: %s offsets are not ascending at %u\n",
                    Path, j);
            return 1;
        }
    }
    if (T.Header->NameBytes == 0 || T.Names[T.Header->NameBytes - 1] != '\0') {
        fprintf(stderr, "nameplate: %s name blob does not end in NUL\n", Path);
        return 1;
    }

    free(Syms);
    free(Img.Bytes);
    return 0;
}

static int CommandList(const char *Path, const char *AddrText)
{
    Image Img;
    Table T;
    uint32_t i;

    ImageLoad(&Img, Path);
    T = TableOpen(&Img);

    if (AddrText) {
        uint64_t Want = strtoull(AddrText, NULL, 0);
        uint64_t Base = T.Header->BaseAddress;
        uint32_t Low = 0, High = T.Header->EntryCount, Hit;

        if (Want < Base) {
            printf("0x%llx: not in this image\n", (unsigned long long)Want);
            return 0;
        }
        while (High - Low > 1) {
            uint32_t Mid = Low + (High - Low) / 2;
            if (Base + T.Offsets[Mid] <= Want)
                Low = Mid;
            else
                High = Mid;
        }
        Hit = Low;
        {
            uint64_t Start = Base + T.Offsets[Hit];
            uint64_t Delta = Want - Start;

            if (T.Sizes[Hit] != 0 && Delta >= T.Sizes[Hit])
                printf("0x%llx: past the end of %s\n", (unsigned long long)Want,
                       T.Names + T.NameOffsets[Hit]);
            else if (Delta == 0)
                printf("0x%llx: %s\n", (unsigned long long)Want,
                       T.Names + T.NameOffsets[Hit]);
            else
                printf("0x%llx: %s+0x%llx\n", (unsigned long long)Want,
                       T.Names + T.NameOffsets[Hit], (unsigned long long)Delta);
        }
        free(Img.Bytes);
        return 0;
    }

    printf("# %s — %u names, base 0x%llx, %llu bytes\n", Path,
           T.Header->EntryCount, (unsigned long long)T.Header->BaseAddress,
           (unsigned long long)T.Header->TotalBytes);
    for (i = 0; i < T.Header->EntryCount; i++)
        printf("0x%llx %u %s\n",
               (unsigned long long)(T.Header->BaseAddress + T.Offsets[i]),
               T.Sizes[i], T.Names + T.NameOffsets[i]);

    free(Img.Bytes);
    return 0;
}

int main(int argc, char *argv[])
{
    if (!HostIsLittleEndian())
        Fail("this tool writes little-endian ELF and the host is not", NULL);

    if (argc >= 4 && strcmp(argv[1], "build") == 0)
        return CommandBuild(argv[2], argv[3]);
    if (argc >= 3 && strcmp(argv[1], "verify") == 0)
        return CommandVerify(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "list") == 0)
        return CommandList(argv[2], argc >= 4 ? argv[3] : NULL);

    fprintf(stderr,
            "Usage: %s build  <pass1.elf> <out.o>\n"
            "       %s verify <final.elf>\n"
            "       %s list   <elf> [addr]\n",
            argv[0], argv[0], argv[0]);
    return 2;
}
