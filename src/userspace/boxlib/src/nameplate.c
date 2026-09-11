
#include "box/nameplate.h"
#include "box/defs.h"

#include "nameplate_format.h"

extern const char __nameplate_start[];
extern const char __nameplate_end[];

typedef struct NameplateView {
    const NameplateHeader *Header;
    const uint32_t *Offsets;
    const uint32_t *Sizes;
    const uint32_t *NameOffsets;
    const char     *Names;
} NameplateView;

static int NameplateOpen(NameplateView *View)
{
    const NameplateHeader *Header = (const NameplateHeader *)(const void *)__nameplate_start;
    uint64_t Bytes = (uint64_t)(__nameplate_end - __nameplate_start);
    const char *Base = __nameplate_start;
    uint32_t Count;

    if (Bytes < sizeof(NameplateHeader))
        return 0;
    if (!NameplateHeaderValid(Header, Bytes))
        return 0;

    Count = Header->EntryCount;
    View->Header      = Header;
    View->Offsets     = (const uint32_t *)(const void *)(Base + NAMEPLATE_OFFSETS_AT(Count));
    View->Sizes       = (const uint32_t *)(const void *)(Base + NAMEPLATE_SIZES_AT(Count));
    View->NameOffsets = (const uint32_t *)(const void *)(Base + NAMEPLATE_NAME_OFFSETS_AT(Count));
    View->Names       = Base + NAMEPLATE_NAMES_AT(Count);
    return 1;
}

int nameplate_lookup(uintptr_t address, NameplateSite *site)
{
    NameplateView View;
    uint64_t Want, Start, Delta;
    uint32_t Low, High, Hit, NameOffset;

    if (!site)
        return 0;
    if (!NameplateOpen(&View))
        return 0;

    Want = (uint64_t)address;
    if (Want < View.Header->BaseAddress)
        return 0;
    Want -= View.Header->BaseAddress;
    if (Want > 0xFFFFFFFFull)
        return 0;
    if ((uint32_t)Want < View.Offsets[0])
        return 0;

    Low  = 0;
    High = View.Header->EntryCount;
    while (High - Low > 1) {
        uint32_t Mid = Low + (High - Low) / 2;

        if (View.Offsets[Mid] <= (uint32_t)Want)
            Low = Mid;
        else
            High = Mid;
    }
    Hit = Low;

    Start = View.Header->BaseAddress + View.Offsets[Hit];
    Delta = (uint64_t)address - Start;

    if (View.Sizes[Hit] != 0 && Delta >= View.Sizes[Hit])
        return 0;

    NameOffset = View.NameOffsets[Hit];
    if (NameOffset >= View.Header->NameBytes)
        return 0;
    if (View.Names[View.Header->NameBytes - 1] != '\0')
        return 0;

    site->Name   = View.Names + NameOffset;
    site->Start  = (uintptr_t)Start;
    site->Offset = Delta;
    return 1;
}

uint32_t nameplate_count(void)
{
    NameplateView View;

    return NameplateOpen(&View) ? View.Header->EntryCount : 0u;
}