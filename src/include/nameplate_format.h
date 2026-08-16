#ifndef BOXOS_NAMEPLATE_FORMAT_H
#define BOXOS_NAMEPLATE_FORMAT_H

/*
 * Nameplate — the table that lets an address name itself.
 *
 * A machine part carries a nameplate riveted to its side: the name travels
 * WITH the part, not in a service book at the other end of the world. A BoxOS
 * image carries the same thing — a `.nameplate` section, generated at link
 * time from the binary's own symbol table, that turns a code address into the
 * name of the function containing it.
 *
 * Why not the ELF .symtab the image already has: the loader maps PT_LOAD and
 * nothing else, so .symtab is not in the process's address space at all. Why
 * not read the image back from TagFS on demand: symbolization is wanted
 * exactly when something died, and a diagnostic that needs the filesystem is
 * worthless for the faults that happen INSIDE the filesystem — or before it
 * is up. So the table is SHF_ALLOC: it rides inside a PT_LOAD segment, the
 * loader maps it for free, and a lookup is a binary search through the
 * process's own memory. No syscall, no disk, no lock, nothing to be holding
 * when you crash.
 *
 * It is also smaller than what it replaces. .symtab + .strtab for the C++
 * test image is 6.9 MB, of which most is symbols no backtrace can ever name;
 * the equivalent Nameplate is 3.7 MB, so an image that carries one and drops
 * both is SMALLER than it was.
 *
 * Three consumers share this one definition, which is why it lives here and
 * not in any of them:
 *   tools/nameplate.c            writes it, at link time
 *   boxlib box/nameplate.h       reads it, in-process (C, C++, the shell)
 *   kernel idt.c                 reads it, out-of-process, through get_user,
 *                                to name the frames of a user-mode fault
 *
 * ── Layout ────────────────────────────────────────────────────────────────
 *
 *   NameplateHeader                                  32 bytes
 *   uint32_t Offset[EntryCount]     address - BaseAddress, STRICTLY ASCENDING
 *   uint32_t Size[EntryCount]       function length in bytes, 0 = unknown
 *   uint32_t NameOffset[EntryCount] byte offset into Names
 *   char     Names[NameBytes]       NUL-terminated, last byte is always NUL
 *
 * Three parallel arrays rather than an array of structs: a lookup binary-
 * searches Offset alone, so the search touches 4 bytes per entry instead of
 * 12 and a cache line carries 16 candidates instead of 5.
 *
 * Addresses are 32-bit deltas from BaseAddress. User images start at 0xC000
 * and the largest is a few megabytes, so the delta always fits; the generator
 * refuses to emit a table where it would not, rather than truncating.
 *
 * Names are stored MANGLED, exactly as the linker wrote them. Demangling is
 * not done here and not done at build time: measured on the C++ test image,
 * demangling grows 3.67 MB of names to 9.71 MB, with single names reaching
 * 5266 characters — the image budget does not have that, and a demangler in
 * the tree would serve more than this table anyway (typeid().name(), the
 * shell) and belongs to whoever writes it.
 *
 * Integer types come from the includer, as everywhere in src/include:
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/defs.h"
 *   Host tool: #include <stdint.h>
 * ========================================================================== */

#include "boxos_magic.h"

/* The section the linker script places, and the tool fills. */
#define NAMEPLATE_SECTION_NAME  ".nameplate"

#define NAMEPLATE_VERSION       1u

/* A table with more entries than this is refused as corrupt rather than
 * trusted: the largest real one in the tree holds ~40k, and the bound keeps
 * every size computation below far away from overflowing 64 bits. */
#define NAMEPLATE_MAX_ENTRIES   (1u << 24)

typedef struct NameplateHeader {
    uint32_t Magic;        /* NAMEPLATE_MAGIC */
    uint32_t Version;      /* NAMEPLATE_VERSION */
    uint32_t EntryCount;
    uint32_t NameBytes;
    uint64_t BaseAddress;  /* address that Offset[] is measured from */
    uint64_t TotalBytes;   /* whole table, header included — self-describing */
} NameplateHeader;

/* Byte offsets of the four arrays, for readers that cannot simply take a
 * pointer into the table — the kernel copies through get_user, one word at a
 * time, and needs the arithmetic without the dereference. */
#define NAMEPLATE_OFFSETS_AT(n)       ((uint64_t)sizeof(NameplateHeader))
#define NAMEPLATE_SIZES_AT(n)         (NAMEPLATE_OFFSETS_AT(n)  + 4ull * (uint64_t)(n))
#define NAMEPLATE_NAME_OFFSETS_AT(n)  (NAMEPLATE_SIZES_AT(n)    + 4ull * (uint64_t)(n))
#define NAMEPLATE_NAMES_AT(n)         (NAMEPLATE_NAME_OFFSETS_AT(n) + 4ull * (uint64_t)(n))
#define NAMEPLATE_TOTAL_BYTES(n, nb)  (NAMEPLATE_NAMES_AT(n)    + (uint64_t)(nb))

/*
 * Is this header one we may act on? O(1) — the arrays themselves are NOT
 * scanned, here or anywhere: a lookup bounds-checks every index it forms, so
 * a table that is corrupt beyond its header can return a wrong name but can
 * never read outside itself. A diagnostic path pays for its own safety, not
 * for a full audit of 40k entries.
 *
 * `Bytes` is how many bytes the reader actually has (the section's size, or
 * the mapping's). Pass 0 when it is not known independently, and TotalBytes
 * is trusted for consistency only.
 */
static inline int NameplateHeaderValid(const NameplateHeader *Header, uint64_t Bytes)
{
    uint64_t Expected;

    if (!Header)
        return 0;
    if (Header->Magic != NAMEPLATE_MAGIC || Header->Version != NAMEPLATE_VERSION)
        return 0;
    if (Header->EntryCount == 0 || Header->EntryCount > NAMEPLATE_MAX_ENTRIES)
        return 0;
    /* Every name is at least one NUL, and the blob ends with one. */
    if (Header->NameBytes < Header->EntryCount)
        return 0;

    Expected = NAMEPLATE_TOTAL_BYTES(Header->EntryCount, Header->NameBytes);
    if (Header->TotalBytes != Expected)
        return 0;
    if (Bytes != 0 && Header->TotalBytes > Bytes)
        return 0;

    return 1;
}

#endif /* BOXOS_NAMEPLATE_FORMAT_H */
