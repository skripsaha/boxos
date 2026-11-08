#include "box_load.h"
#include "box_reader.h"
#include "box_needs.h"
#include "nameplate_format.h"
#include "cabin_layout.h"
#include "aslr.h"
#include "cpuid.h"
#include "fpu.h"
#include "crypto.h"
#include "pmm.h"
#include "klib.h"

/* ── the kernel's SHA-256 behind the reader ─────────────────────────────── */

static void BoxHashBegin(void *state)
{
    KSha256Init((KSha256Ctx *)state);
}

static void BoxHashAdd(void *state, const uint8_t *bytes, uint64_t count)
{
    while (count > 0) {
        uint32_t piece = count > 0x40000000u ? 0x40000000u : (uint32_t)count;
        KSha256Update((KSha256Ctx *)state, bytes, piece);
        bytes += piece;
        count -= piece;
    }
}

static void BoxHashEnd(void *state, uint8_t digest[BOX_HASH_BYTES])
{
    KSha256Final((KSha256Ctx *)state, digest);
}

/* One load: the box's table, read from its start, and what reads it. */
typedef struct {
    vmm_context_t     *Vmm;
    const ImageSource *Source;
    const char        *Name;
    KSha256Ctx         HashState;
    BoxHasher          Hasher;
    BoxTable           Table;
} BoxLoading;

static error_t BoxRefuse(const BoxLoading *load, BoxVerdict verdict, const BoxPart *part)
{
    if (part)
        kprintf("[IMAGE] '%s' is not started: %s (part %.*s)\n", load->Name, BoxVerdictText(verdict),
                (int)part->NameLength, (const char *)part->Name);
    else
        kprintf("[IMAGE] '%s' is not started: %s\n", load->Name, BoxVerdictText(verdict));
    return ERR_INVALID_ELF;
}

/* ── bytes read whole ───────────────────────────────────────────────────── */

/* Bytes of the file the loader reads whole — the start of it, a small part —
 * in pages of their own for as long as they are read. */
typedef struct {
    void    *Frames;
    size_t   Pages;
    uint8_t *Bytes;
} BoxHeld;

static void BoxLetGo(BoxHeld *held)
{
    if (held->Frames)
        pmm_free(held->Frames, held->Pages);
    held->Frames = NULL;
    held->Bytes  = NULL;
}

static error_t BoxHold(const BoxLoading *load, uint64_t offset, uint64_t count, BoxHeld *out)
{
    out->Frames = NULL;
    out->Pages  = (size_t)((count + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);
    out->Bytes  = NULL;
    if (count == 0)
        return OK;
    out->Frames = pmm_alloc(out->Pages);
    if (!out->Frames) {
        kprintf("[IMAGE] '%s' is not started: no %zu pages to read %llu of its bytes into\n",
                load->Name, out->Pages, (unsigned long long)count);
        return ERR_NO_MEMORY;
    }
    out->Bytes = (uint8_t *)vmm_phys_to_virt((uintptr_t)out->Frames);
    error_t rc = ImageSourceRead(load->Source, offset, out->Bytes, count, load->Name);
    if (rc != OK)
        BoxLetGo(out);
    return rc;
}

/* A part read whole and held to its hash. */
static error_t BoxHoldPart(BoxLoading *load, const BoxPart *part, BoxHeld *out)
{
    error_t rc = BoxHold(load, part->Offset, part->Size, out);
    if (rc != OK)
        return rc;
    if (!BoxPartHolds(part, out->Bytes, &load->Hasher)) {
        BoxLetGo(out);
        return BoxRefuse(load, BOX_PART_HASH, part);
    }
    return OK;
}

/* ── needs ──────────────────────────────────────────────────────────────── */

static bool BoxNeedMet(const BoxNeedsCpu *need)
{
    uint32_t words[4];
    uint32_t top = need->Leaf >= 0x80000000u ? g_cpu_caps.max_extended_leaf : g_cpu_caps.max_basic_leaf;
    if (need->Leaf > top)
        return false;
    cpuid_count(need->Leaf, need->Subleaf, &words[0], &words[1], &words[2], &words[3]);
    if (((words[need->Register] >> need->Bit) & 1u) == 0)
        return false;
    return (g_xsave_mask & need->State) == need->State;
}

/* Every fact of the `needs` part this system knows held to this machine. */
static error_t BoxCheckNeeds(BoxLoading *load, const BoxPart *part, const uint8_t *bytes)
{
    BoxText    text;
    BoxVerdict verdict = BoxTextBegin(bytes, part->Size, BOX_KIND_NEEDS, &text);
    if (verdict != BOX_OK)
        return BoxRefuse(load, verdict, part);

    const uint8_t *line, *word;
    uint64_t       length, word_length;
    while (BoxTextLine(&text, &line, &length)) {
        uint64_t at = 0;
        BoxTextWord(line, length, &at, &word, &word_length);
        if (!BoxSpanIs(word, word_length, "cpu"))
            continue;   /* a fact this system does not know: passed over */
        if (BoxTextWordCount(line, length) != 2)
            return BoxRefuse(load, BOX_TEXT_LAYOUT, part);
        BoxTextWord(line, length, &at, &word, &word_length);

        const BoxNeedsCpu *need = BoxNeedsCpuFind(word, word_length);
        if (!need) {
            kprintf("[IMAGE] '%s' is not started: it needs cpu %.*s, which this system does not "
                    "know how to look for\n", load->Name, (int)word_length, (const char *)word);
            return ERR_INVALID_ELF;
        }
        if (!BoxNeedMet(need)) {
            kprintf("[IMAGE] '%s' is not started: it needs %s, and this machine has none\n",
                    load->Name, need->Word);
            return ERR_INVALID_ELF;
        }
    }
    return OK;
}

/* ── the image ──────────────────────────────────────────────────────────── */

/*
 * One part of the image in memory: code, const, data, or the names. Its pages
 * are taken one at a time — no part needs memory that happens to be in one
 * piece — the frame of each kept in an array in pages of its own, and each
 * page filled straight from the file, its hash taken as it comes.
 */
typedef struct {
    bool       Present;
    BoxPart    Part;
    uint64_t   Place;     /* from the base */
    uint64_t   Pages;
    uint64_t   Rights;    /* VMM flags */
    uintptr_t *Frames;
    void      *FramesHeld;
    size_t     FramesPages;
    uint64_t   Taken;     /* frames allocated */
    uint64_t   Mapped;    /* pages mapped — the cabin's from then on */
} BoxImagePart;

enum { BOX_IMAGE_CODE, BOX_IMAGE_CONST, BOX_IMAGE_DATA, BOX_IMAGE_NAMES, BOX_IMAGE_PARTS };

/* What a part not mapped still holds, given back. */
static void BoxImageLetGo(BoxImagePart *parts)
{
    for (unsigned i = 0; i < BOX_IMAGE_PARTS; i++) {
        BoxImagePart *part = &parts[i];
        for (uint64_t page = part->Mapped; page < part->Taken; page++)
            pmm_free((void *)part->Frames[page], 1);
        if (part->FramesHeld)
            pmm_free(part->FramesHeld, part->FramesPages);
        part->FramesHeld = NULL;
        part->Taken = part->Mapped = 0;
    }
}

/* The part's pages taken and filled from the file, and its hash held. */
static error_t BoxImageRead(BoxLoading *load, BoxImagePart *part)
{
    part->Pages       = (part->Part.Memory + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    part->FramesPages = (size_t)((part->Pages * sizeof(uintptr_t) + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);
    if (part->Pages == 0)
        return OK;
    part->FramesHeld = pmm_alloc(part->FramesPages);
    if (!part->FramesHeld) {
        kprintf("[IMAGE] '%s' is not started: no room to list the %llu pages of its part %.*s\n",
                load->Name, (unsigned long long)part->Pages, (int)part->Part.NameLength,
                (const char *)part->Part.Name);
        return ERR_NO_MEMORY;
    }
    part->Frames = (uintptr_t *)vmm_phys_to_virt((uintptr_t)part->FramesHeld);

    uint8_t digest[BOX_HASH_BYTES];
    load->Hasher.Begin(load->Hasher.State);
    for (uint64_t page = 0; page < part->Pages; page++) {
        void *frame = pmm_alloc_zero(1);
        if (!frame) {
            kprintf("[IMAGE] '%s' is not started: no page for its part %.*s\n", load->Name,
                    (int)part->Part.NameLength, (const char *)part->Part.Name);
            return ERR_NO_MEMORY;
        }
        part->Frames[page] = (uintptr_t)frame;
        part->Taken        = page + 1;

        uint64_t from = page * VMM_PAGE_SIZE;
        if (from >= part->Part.Size)
            continue;   /* data's zeros past the file's bytes */
        uint64_t count = part->Part.Size - from < VMM_PAGE_SIZE ? part->Part.Size - from : VMM_PAGE_SIZE;
        uint8_t *bytes = (uint8_t *)vmm_phys_to_virt((uintptr_t)frame);
        error_t  rc    = ImageSourceRead(load->Source, part->Part.Offset + from, bytes, count, load->Name);
        if (rc != OK)
            return rc;
        load->Hasher.Add(load->Hasher.State, bytes, count);
    }
    load->Hasher.End(load->Hasher.State, digest);
    if (!BoxSameHash(digest, part->Part.Hash))
        return BoxRefuse(load, BOX_PART_HASH, &part->Part);
    return OK;
}

/* Every fixup of an accepted `fixups` part: the base added to the word it
 * names, in the frames of the part that holds it — before any of them is
 * mapped, so code and constants are mapped already fixed and never writable. */
static void BoxImageFix(BoxImagePart *parts, const uint8_t *fixups, uint64_t size, uint64_t base)
{
    BoxFixupWalk walk;
    uint64_t     offset;
    BoxFixupBegin(fixups, size, &walk);
    while (BoxFixupNext(&walk, &offset)) {
        for (unsigned i = BOX_IMAGE_CODE; i <= BOX_IMAGE_DATA; i++) {
            BoxImagePart *part = &parts[i];
            if (!part->Present || offset < part->Place || offset - part->Place >= part->Part.Size)
                continue;
            uint64_t  within = offset - part->Place;
            uint8_t  *page   = (uint8_t *)vmm_phys_to_virt(part->Frames[within / VMM_PAGE_SIZE]);
            uint64_t *word   = (uint64_t *)(page + within % VMM_PAGE_SIZE);
            *word += base;
            break;
        }
    }
}

static error_t BoxImageMap(BoxLoading *load, BoxImagePart *part, uint64_t base)
{
    for (uint64_t page = 0; page < part->Pages; page++) {
        uintptr_t        at     = (uintptr_t)(base + part->Place + page * VMM_PAGE_SIZE);
        vmm_map_result_t mapped = vmm_map_page(load->Vmm, at, part->Frames[page], part->Rights);
        if (!mapped.success) {
            kprintf("[IMAGE] '%s' is not started: its part %.*s was not mapped at 0x%llx: %s\n",
                    load->Name, (int)part->Part.NameLength, (const char *)part->Part.Name,
                    (unsigned long long)at, mapped.error_msg);
            return ERR_SPAWN_FAILED;
        }
        part->Mapped = page + 1;
    }
    return OK;
}

/* Where the image goes: anywhere in the room a program's image has, chosen at
 * random, when the box carries fixups; where its places say, which must lie in
 * that room, when it does not. */
static error_t BoxImageBase(BoxLoading *load, BoxImagePart *parts, bool movable, uint64_t *base)
{
    uint64_t low = BOX_PLACE_LIMIT, high = 0;
    for (unsigned i = 0; i < BOX_IMAGE_PARTS; i++) {
        if (!parts[i].Present || parts[i].Pages == 0)
            continue;
        uint64_t end = parts[i].Place + parts[i].Pages * VMM_PAGE_SIZE;
        if (parts[i].Place < low) low = parts[i].Place;
        if (end > high) high = end;
    }
    if (high == 0)
        low = 0;

    uint64_t room = CABIN_CODE_END_ADDR - CABIN_CODE_START_ADDR;
    if (movable) {
        if (high > room) {
            kprintf("[IMAGE] '%s' is not started: its image of %llu bytes is larger than the room "
                    "a program's image has (%llu)\n", load->Name, (unsigned long long)high,
                    (unsigned long long)room);
            return ERR_BINARY_TOO_LARGE;
        }
        *base = CABIN_CODE_START_ADDR + aslr_random_offset(room - high + VMM_PAGE_SIZE);
        return OK;
    }
    if (low < CABIN_CODE_START_ADDR || high > CABIN_CODE_END_ADDR) {
        kprintf("[IMAGE] '%s' is not started: it cannot be moved, and its image at 0x%llx-0x%llx "
                "lies outside the room a program's image has\n", load->Name,
                (unsigned long long)low, (unsigned long long)high);
        return ERR_INVALID_ELF;
    }
    *base = 0;
    return OK;
}

/* ── the box ────────────────────────────────────────────────────────────── */

static error_t BoxLoadImage(BoxLoading *load, ImageLoaded *loaded, uint64_t start, const BoxHeld *fixups,
                            const BoxPart *fixups_part)
{
    static const BoxKind  kinds[BOX_IMAGE_PARTS]  = { BOX_KIND_CODE, BOX_KIND_CONST, BOX_KIND_DATA,
                                                      BOX_KIND_NAMES };
    static const uint64_t rights[BOX_IMAGE_PARTS] = {
        VMM_FLAG_PRESENT | VMM_FLAG_USER,
        VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE,
        VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NO_EXECUTE,
        VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE,
    };
    BoxImagePart parts[BOX_IMAGE_PARTS];
    memset(parts, 0, sizeof(parts));

    /* The names go on the pages after the image, read-only: a fault dump finds
     * them there, and nothing ever runs them. */
    uint64_t image_end = 0;
    for (unsigned i = 0; i < BOX_IMAGE_PARTS; i++) {
        parts[i].Rights  = rights[i];
        parts[i].Present = BoxPartOfKind(&load->Table, kinds[i], &parts[i].Part);
        if (!parts[i].Present)
            continue;
        if (i == BOX_IMAGE_NAMES) {
            parts[i].Place = image_end;
        } else {
            parts[i].Place = parts[i].Part.Place;
            uint64_t end   = parts[i].Place + (parts[i].Part.Memory + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE * VMM_PAGE_SIZE;
            if (end > image_end) image_end = end;
        }
    }

    error_t rc = OK;
    for (unsigned i = 0; i < BOX_IMAGE_PARTS && rc == OK; i++)
        if (parts[i].Present)
            rc = BoxImageRead(load, &parts[i]);

    uint64_t base = 0;
    if (rc == OK)
        rc = BoxImageBase(load, parts, fixups_part != NULL, &base);
    if (rc == OK && fixups_part)
        BoxImageFix(parts, fixups->Bytes, fixups_part->Size, base);
    for (unsigned i = 0; i < BOX_IMAGE_PARTS && rc == OK; i++)
        if (parts[i].Present)
            rc = BoxImageMap(load, &parts[i], base);

    if (rc == OK) {
        loaded->Entry = (uintptr_t)(base + start);
        BoxImagePart *names = &parts[BOX_IMAGE_NAMES];
        if (names->Present && names->Pages > 0 && names->Part.Size >= sizeof(NameplateHeader)) {
            const NameplateHeader *header = (const NameplateHeader *)vmm_phys_to_virt(names->Frames[0]);
            if (NameplateHeaderValid(header, names->Part.Size)) {
                loaded->NameplateVa    = (uintptr_t)(base + names->Place);
                loaded->NameplateBytes = header->TotalBytes;
            }
        }
        debug_printf("[IMAGE] '%s': a box at 0x%llx, %s\n", load->Name, (unsigned long long)base,
                     fixups_part ? "placed where the kernel chose" : "where it was linked");
    }
    BoxImageLetGo(parts);
    return rc;
}

error_t BoxLoad(vmm_context_t *vmm, const ImageSource *source, const char *name, ImageLoaded *loaded)
{
    BoxLoading load;
    memset(&load, 0, sizeof(load));
    load.Vmm    = vmm;
    load.Source = source;
    load.Name   = name;
    load.Hasher = (BoxHasher){ &load.HashState, BoxHashBegin, BoxHashAdd, BoxHashEnd };

    /* The header, then the start of the file to the end of the names. */
    uint8_t  head_bytes[BOX_HEADER_BYTES];
    uint64_t look = source->Bytes < BOX_HEADER_BYTES ? source->Bytes : BOX_HEADER_BYTES;
    error_t  rc   = ImageSourceRead(source, 0, head_bytes, look, name);
    if (rc != OK)
        return rc;
    BoxHead    head;
    BoxVerdict verdict = BoxReadHead(head_bytes, look, source->Bytes, &head);
    if (verdict != BOX_OK)
        return BoxRefuse(&load, verdict, NULL);

    BoxHeld prefix;
    rc = BoxHold(&load, 0, head.PrefixBytes, &prefix);
    if (rc != OK)
        return rc;
    verdict = BoxRead(prefix.Bytes, head.PrefixBytes, &head, &load.Table);
    if (verdict != BOX_OK) {
        BoxPart refused = { 0 };
        refused.Name       = load.Table.RefusedName;
        refused.NameLength = load.Table.RefusedNameLength;
        rc = BoxRefuse(&load, verdict, refused.Name ? &refused : NULL);
        BoxLetGo(&prefix);
        return rc;
    }
    if (!BoxSealHolds(&load.Table, &load.Hasher)) {
        BoxLetGo(&prefix);
        return BoxRefuse(&load, BOX_SEAL, NULL);
    }

    /* The parts it is started by, each read whole and held to its hash. */
    BoxPart part, fixups_part;
    BoxHeld held = { 0 }, fixups = { 0 };
    bool    movable = false;
    uint64_t start  = 0;

    if (BoxPartOfKind(&load.Table, BOX_KIND_NEEDS, &part)) {
        rc = BoxHoldPart(&load, &part, &held);
        if (rc == OK)
            rc = BoxCheckNeeds(&load, &part, held.Bytes);
        BoxLetGo(&held);
    }
    if (rc == OK && BoxPartOfKind(&load.Table, BOX_KIND_DOORS, &part)) {
        BoxDoors doors;
        rc = BoxHoldPart(&load, &part, &held);
        if (rc == OK && (verdict = BoxDoorsRead(held.Bytes, part.Size, &load.Table, &doors)) != BOX_OK)
            rc = BoxRefuse(&load, verdict, &part);
        if (rc == OK && !BoxDoorFind(&doors, BOX_DOOR_START, &start))
            rc = BoxRefuse(&load, BOX_NO_START, &part);
        BoxLetGo(&held);
    }
    if (rc == OK && BoxPartOfKind(&load.Table, BOX_KIND_THREAD, &part)) {
        BoxThread thread;
        rc = BoxHoldPart(&load, &part, &held);
        if (rc == OK && (verdict = BoxThreadRead(held.Bytes, part.Size, &thread)) != BOX_OK)
            rc = BoxRefuse(&load, verdict, &part);
        BoxLetGo(&held);
    }
    if (rc == OK && BoxPartOfKind(&load.Table, BOX_KIND_FIXUPS, &fixups_part)) {
        movable = true;
        rc = BoxHoldPart(&load, &fixups_part, &fixups);
        if (rc == OK && (verdict = BoxFixupsCheck(fixups.Bytes, fixups_part.Size, &load.Table)) != BOX_OK)
            rc = BoxRefuse(&load, verdict, &fixups_part);
    }

    if (rc == OK)
        rc = BoxLoadImage(&load, loaded, start, &fixups, movable ? &fixups_part : NULL);

    BoxLetGo(&fixups);
    BoxLetGo(&prefix);
    return rc;
}
