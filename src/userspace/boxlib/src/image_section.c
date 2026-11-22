/*
 * image_section.c — one named section of a program's image, read at offsets.
 *
 * The image is untrusted bytes: every offset and size is held inside the file
 * before it is read, and a name is matched only inside the name table.
 */

#include "box/core/image_section.h"
#include "box/file.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define IMAGE_HEADER_BATCH 16u

typedef struct {
    uint8_t  Ident[16];
    uint16_t Type;
    uint16_t Machine;
    uint32_t Version;
    uint64_t Entry;
    uint64_t ProgramHeaders;
    uint64_t SectionHeaders;
    uint32_t Flags;
    uint16_t HeaderSize;
    uint16_t ProgramHeaderSize;
    uint16_t ProgramHeaderCount;
    uint16_t SectionHeaderSize;
    uint16_t SectionHeaderCount;
    uint16_t NameSection;
} ImageElfHeader;

typedef struct {
    uint32_t Name;
    uint32_t Type;
    uint64_t Flags;
    uint64_t Address;
    uint64_t Offset;
    uint64_t Size;
    uint32_t Link;
    uint32_t Info;
    uint64_t Align;
    uint64_t EntrySize;
} ImageElfSection;

_Static_assert(sizeof(ImageElfHeader) == 64, "an ELF64 header is 64 bytes");
_Static_assert(sizeof(ImageElfSection) == 64, "an ELF64 section header is 64 bytes");

#define IMAGE_ELF_CLASS_64   2u
#define IMAGE_ELF_LITTLE     1u
#define IMAGE_SECTION_NOBITS 8u

static int read_exact(file_id_t file, uint64_t offset, void *buffer, size_t size)
{
    int64_t got = fread(file, offset, buffer, size);
    if (got < 0) return (int)got;
    return (size_t)got == size ? 0 : -ERR_CORRUPTED;
}

static bool span_inside(uint64_t offset, uint64_t size, uint64_t whole)
{
    return offset <= whole && size <= whole - offset;
}

static bool name_at(const char *names, uint64_t names_size, uint32_t at, const char *name)
{
    size_t want = strlen(name) + 1;
    if (at >= names_size || names_size - at < want) return false;
    return memcmp(names + at, name, want) == 0;
}

/* The section headers, a few at a time, until the one called Name. */
static int find_section(file_id_t file, const ImageElfHeader *header, const char *names,
                        uint64_t names_size, const char *name, ImageElfSection *found)
{
    for (uint32_t first = 0; first < header->SectionHeaderCount; first += IMAGE_HEADER_BATCH) {
        ImageElfSection batch[IMAGE_HEADER_BATCH];
        uint32_t take = header->SectionHeaderCount - first;
        if (take > IMAGE_HEADER_BATCH) take = IMAGE_HEADER_BATCH;

        int rc = read_exact(file, header->SectionHeaders + (uint64_t)first * sizeof(ImageElfSection),
                            batch, take * sizeof(ImageElfSection));
        if (rc != 0) return rc;
        for (uint32_t i = 0; i < take; i++) {
            if (!name_at(names, names_size, batch[i].Name, name)) continue;
            *found = batch[i];
            return 1;
        }
    }
    return 0;
}

/* A section's bytes, a NUL after them; NULL for an empty one. */
static int read_bytes(file_id_t file, const ImageElfSection *section, uint64_t file_size,
                      uint64_t limit, char **bytes)
{
    if (section->Type == IMAGE_SECTION_NOBITS || section->Size > limit ||
        !span_inside(section->Offset, section->Size, file_size))
        return -ERR_CORRUPTED;
    *bytes = NULL;
    if (section->Size == 0) return 0;

    char *copy = malloc((size_t)section->Size + 1);
    if (!copy) return -ERR_NO_MEMORY;
    int rc = read_exact(file, section->Offset, copy, (size_t)section->Size);
    if (rc != 0) {
        free(copy);
        return rc;
    }
    copy[section->Size] = '\0';
    *bytes = copy;
    return 0;
}

static int read_names(file_id_t file, const ImageElfHeader *header, uint64_t file_size,
                      char **names, uint64_t *names_size)
{
    ImageElfSection section;
    int rc = read_exact(file, header->SectionHeaders +
                              (uint64_t)header->NameSection * sizeof(ImageElfSection),
                        &section, sizeof(section));
    if (rc != 0) return rc;
    rc = read_bytes(file, &section, file_size, file_size, names);
    *names_size = section.Size;
    return rc;
}

static int headers_usable(const ImageElfHeader *header, uint64_t file_size)
{
    if (header->SectionHeaders == 0 || header->SectionHeaderCount == 0) return 0;
    if (header->SectionHeaderSize != sizeof(ImageElfSection)) return -ERR_CORRUPTED;
    if (header->NameSection >= header->SectionHeaderCount) return -ERR_CORRUPTED;
    uint64_t table = (uint64_t)header->SectionHeaderCount * sizeof(ImageElfSection);
    if (!span_inside(header->SectionHeaders, table, file_size)) return -ERR_CORRUPTED;
    return 1;
}

int image_section_read(file_id_t file, const char *name, size_t limit, char **bytes,
                       size_t *size, bool *image)
{
    bool is_image = false;
    if (image) *image = false;
    if (!name || !bytes || !size) return -ERR_INVALID_ARGUMENT;
    *bytes = NULL;
    *size  = 0;

    FileMetadata metadata;
    int rc = FileMetadataRead(MY_TAGS, file, &metadata);
    if (rc != 0) return rc;
    uint64_t file_size = metadata.Size;
    FileMetadataRelease(&metadata);

    ImageElfHeader header;
    if (file_size < sizeof(header)) return 0;
    rc = read_exact(file, 0, &header, sizeof(header));
    if (rc != 0) return rc;
    is_image = memcmp(header.Ident, "\x7f" "ELF", 4) == 0 && header.Ident[4] == IMAGE_ELF_CLASS_64 &&
               header.Ident[5] == IMAGE_ELF_LITTLE;
    if (image) *image = is_image;
    if (!is_image) return 0;

    rc = headers_usable(&header, file_size);
    if (rc <= 0) return rc;

    char    *names = NULL;
    uint64_t names_size = 0;
    rc = read_names(file, &header, file_size, &names, &names_size);
    if (rc != 0) return rc;

    ImageElfSection section = { 0 };
    rc = names ? find_section(file, &header, names, names_size, name, &section) : 0;
    free(names);
    if (rc <= 0) return rc;
    rc = read_bytes(file, &section, file_size, limit, bytes);
    if (rc != 0) return rc;
    *size = (size_t)section.Size;
    return 1;
}
