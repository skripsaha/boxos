#include "ktypes.h"
#include "elf_image.h"

/* ELF64 as the System V ABI lays it down; only what a loader reads. */
typedef struct __attribute__((packed)) {
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
} ElfHeader;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} ElfProgramHeader;

#define ELF_CLASS_64        2
#define ELF_DATA_LSB        1
#define ELF_TYPE_EXEC       2
#define ELF_MACHINE_X86_64  62
#define ELF_PN_XNUM         0xFFFFu

#define ELF_PT_LOAD         1u
#define ELF_PT_INTERP       3u
#define ELF_PT_TLS          7u
#define ELF_PT_GNU_STACK    0x6474E551u

#define ELF_PF_X            1u
#define ELF_PF_W            2u

#define ELF_PAGE            4096ull

_Static_assert(sizeof(ElfHeader) == 64, "ELF64 header is 64 bytes");
_Static_assert(sizeof(ElfProgramHeader) == 56, "ELF64 program header is 56 bytes");
_Static_assert(ELF_IMAGE_NO_HEADER == ELF_PN_XNUM, "no header is the count a table never has");

static uint64_t PageDown(uint64_t address)
{
    return address & ~(ELF_PAGE - 1);
}

/* Only for an address inside the zone, which ends well below 2^64. */
static uint64_t PageUp(uint64_t address)
{
    return PageDown(address + ELF_PAGE - 1);
}

static const ElfProgramHeader *HeaderAt(const ElfImage *image, uint16_t index)
{
    return (const ElfProgramHeader *)(image->File + image->HeadersAt +
                                      (uint64_t)index * sizeof(ElfProgramHeader));
}

static void SegmentOf(const ElfProgramHeader *header, ElfImageSegment *out)
{
    out->Virt        = header->p_vaddr;
    out->FileOffset  = header->p_offset;
    out->FileBytes   = header->p_filesz;
    out->MemoryBytes = header->p_memsz;
    out->Writable    = (header->p_flags & ELF_PF_W) != 0;
    out->Executable  = (header->p_flags & ELF_PF_X) != 0;
}

/* The ELF header, up to and including where its program header table is. */
static ElfImageVerdict ReadHeader(const uint8_t *file, uint64_t bytes, ElfImage *out)
{
    const ElfHeader *header = (const ElfHeader *)file;

    if (bytes < 4 || file[0] != 0x7F || file[1] != 'E' || file[2] != 'L' || file[3] != 'F')
        return ELF_IMAGE_NOT_ELF;
    if (bytes < sizeof(ElfHeader))                 return ELF_IMAGE_TRUNCATED;
    if (header->e_ident[4] != ELF_CLASS_64)        return ELF_IMAGE_NOT_64_BIT;
    if (header->e_ident[5] != ELF_DATA_LSB)        return ELF_IMAGE_NOT_LITTLE_ENDIAN;
    if (header->e_type != ELF_TYPE_EXEC)           return ELF_IMAGE_NOT_EXECUTABLE;
    if (header->e_machine != ELF_MACHINE_X86_64)   return ELF_IMAGE_NOT_X86_64;
    if (header->e_phentsize != sizeof(ElfProgramHeader)) return ELF_IMAGE_HEADER_SIZE;
    if (header->e_phnum == 0)                      return ELF_IMAGE_NO_HEADERS;
    if (header->e_phnum == ELF_PN_XNUM)            return ELF_IMAGE_HEADER_COUNT_ELSEWHERE;
    if (header->e_phoff > bytes ||
        (uint64_t)header->e_phnum * sizeof(ElfProgramHeader) > bytes - header->e_phoff)
        return ELF_IMAGE_HEADERS_OUTSIDE_FILE;

    out->Entry       = header->e_entry;
    out->HeadersAt   = header->e_phoff;
    out->HeaderCount = header->e_phnum;
    return ELF_IMAGE_OK;
}

/* The template of thread-local storage, held to the file, its alignment to a
 * power of two no larger than a page — 0 and 1 both mean none, as ELF says —
 * and its size to the zone a block of it must be placed in. */
static ElfImageVerdict ThreadVerdict(const ElfProgramHeader *header, uint64_t file_bytes,
                                     uint64_t zone_bytes, ElfImageThread *out)
{
    uint64_t align = header->p_align > 1 ? header->p_align : 1;
    if (header->p_filesz > header->p_memsz)
        return ELF_IMAGE_SEGMENT_FILE_OVER_MEMORY;
    if (header->p_offset > file_bytes || header->p_filesz > file_bytes - header->p_offset)
        return ELF_IMAGE_SEGMENT_OUTSIDE_FILE;
    if ((align & (align - 1)) != 0 || align > ELF_PAGE)
        return ELF_IMAGE_THREAD_ALIGN;
    if (header->p_memsz > zone_bytes)
        return ELF_IMAGE_THREAD_TOO_LARGE;
    out->Align      = align;
    out->FileOffset = header->p_offset;
    out->InitBytes  = header->p_filesz;
    out->TotalBytes = header->p_memsz;
    return ELF_IMAGE_OK;
}

/* One segment to load, held to the file and the zone — each side asked as a
 * subtraction from the limit, once the start is known to be under it. */
static ElfImageVerdict SegmentVerdict(const ElfImageSegment *segment, uint64_t file_bytes,
                                      uint64_t zone_start, uint64_t zone_end)
{
    if (segment->FileBytes > segment->MemoryBytes)
        return ELF_IMAGE_SEGMENT_FILE_OVER_MEMORY;
    if (segment->FileOffset > file_bytes ||
        segment->FileBytes > file_bytes - segment->FileOffset)
        return ELF_IMAGE_SEGMENT_OUTSIDE_FILE;
    if (segment->Virt < zone_start || segment->Virt >= zone_end ||
        segment->MemoryBytes > zone_end - segment->Virt)
        return ELF_IMAGE_SEGMENT_OUTSIDE_ZONE;
    if (segment->Writable && segment->Executable)
        return ELF_IMAGE_SEGMENT_WRITABLE_EXECUTABLE;
    return ELF_IMAGE_OK;
}

ElfImageVerdict ElfImageRead(const void *file, uint64_t bytes, uint64_t zone_start,
                             uint64_t zone_end, ElfImage *out)
{
    out->File        = (const uint8_t *)file;
    out->FileBytes   = bytes;
    out->Entry       = 0;
    out->HeadersAt   = 0;
    out->End         = 0;
    out->Thread      = (ElfImageThread){ 0 };
    out->HeaderCount = 0;
    out->Header      = ELF_IMAGE_NO_HEADER;

    ElfImageVerdict verdict = ReadHeader(out->File, bytes, out);
    if (verdict != ELF_IMAGE_OK)
        return verdict;

    /* The lowest page the next segment may begin on: a page holds one segment
     * and one set of rights, and the table lists segments by address. */
    uint64_t free_from     = zone_start;
    bool     loads         = false;
    bool     entry_in_code = false;
    bool     thread        = false;

    for (uint16_t i = 0; i < out->HeaderCount; i++) {
        const ElfProgramHeader *header = HeaderAt(out, i);
        ElfImageSegment         segment;

        out->Header = i;
        if (header->p_type == ELF_PT_INTERP)
            return ELF_IMAGE_INTERPRETER;
        if (header->p_type == ELF_PT_GNU_STACK && (header->p_flags & ELF_PF_X))
            return ELF_IMAGE_STACK_EXECUTABLE;
        if (header->p_type == ELF_PT_TLS) {
            if (thread)
                return ELF_IMAGE_THREAD_TWICE;
            verdict = ThreadVerdict(header, bytes, zone_end - zone_start, &out->Thread);
            if (verdict != ELF_IMAGE_OK)
                return verdict;
            thread = true;
            continue;
        }
        if (header->p_type != ELF_PT_LOAD)
            continue;

        SegmentOf(header, &segment);
        if (segment.MemoryBytes == 0 && segment.FileBytes == 0)
            continue;
        verdict = SegmentVerdict(&segment, bytes, zone_start, zone_end);
        if (verdict != ELF_IMAGE_OK)
            return verdict;
        if (PageDown(segment.Virt) < free_from)
            return ELF_IMAGE_SEGMENT_OUT_OF_ORDER;
        free_from = PageUp(segment.Virt + segment.MemoryBytes);
        loads     = true;

        if (segment.Executable && out->Entry >= segment.Virt &&
            out->Entry - segment.Virt < segment.FileBytes)
            entry_in_code = true;
    }

    out->Header = ELF_IMAGE_NO_HEADER;
    out->End    = free_from;
    if (!loads)
        return ELF_IMAGE_NOTHING_TO_LOAD;
    if (!entry_in_code)
        return ELF_IMAGE_ENTRY_OUTSIDE_CODE;
    return ELF_IMAGE_OK;
}

bool ElfImageNextSegment(const ElfImage *image, uint16_t *at, ElfImageSegment *out)
{
    for (uint16_t i = *at; i < image->HeaderCount; i++) {
        const ElfProgramHeader *header = HeaderAt(image, i);

        if (header->p_type != ELF_PT_LOAD || header->p_memsz == 0)
            continue;
        SegmentOf(header, out);
        *at = (uint16_t)(i + 1);
        return true;
    }
    *at = image->HeaderCount;
    return false;
}

const char *ElfImageVerdictText(ElfImageVerdict verdict)
{
    switch (verdict) {
    case ELF_IMAGE_OK:                return "nothing is wrong with it";
    case ELF_IMAGE_NOT_ELF:           return "it does not begin as an ELF file";
    case ELF_IMAGE_TRUNCATED:         return "it is shorter than an ELF header";
    case ELF_IMAGE_NOT_64_BIT:        return "it is not a 64-bit ELF";
    case ELF_IMAGE_NOT_LITTLE_ENDIAN: return "it is not little-endian";
    case ELF_IMAGE_NOT_EXECUTABLE:    return "it is not an executable (ET_EXEC)";
    case ELF_IMAGE_NOT_X86_64:        return "it is not for x86-64";
    case ELF_IMAGE_HEADER_SIZE:       return "its program headers are not 56 bytes each";
    case ELF_IMAGE_NO_HEADERS:        return "it has no program headers";
    case ELF_IMAGE_HEADER_COUNT_ELSEWHERE:
        return "it keeps the count of its program headers elsewhere (PN_XNUM)";
    case ELF_IMAGE_HEADERS_OUTSIDE_FILE:
        return "its program headers run past the end of the file";
    case ELF_IMAGE_SEGMENT_FILE_OVER_MEMORY:
        return "a segment has more bytes in the file than in memory";
    case ELF_IMAGE_SEGMENT_OUTSIDE_FILE:
        return "a segment's bytes run past the end of the file";
    case ELF_IMAGE_SEGMENT_OUTSIDE_ZONE:
        return "a segment lies outside the room a program's image has";
    case ELF_IMAGE_SEGMENT_OUT_OF_ORDER:
        return "a segment shares a page with the one before it, or lies below it";
    case ELF_IMAGE_SEGMENT_WRITABLE_EXECUTABLE:
        return "a segment is writable and executable at once";
    case ELF_IMAGE_STACK_EXECUTABLE:  return "it asks for an executable stack";
    case ELF_IMAGE_INTERPRETER:
        return "it needs a program interpreter, and this system links nothing at start";
    case ELF_IMAGE_NOTHING_TO_LOAD:   return "it has no segment to load";
    case ELF_IMAGE_ENTRY_OUTSIDE_CODE:
        return "its entry point is not in code the file supplies";
    case ELF_IMAGE_THREAD_TWICE:
        return "it has two templates of thread-local storage";
    case ELF_IMAGE_THREAD_ALIGN:
        return "its thread-local storage asks for an alignment that is not a power of two up to a page";
    case ELF_IMAGE_THREAD_TOO_LARGE:
        return "its thread-local storage is larger than the room a program's image has";
    case ELF_IMAGE_VERDICT_COUNT:     break;
    }
    return "it was refused for a reason this kernel has no words for";
}
