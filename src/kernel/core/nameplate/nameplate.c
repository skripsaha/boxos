/*
 * nameplate.c — the kernel half of Nameplate.
 *
 * See nameplate.h for what the two entry points are for, and
 * src/include/nameplate_format.h for the table itself.
 */

#include "nameplate.h"

#include "klib.h"
#include "uaccess.h"

#include "nameplate_format.h"

/* ELF64, only the parts the section-header walk touches. vmm.c keeps its own
 * copy of the header and the PROGRAM headers for the loading side; these two
 * are the section-header side and are needed nowhere else. */
typedef struct
{
    uint8_t  e_ident[16];
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
} __attribute__((packed)) NpElfEhdr;

typedef struct
{
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
} __attribute__((packed)) NpElfShdr;

/* ── locate ───────────────────────────────────────────────────────────── */

static int SectionSpanOk(const NpElfShdr *sh, uint64_t bytes)
{
    if (sh->sh_type == 8 /* SHT_NOBITS */) return 0;
    if (sh->sh_offset > bytes) return 0;
    return sh->sh_size <= bytes - sh->sh_offset;
}

static int LocateInElf(const uint8_t *image, uint64_t bytes, uintptr_t *out_va,
                       uint64_t *out_bytes)
{
    const NpElfEhdr *ehdr = (const NpElfEhdr *)image;
    const NpElfShdr *sections, *names;
    const char      *strings;
    uint64_t         table_end;
    uint16_t         i;

    if (bytes < sizeof(NpElfEhdr)) return 0;
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) return 0;
    if (ehdr->e_shentsize != sizeof(NpElfShdr)) return 0;
    if (ehdr->e_shstrndx >= ehdr->e_shnum) return 0;

    table_end = ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(NpElfShdr);
    if (table_end < ehdr->e_shoff || table_end > bytes) return 0;

    sections = (const NpElfShdr *)(image + ehdr->e_shoff);
    names    = &sections[ehdr->e_shstrndx];
    if (!SectionSpanOk(names, bytes)) return 0;
    strings = (const char *)(image + names->sh_offset);

    for (i = 0; i < ehdr->e_shnum; i++) {
        const NpElfShdr       *sh = &sections[i];
        const NameplateHeader *header;
        const char            *name;
        uint64_t               left;

        if (sh->sh_name >= names->sh_size) continue;
        name = strings + sh->sh_name;
        left = names->sh_size - sh->sh_name;
        if (strnlen(name, left) >= left) continue;   /* unterminated */
        if (strcmp(name, NAMEPLATE_SECTION_NAME) != 0) continue;

        if (sh->sh_addr == 0) return 0;                   /* never mapped */
        if (!SectionSpanOk(sh, bytes)) return 0;
        if (sh->sh_size < sizeof(NameplateHeader)) return 0;

        header = (const NameplateHeader *)(image + sh->sh_offset);
        if (!NameplateHeaderValid(header, sh->sh_size)) return 0;

        *out_va    = (uintptr_t)sh->sh_addr;
        *out_bytes = header->TotalBytes;
        return 1;
    }
    return 0;
}

static int LocateInFlat(const uint8_t *image, uint64_t bytes, uintptr_t load_base,
                        uintptr_t *out_va, uint64_t *out_bytes)
{
    uint64_t off;

    if (bytes < sizeof(NameplateHeader)) return 0;

    /* The table is eight-byte aligned in the image and a flat binary is the
     * image, byte for byte, from its load address — so stepping by eight
     * cannot step over it. */
    for (off = 0; off + sizeof(NameplateHeader) <= bytes; off += 8) {
        const NameplateHeader *header = (const NameplateHeader *)(image + off);

        if (header->Magic != NAMEPLATE_MAGIC) continue;
        if (!NameplateHeaderValid(header, bytes - off)) continue;

        *out_va    = load_base + (uintptr_t)off;
        *out_bytes = header->TotalBytes;
        return 1;
    }
    return 0;
}

int nameplate_locate(const void *image, uint64_t bytes, uintptr_t load_base,
                     uintptr_t *out_va, uint64_t *out_bytes)
{
    const uint8_t *bytes_in = (const uint8_t *)image;

    if (!image || !out_va || !out_bytes || bytes < sizeof(NameplateHeader))
        return 0;

    if (bytes >= 4 && bytes_in[0] == 0x7F && bytes_in[1] == 'E' &&
        bytes_in[2] == 'L' && bytes_in[3] == 'F')
        return LocateInElf(bytes_in, bytes, out_va, out_bytes);

    return LocateInFlat(bytes_in, bytes, load_base, out_va, out_bytes);
}

/* ── lookup, across the address-space boundary ────────────────────────── */

/* Every read below goes through one of these, chosen by the caller. The table
 * being searched is sometimes in a faulted process's address space and
 * sometimes in the kernel's own image, and those are not the same memory —
 * get_user_u32 exists precisely to refuse a kernel address. One search, two
 * ways of touching the bytes; the alternative is a second copy of a binary
 * search, which is a second place for it to be wrong.
 *
 * get_user_u32 stays the user-side primitive: it is the one the fault dump
 * already used before Nameplate existed, and a diagnostic is a bad place to
 * introduce a mechanism that has never run in exception context. */
typedef int (*NameplateReadWord)(uintptr_t at, uint32_t *out);

static int ReadWordUser(uintptr_t at, uint32_t *out)
{
    if (at & 3u) return 0;                       /* every field is aligned */
    return get_user_u32(out, (const uint32_t *)at) == 0;
}

/* The kernel's own table lives inside the kernel image, which is mapped for
 * as long as there is a kernel at all. A plain load is correct here and a
 * get_user would simply refuse the address. */
static int ReadWordKernel(uintptr_t at, uint32_t *out)
{
    if (at & 3u) return 0;
    *out = *(const volatile uint32_t *)at;
    return 1;
}

static int ReadHeader(NameplateReadWord Read, uintptr_t table_va,
                      uint64_t table_bytes, NameplateHeader *out)
{
    uint32_t lo, hi;

    if (table_va & 7u) return 0;

    if (!Read(table_va + 0, &out->Magic)) return 0;
    if (out->Magic != NAMEPLATE_MAGIC) return 0;
    if (!Read(table_va + 4, &out->Version)) return 0;
    if (!Read(table_va + 8, &out->EntryCount)) return 0;
    if (!Read(table_va + 12, &out->NameBytes)) return 0;

    if (!Read(table_va + 16, &lo) || !Read(table_va + 20, &hi)) return 0;
    out->BaseAddress = ((uint64_t)hi << 32) | lo;
    if (!Read(table_va + 24, &lo) || !Read(table_va + 28, &hi)) return 0;
    out->TotalBytes = ((uint64_t)hi << 32) | lo;

    return NameplateHeaderValid(out, table_bytes);
}

static int NameAt(NameplateReadWord Read, uintptr_t table_va,
                  uint64_t table_bytes, uintptr_t addr, NameplateName *out)
{
    NameplateHeader header;
    uintptr_t       offsets, sizes, name_offsets, names;
    uint32_t        low, high, hit, want, start_off, span, name_off;
    uint64_t        start, delta;

    if (!out || table_va == 0 || table_bytes < sizeof(NameplateHeader)) return 0;
    if (!ReadHeader(Read, table_va, table_bytes, &header)) return 0;

    if ((uint64_t)addr < header.BaseAddress) return 0;
    if ((uint64_t)addr - header.BaseAddress > 0xFFFFFFFFull) return 0;
    want = (uint32_t)((uint64_t)addr - header.BaseAddress);

    offsets      = table_va + NAMEPLATE_OFFSETS_AT(header.EntryCount);
    sizes        = table_va + NAMEPLATE_SIZES_AT(header.EntryCount);
    name_offsets = table_va + NAMEPLATE_NAME_OFFSETS_AT(header.EntryCount);
    names        = table_va + NAMEPLATE_NAMES_AT(header.EntryCount);

    if (!Read(offsets, &start_off)) return 0;
    if (want < start_off) return 0;

    /* Greatest entry at or below the address — the same search boxlib does
     * in-process, one get_user per probe instead of one load. */
    low  = 0;
    high = header.EntryCount;
    while (high - low > 1) {
        uint32_t mid = low + (high - low) / 2;
        uint32_t probe;

        if (!Read(offsets + (uintptr_t)mid * 4u, &probe)) return 0;
        if (probe <= want)
            low = mid;
        else
            high = mid;
    }
    hit = low;

    if (!Read(offsets + (uintptr_t)hit * 4u, &start_off)) return 0;
    if (!Read(sizes + (uintptr_t)hit * 4u, &span)) return 0;

    start = header.BaseAddress + start_off;
    delta = (uint64_t)addr - start;

    /* Past the end of the nearest function: padding, or code no symbol
     * claims. Say nothing rather than name the neighbour — a dump that
     * confidently names the wrong function costs more than one that admits
     * it does not know. */
    if (span != 0 && delta >= span) return 0;

    if (!Read(name_offsets + (uintptr_t)hit * 4u, &name_off)) return 0;
    if (name_off >= header.NameBytes) return 0;

    /* Copy the name out a word at a time, since the bytes are not aligned and
     * get_user_u32 is the only primitive in play. Stops at the NUL, at the
     * end of the blob, or at the buffer — whichever comes first. */
    {
        uint64_t left = header.NameBytes - name_off;
        uint32_t word = 0;
        uintptr_t at  = names + name_off;
        size_t    n   = 0;
        int       truncated = 0;

        while (n + 1 < NAMEPLATE_NAME_MAX && (uint64_t)n < left) {
            uintptr_t byte_at = at + n;

            if ((byte_at & 3u) == 0 || n == 0) {
                if (!Read(byte_at & ~(uintptr_t)3u, &word)) return 0;
            }
            char c = (char)((word >> (8u * (byte_at & 3u))) & 0xFFu);
            if (c == '\0') break;
            out->Text[n++] = c;
        }
        if (n + 1 >= NAMEPLATE_NAME_MAX) truncated = 1;
        if (truncated && n >= 3) {
            out->Text[n - 3] = '.';
            out->Text[n - 2] = '.';
            out->Text[n - 1] = '.';
        }
        out->Text[n] = '\0';
        if (n == 0) return 0;
    }

    out->Offset = delta;
    return 1;
}

/* ── the two entry points ─────────────────────────────────────────────── */

/* A faulted process's table, read across the address-space boundary. */
int nameplate_name_at(uintptr_t table_va, uint64_t table_bytes, uintptr_t addr,
                      NameplateName *out)
{
    return NameAt(ReadWordUser, table_va, table_bytes, addr, out);
}

/* The kernel's own table, read directly. This is what turns a kernel panic
 * from a column of hexadecimal into a call chain somebody can read off a
 * photograph of a screen — with no matching kernel.elf at the other end of
 * the world, which is exactly the situation a panic on a strange machine
 * puts you in. */
int nameplate_name_at_kernel(uintptr_t table_va, uint64_t table_bytes,
                             uintptr_t addr, NameplateName *out)
{
    return NameAt(ReadWordKernel, table_va, table_bytes, addr, out);
}
