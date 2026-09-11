#ifndef BOXOS_NAMEPLATE_FORMAT_H
#define BOXOS_NAMEPLATE_FORMAT_H


#include "boxos_magic.h"

#define NAMEPLATE_SECTION_NAME  ".nameplate"

#define NAMEPLATE_VERSION       1u

#define NAMEPLATE_MAX_ENTRIES   (1u << 24)

typedef struct NameplateHeader {
    uint32_t Magic;
    uint32_t Version;
    uint32_t EntryCount;
    uint32_t NameBytes;
    uint64_t BaseAddress;
    uint64_t TotalBytes;
} NameplateHeader;

#define NAMEPLATE_OFFSETS_AT(n)       ((uint64_t)sizeof(NameplateHeader))
#define NAMEPLATE_SIZES_AT(n)         (NAMEPLATE_OFFSETS_AT(n)  + 4ull * (uint64_t)(n))
#define NAMEPLATE_NAME_OFFSETS_AT(n)  (NAMEPLATE_SIZES_AT(n)    + 4ull * (uint64_t)(n))
#define NAMEPLATE_NAMES_AT(n)         (NAMEPLATE_NAME_OFFSETS_AT(n) + 4ull * (uint64_t)(n))
#define NAMEPLATE_TOTAL_BYTES(n, nb)  (NAMEPLATE_NAMES_AT(n)    + (uint64_t)(nb))

static inline int NameplateHeaderValid(const NameplateHeader *Header, uint64_t Bytes)
{
    uint64_t Expected;

    if (!Header)
        return 0;
    if (Header->Magic != NAMEPLATE_MAGIC || Header->Version != NAMEPLATE_VERSION)
        return 0;
    if (Header->EntryCount == 0 || Header->EntryCount > NAMEPLATE_MAX_ENTRIES)
        return 0;
    if (Header->NameBytes < Header->EntryCount)
        return 0;

    Expected = NAMEPLATE_TOTAL_BYTES(Header->EntryCount, Header->NameBytes);
    if (Header->TotalBytes != Expected)
        return 0;
    if (Bytes != 0 && Header->TotalBytes > Bytes)
        return 0;

    return 1;
}

#endif