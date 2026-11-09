/*
 * nameplate.c — resolve an address to a name, out of this program's own table.
 *
 * The table is made by tools/nameplate or the BoxASM linker (see
 * src/include/nameplate_format.h). A box carries it as its part `names`,
 * which the kernel puts into memory at start and names in the table of the
 * parts it loaded; an ELF image carries it in its read-only segment, between
 * the two symbols below. Everything here is a read of read-only memory: no
 * syscall, no allocation, no lock — because the caller is usually in the
 * middle of something going wrong, and may well be holding a lock of its own.
 */

#include "box/nameplate.h"
#include "box/defs.h"
#include "box/program_part.h"
#include "box/core/cabin.h"

#include "nameplate_format.h"

/* PROVIDEd by user.ld (and defined by BoxASM's linker) round an ELF image's
 * table — inside the image, so hidden: reached relative to the code. Without a
 * table, start == end. A box has its table elsewhere: these name nothing
 * loaded there. */
extern const char __nameplate_start[] __attribute__((visibility("hidden")));
extern const char __nameplate_end[] __attribute__((visibility("hidden")));

typedef struct NameplateView {
    const NameplateHeader *Header;
    const uint32_t *Offsets;
    const uint32_t *Sizes;
    const uint32_t *NameOffsets;
    const char     *Names;
} NameplateView;

/*
 * Take a look at the table, refusing anything that is not exactly what it
 * claims to be. Validation is O(1) — the header only — and it is repeated on
 * every call rather than cached: caching would need a flag, a flag would need
 * to be safe against two strands arriving at once, and none of that is worth
 * buying when the check is six comparisons. The arrays are not scanned here,
 * so every index formed later is bounds-checked where it is formed.
 */
static int NameplateOpen(NameplateView *View)
{
    const NameplateHeader *Header;
    const char *Base = __nameplate_start;
    uint64_t Bytes = (uint64_t)(__nameplate_end - __nameplate_start);
    uint32_t Count;

    if (cabin_info()->parts_addr != 0) {
        const uint8_t *Loaded;
        size_t Size;

        if (program_part_loaded("names", &Loaded, &Size) != 1)
            return 0;
        Base = (const char *)Loaded;
        Bytes = Size;
    }
    Header = (const NameplateHeader *)(const void *)Base;
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
    /* The table names addresses where the image was linked; the kernel placed
     * it this much further on. */
    uint64_t Slide = cabin_info()->image_base;

    if (!site)
        return 0;
    if (!NameplateOpen(&View))
        return 0;
    if ((uint64_t)address < Slide)
        return 0;

    Want = (uint64_t)address - Slide;
    if (Want < View.Header->BaseAddress)
        return 0;
    Want -= View.Header->BaseAddress;
    if (Want > 0xFFFFFFFFull)
        return 0;
    if ((uint32_t)Want < View.Offsets[0])
        return 0;

    /* Greatest entry at or below the address. The invariant is
     * Offsets[Low] <= Want < Offsets[High], held from the first line. */
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
    Delta = (uint64_t)address - Slide - Start;

    /* Past the end of the function it landed nearest to: padding, or code no
     * symbol claims. Say nothing rather than name the neighbour. */
    if (View.Sizes[Hit] != 0 && Delta >= View.Sizes[Hit])
        return 0;

    NameOffset = View.NameOffsets[Hit];
    if (NameOffset >= View.Header->NameBytes)
        return 0;
    /* The blob's last byte is a NUL (the header check guarantees the blob is
     * at least EntryCount bytes and the tool always terminates), so a name
     * starting inside it is always terminated inside it. */
    if (View.Names[View.Header->NameBytes - 1] != '\0')
        return 0;

    site->Name   = View.Names + NameOffset;
    site->Start  = (uintptr_t)(Start + Slide);
    site->Offset = Delta;
    return 1;
}

uint32_t nameplate_count(void)
{
    NameplateView View;

    return NameplateOpen(&View) ? View.Header->EntryCount : 0u;
}
