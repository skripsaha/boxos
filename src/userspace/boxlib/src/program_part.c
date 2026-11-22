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

/* ---- which kind ---------------------------------------------------------- */

/* What a program's file begins with, and the fewest bytes a file of its kind
 * holds: a box's magic; an ELF64 little-endian image's, as image_section.c
 * reads one, its whole header there. The system keeps every file's first
 * bytes on its card (FileMetadata.Head), so these are what program_kind tells
 * a program by and what program_files asks the volume for. */
typedef struct {
    ProgramKind Kind;
    uint8_t     Length;
    uint8_t     Least;
    uint8_t     Bytes[8];
} ProgramBeginning;

static const ProgramBeginning beginnings[] = {
    { PROGRAM_BOX, 8, 8,
      { BOX_MAGIC_0, BOX_MAGIC_1, BOX_MAGIC_2, BOX_MAGIC_3, BOX_MAGIC_4, BOX_MAGIC_5, BOX_MAGIC_6, BOX_MAGIC_7 } },
    { PROGRAM_ELF, 6, 64, { 0x7f, 'E', 'L', 'F', 2, 1 } },
};
#define BEGINNINGS (sizeof beginnings / sizeof beginnings[0])

/* What a file of `file_size` bytes beginning with `first` (`look` of them) is. */
static ProgramKind kind_by(const uint8_t *first, uint64_t look, uint64_t file_size)
{
    for (size_t i = 0; i < BEGINNINGS; i++)
        if (look >= beginnings[i].Length && file_size >= beginnings[i].Least &&
            memcmp(first, beginnings[i].Bytes, beginnings[i].Length) == 0)
            return beginnings[i].Kind;
    return PROGRAM_NONE;
}

/* What the file is, by its first bytes — the ones the system keeps of it
 * (FileMetadata.Head): no byte of the file is read. Its size, its first bytes
 * in `first` (`look` of them, 8 at the most) and the kind they say. 0, or why
 * the file's card could not be read. */
static int kind_of(file_id_t file, uint64_t *file_size, uint8_t first[8], uint64_t *look, ProgramKind *kind)
{
    FileMetadata metadata;
    int rc = FileMetadataRead(MY_TAGS, file, &metadata);
    if (rc != 0) return rc;
    *file_size = metadata.Size;
    *look      = metadata.HeadLength < 8 ? metadata.HeadLength : 8;
    memcpy(first, metadata.Head, (size_t)*look);
    FileMetadataRelease(&metadata);
    *kind = kind_by(first, *look, *file_size);
    return 0;
}

int program_kind(file_id_t file, ProgramKind *kind)
{
    uint64_t size, look;
    uint8_t  first[8];
    *kind = PROGRAM_NONE;
    return kind_of(file, &size, first, &look, kind);
}

/* The ids of `a` (`na` of them) and of `b` (`nb`), each rising, as one rising
 * list; `a` let go. NULL with no memory. */
static file_id_t *merged(file_id_t *a, int na, const file_id_t *b, int nb)
{
    file_id_t *out = malloc(((size_t)na + (size_t)nb) * sizeof(file_id_t));
    if (out) {
        int i = 0, j = 0, k = 0;
        while (i < na || j < nb) out[k++] = j >= nb || (i < na && a[i] < b[j]) ? a[i++] : b[j++];
    }
    free(a);
    return out;
}

int program_files(file_id_t **out_ids)
{
    if (!out_ids) return -ERR_INVALID_ARGUMENT;
    *out_ids = NULL;
    file_id_t *found = NULL;
    int        count = 0;
    for (size_t i = 0; i < BEGINNINGS; i++) {
        file_id_t *ids = NULL;
        int        n   = files_starting_with(beginnings[i].Bytes, beginnings[i].Length, &ids);
        if (n < 0) {
            free(found);
            return n;
        }
        /* A file holds at least as many bytes as it begins with; one of a
         * kind that must hold more is asked how many it holds. */
        int kept = 0;
        for (int at = 0; at < n; at++) {
            ProgramKind kind = PROGRAM_NONE;
            if (beginnings[i].Least > beginnings[i].Length &&
                (program_kind(ids[at], &kind) != 0 || kind != beginnings[i].Kind))
                continue;
            ids[kept++] = ids[at];
        }
        if (kept) {
            found = merged(found, count, ids, kept);
            if (!found) {
                free(ids);
                return -ERR_NO_MEMORY;
            }
            count += kept;
        }
        free(ids);
    }
    *out_ids = found;
    return count;
}

int program_part_read(file_id_t file, const char *name, size_t limit, char **bytes, size_t *size,
                      ProgramKind *kind)
{
    if (kind) *kind = PROGRAM_NONE;
    if (!name || !bytes || !size) return -ERR_INVALID_ARGUMENT;
    *bytes = NULL;
    *size  = 0;

    uint64_t    file_size, look;
    uint8_t     first[8];
    ProgramKind found;
    int         rc = kind_of(file, &file_size, first, &look, &found);
    if (rc != 0) return rc;
    if (kind) *kind = found;

    if (found == PROGRAM_BOX) return box_part_read(file, file_size, name, limit, bytes, size);
    if (found == PROGRAM_NONE) return 0;
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
