/*
 * program_part.c — a program's parts by their names, a box's and an ELF
 * image's alike (box/program_part.h).
 *
 * The file is untrusted bytes. A box is read through the one reader the kernel
 * uses, so a part this library hands over is a part the kernel would have
 * accepted: the table held to the seal before a name in it is believed, the
 * part to its hash before a byte of it is given out.
 */

#include "box/program_part.h"
#include "box/core/image_section.h"
#include "box/core/cabin.h"
#include "box/file.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/sha256.h"
#include "box_reader.h"
#include "box_loaded.h"

static int read_exact(file_id_t file, uint64_t offset, void *buffer, size_t size)
{
    int64_t got = fread(file, offset, buffer, size);
    if (got < 0) return (int)got;
    return (size_t)got == size ? 0 : -ERR_CORRUPTED;
}

/* ---- the box ------------------------------------------------------------ */

static void hash_begin(void *state)
{
    sha256_begin((Sha256 *)state);
}

static void hash_add(void *state, const uint8_t *bytes, uint64_t count)
{
    sha256_add((Sha256 *)state, bytes, (size_t)count);
}

static void hash_end(void *state, uint8_t digest[BOX_HASH_BYTES])
{
    sha256_end((Sha256 *)state, digest);
}

/* The part `part` of the box, held to its hash. */
static int box_part_bytes(file_id_t file, const BoxPart *part, size_t limit, const BoxHasher *hasher,
                          char **bytes, size_t *size)
{
    if (part->Flags & BOX_FLAG_COMPRESSED) return -ERR_UNSUPPORTED;
    if (part->Size > limit) return -ERR_CORRUPTED;

    char *copy = malloc((size_t)part->Size + 1);
    if (!copy) return -ERR_NO_MEMORY;
    int rc = read_exact(file, part->Offset, copy, (size_t)part->Size);
    if (rc == 0 && !BoxPartHolds(part, (const uint8_t *)copy, hasher)) rc = -ERR_CORRUPTED;
    if (rc != 0) {
        free(copy);
        return rc;
    }
    copy[part->Size] = '\0';
    if (part->Size == 0) {
        free(copy);
        copy = NULL;
    }
    *bytes = copy;
    *size  = (size_t)part->Size;
    return 1;
}

/* The part called Name of the box File: its header, then its table and names,
 * held to the seal, then the part. */
static int box_part_read(file_id_t file, uint64_t file_size, const char *name, size_t limit,
                         char **bytes, size_t *size)
{
    uint8_t  head_bytes[BOX_HEADER_BYTES];
    uint64_t look = file_size < BOX_HEADER_BYTES ? file_size : BOX_HEADER_BYTES;
    int      rc   = read_exact(file, 0, head_bytes, (size_t)look);
    if (rc != 0) return rc;
    BoxHead head;
    if (BoxReadHead(head_bytes, look, file_size, &head) != BOX_OK) return -ERR_CORRUPTED;

    uint8_t *prefix = malloc((size_t)head.PrefixBytes);
    if (!prefix) return -ERR_NO_MEMORY;
    rc = read_exact(file, 0, prefix, (size_t)head.PrefixBytes);

    Sha256    state;
    BoxHasher hasher = { &state, hash_begin, hash_add, hash_end };
    BoxTable  table;
    BoxPart   part;
    if (rc == 0 && (BoxRead(prefix, head.PrefixBytes, &head, &table) != BOX_OK ||
                    !BoxSealHolds(&table, &hasher)))
        rc = -ERR_CORRUPTED;
    if (rc == 0)
        rc = BoxPartFind(&table, (const uint8_t *)name, strlen(name), &part)
                 ? box_part_bytes(file, &part, limit, &hasher, bytes, size)
                 : 0;
    free(prefix);
    return rc;
}

/* ---- the ELF image ------------------------------------------------------- */

/* The section an ELF image keeps a part in — for the parts an image has. */
static const char *elf_section_of(const char *name)
{
    if (strcmp(name, "form") == 0) return ".boxos.form";
    if (strcmp(name, "settings") == 0) return ".boxos.settings";
    if (strcmp(name, "life") == 0) return ".boxos.life";
    if (strcmp(name, "recipe") == 0) return ".boxos.recipe";
    return NULL;
}

/* Whether a file of `file_size` bytes beginning with `first` is an ELF64
 * little-endian image, as image_section.c reads one: its whole header there. */
static bool elf_image(const uint8_t *first, uint64_t look, uint64_t file_size)
{
    return file_size >= 64 && look >= 6 && memcmp(first, "\x7f" "ELF", 4) == 0 && first[4] == 2 &&
           first[5] == 1;
}

/* ---- which kind ---------------------------------------------------------- */

int program_part_read(file_id_t file, const char *name, size_t limit, char **bytes, size_t *size,
                      ProgramKind *kind)
{
    if (kind) *kind = PROGRAM_NONE;
    if (!name || !bytes || !size) return -ERR_INVALID_ARGUMENT;
    *bytes = NULL;
    *size  = 0;

    FileMetadata metadata;
    int rc = FileMetadataRead(file, &metadata);
    if (rc != 0) return rc;
    uint64_t file_size = metadata.Size;
    FileMetadataRelease(&metadata);

    static const uint8_t box_magic[8] = { BOX_MAGIC_0, BOX_MAGIC_1, BOX_MAGIC_2, BOX_MAGIC_3,
                                          BOX_MAGIC_4, BOX_MAGIC_5, BOX_MAGIC_6, BOX_MAGIC_7 };
    uint8_t  first[8];
    uint64_t look = file_size < sizeof(first) ? file_size : sizeof(first);
    rc = read_exact(file, 0, first, (size_t)look);
    if (rc != 0) return rc;

    if (look == sizeof(first) && memcmp(first, box_magic, sizeof(first)) == 0) {
        if (kind) *kind = PROGRAM_BOX;
        return box_part_read(file, file_size, name, limit, bytes, size);
    }

    if (!elf_image(first, look, file_size)) return 0;
    if (kind) *kind = PROGRAM_ELF;
    const char *section = elf_section_of(name);
    return section ? image_section_read(file, section, limit, bytes, size, NULL) : 0;
}

/* ---- this program's memory ------------------------------------------------ */

int program_part_loaded(const char *name, const uint8_t **bytes, size_t *size)
{
    if (!name || !bytes || !size) return -ERR_INVALID_ARGUMENT;
    uint64_t at = cabin_info()->parts_addr;
    if (at == 0) return 0;

    /* The kernel wrote the table, on pages this program can only read. */
    const BoxLoadedHead *head   = (const BoxLoadedHead *)(uintptr_t)at;
    const BoxLoadedPart *parts  = (const BoxLoadedPart *)(head + 1);
    size_t               length = strlen(name);
    for (uint32_t i = 0; i < head->Count; i++) {
        if (parts[i].NameLength != length ||
            memcmp((const char *)(uintptr_t)at + parts[i].NameAt, name, length) != 0)
            continue;
        *bytes = (const uint8_t *)(uintptr_t)parts[i].Address;
        *size  = (size_t)parts[i].Size;
        return 1;
    }
    return 0;
}
