#include "box_load.h"
#include "box_reader.h"
#include "box_needs.h"
#include "box_loaded.h"
#include "auth_tags.h"
#include "nameplate_format.h"
#include "cabin_layout.h"
#include "kernel_config.h"
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

/* One load: the box's table, read from its start, what reads it, and the
 * template of its thread-local storage (its `thread` part, held). */
typedef struct {
    vmm_context_t     *Vmm;
    const ImageSource *Source;
    const char        *Name;
    KSha256Ctx         HashState;
    BoxHasher          Hasher;
    BoxTable           Table;
    ImageThread        Thread;
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

/* ── hints ──────────────────────────────────────────────────────────────── */

/* The main stack's reserve for a box that asks `asked` bytes of it: whole
 * pages, between the least and the most the system gives a main stack. A hint
 * prepares; it raises no limit — what is cut is said. */
static uint64_t BoxStackPrepared(const BoxLoading *load, uint64_t asked)
{
    uint64_t reserve;
    if (!BoxAlignUp(asked, VMM_PAGE_SIZE, CONFIG_USER_STACK_RESERVE_MAX, &reserve)) {
        kprintf("[IMAGE] '%s' asks for a stack of %llu KiB; a main stack is given at most %llu KiB, "
                "and that is what it has\n", load->Name, (unsigned long long)(asked >> 10),
                (unsigned long long)(CONFIG_USER_STACK_RESERVE_MAX >> 10));
        return CONFIG_USER_STACK_RESERVE_MAX;
    }
    if (reserve < CONFIG_USER_STACK_RESERVE_MIN) {
        kprintf("[IMAGE] '%s' asks for a stack of %llu bytes; a main stack is given at least %llu KiB, "
                "and that is what it has\n", load->Name, (unsigned long long)asked,
                (unsigned long long)(CONFIG_USER_STACK_RESERVE_MIN >> 10));
        return CONFIG_USER_STACK_RESERVE_MIN;
    }
    return reserve;
}

/* What the box's `hints` part asks, prepared within what the system gives: the
 * main stack's reserve into *stack, and the memory it says it uses held to
 * what a cabin's heap holds — said when it is more, changing nothing, for this
 * system keeps no other limit on memory yet. A hint this system does not know
 * is passed over; one it knows is read strictly, and said once. */
static error_t BoxCheckHints(BoxLoading *load, const BoxPart *part, const uint8_t *bytes, uint64_t *stack)
{
    BoxText    text;
    BoxVerdict verdict = BoxTextBegin(bytes, part->Size, BOX_KIND_HINTS, &text);
    if (verdict != BOX_OK)
        return BoxRefuse(load, verdict, part);

    bool           said_stack = false, said_memory = false;
    const uint8_t *line, *word, *number, *unit;
    uint64_t       length, word_length, number_length, unit_length, asked;
    while (BoxTextLine(&text, &line, &length)) {
        uint64_t at = 0;
        BoxTextWord(line, length, &at, &word, &word_length);
        bool is_stack  = BoxSpanIs(word, word_length, "stack");
        bool is_memory = BoxSpanIs(word, word_length, "memory");
        if (!is_stack && !is_memory)
            continue;   /* a hint this system does not know: passed over */
        if (BoxTextWordCount(line, length) != 3 || (is_stack ? said_stack : said_memory))
            return BoxRefuse(load, BOX_TEXT_LAYOUT, part);
        BoxTextWord(line, length, &at, &number, &number_length);
        BoxTextWord(line, length, &at, &unit, &unit_length);
        if (!BoxTextSize(number, number_length, unit, unit_length, &asked)) {
            kprintf("[IMAGE] '%s' is not started: its hints say '%.*s', and that is no size this "
                    "system reads\n", load->Name, (int)length, (const char *)line);
            return ERR_INVALID_ELF;
        }
        if (is_stack) {
            said_stack = true;
            *stack     = BoxStackPrepared(load, asked);
        } else {
            said_memory = true;
            if (asked > CABIN_HEAP_MAX_SIZE)
                kprintf("[IMAGE] '%s' says it uses %llu MiB of memory; a cabin's heap holds %llu MiB\n",
                        load->Name, (unsigned long long)(asked >> 20),
                        (unsigned long long)(CABIN_HEAP_MAX_SIZE >> 20));
        }
    }
    return OK;
}

/* ── the image ──────────────────────────────────────────────────────────── */

/*
 * One part of the box in memory: code, const, data, the names, a part carried
 * in, or the table of them all. Its pages are taken one at a time — no part
 * needs memory that happens to be in one piece — the frame of each kept in an
 * array in pages of its own.
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

#define BOX_RIGHTS_READ (VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE)

static uint64_t BoxPagesOf(uint64_t bytes)
{
    return (bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
}

/* What a part not mapped still holds, given back. */
static void BoxImageLetGo(BoxImagePart *part)
{
    for (uint64_t page = part->Mapped; page < part->Taken; page++)
        pmm_free((void *)part->Frames[page], 1);
    if (part->FramesHeld)
        pmm_free(part->FramesHeld, part->FramesPages);
    part->FramesHeld = NULL;
    part->Taken = part->Mapped = 0;
}

/* `pages` zeroed pages for the part, and the list of their frames. */
static error_t BoxImageTake(BoxLoading *load, BoxImagePart *part, uint64_t pages)
{
    part->Pages       = pages;
    part->FramesPages = (size_t)BoxPagesOf(pages * sizeof(uintptr_t));
    if (pages == 0)
        return OK;
    part->FramesHeld = pmm_alloc(part->FramesPages);
    if (!part->FramesHeld) {
        kprintf("[IMAGE] '%s' is not started: no room to list the %llu pages it takes at 0x%llx\n",
                load->Name, (unsigned long long)pages, (unsigned long long)part->Place);
        return ERR_NO_MEMORY;
    }
    part->Frames = (uintptr_t *)vmm_phys_to_virt((uintptr_t)part->FramesHeld);
    for (uint64_t page = 0; page < pages; page++) {
        void *frame = pmm_alloc_zero(1);
        if (!frame) {
            kprintf("[IMAGE] '%s' is not started: memory has no page left for it\n", load->Name);
            return ERR_NO_MEMORY;
        }
        part->Frames[page] = (uintptr_t)frame;
        part->Taken        = page + 1;
    }
    return OK;
}

/* The part's pages taken and filled straight from the file, its hash taken as
 * the bytes come and held to the table's. */
static error_t BoxImageRead(BoxLoading *load, BoxImagePart *part)
{
    error_t rc = BoxImageTake(load, part, BoxPagesOf(part->Part.Memory));
    if (rc != OK)
        return rc;

    uint8_t digest[BOX_HASH_BYTES];
    load->Hasher.Begin(load->Hasher.State);
    for (uint64_t from = 0; from < part->Part.Size; from += VMM_PAGE_SIZE) {
        uint64_t count = part->Part.Size - from < VMM_PAGE_SIZE ? part->Part.Size - from : VMM_PAGE_SIZE;
        uint8_t *bytes = (uint8_t *)vmm_phys_to_virt(part->Frames[from / VMM_PAGE_SIZE]);
        rc = ImageSourceRead(load->Source, part->Part.Offset + from, bytes, count, load->Name);
        if (rc != OK)
            return rc;
        load->Hasher.Add(load->Hasher.State, bytes, count);
    }
    load->Hasher.End(load->Hasher.State, digest);
    if (!BoxSameHash(digest, part->Part.Hash))
        return BoxRefuse(load, BOX_PART_HASH, &part->Part);
    return OK;
}

/* `count` bytes into the part's pages from `at` on, across their seams. */
static void BoxImagePut(const BoxImagePart *part, uint64_t at, const void *bytes, uint64_t count)
{
    const uint8_t *from = (const uint8_t *)bytes;
    while (count > 0) {
        uint64_t within = at % VMM_PAGE_SIZE;
        uint64_t piece  = VMM_PAGE_SIZE - within < count ? VMM_PAGE_SIZE - within : count;
        memcpy((uint8_t *)vmm_phys_to_virt(part->Frames[at / VMM_PAGE_SIZE]) + within, from, piece);
        at    += piece;
        from  += piece;
        count -= piece;
    }
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
            kprintf("[IMAGE] '%s' is not started: its page at 0x%llx was not mapped: %s\n",
                    load->Name, (unsigned long long)at, mapped.error_msg);
            return ERR_SPAWN_FAILED;
        }
        part->Mapped = page + 1;
    }
    return OK;
}

/* ── where everything goes ──────────────────────────────────────────────── */

/*
 * Everything of the box that goes into memory, placed from the base before a
 * byte of it is read: code, const and data where their places say; on the
 * pages after them the names; after those each part carried in, on pages of
 * its own, in the order of the box's table; then the table of what is where
 * (box_loaded.h); and last the main strand's thread-local storage.
 */
typedef struct {
    BoxImagePart Parts[BOX_IMAGE_PARTS];
    uint64_t     Low;          /* the image's first page */
    uint64_t     CarriedAt;    /* the first page after the names */
    uint64_t     TableAt;
    uint64_t     TableBytes;
    uint64_t     ThreadAt;     /* the main strand's thread-local storage */
    uint64_t     End;          /* past the last page of it all */
    uint32_t     Listed;       /* the parts the table names */
} BoxPlan;

/* Every page of it lies in the room a program's image has, under 4 GiB from
 * the base: an offset or a length in the table fits its 32 bits. */
_Static_assert(CABIN_CODE_END_ADDR - CABIN_CODE_START_ADDR <= 0xFFFFFFFFull,
               "the table of loaded parts measures its image in 32 bits");
_Static_assert(BOX_LOADED_SEAL_BYTES == BOX_HASH_BYTES, "the table carries the box's seal");

/* `bytes` more, on whole pages, after `*at` — unless that reaches past where
 * any image can. */
static bool BoxPlanAdd(uint64_t *at, uint64_t bytes)
{
    uint64_t span;
    if (!BoxAlignUp(bytes, VMM_PAGE_SIZE, BOX_PLACE_LIMIT, &span) || span > BOX_PLACE_LIMIT - *at)
        return false;
    *at += span;
    return true;
}

/* The image part that is the box's part `index`, if one is. */
static BoxImagePart *BoxPlanFixed(BoxPlan *plan, uint32_t index)
{
    for (unsigned i = 0; i < BOX_IMAGE_PARTS; i++)
        if (plan->Parts[i].Present && plan->Parts[i].Part.Index == index)
            return &plan->Parts[i];
    return NULL;
}

static error_t BoxPlanLay(BoxLoading *load, BoxPlan *plan)
{
    static const BoxKind  kinds[BOX_IMAGE_PARTS]  = { BOX_KIND_CODE, BOX_KIND_CONST, BOX_KIND_DATA,
                                                      BOX_KIND_NAMES };
    static const uint64_t rights[BOX_IMAGE_PARTS] = {
        VMM_FLAG_PRESENT | VMM_FLAG_USER,
        BOX_RIGHTS_READ,
        VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NO_EXECUTE,
        BOX_RIGHTS_READ,
    };

    /* The reader holds code, const and data below the limit, each from a
     * page of its own; code is always there. */
    uint64_t image_end = 0;
    plan->Low = BOX_PLACE_LIMIT;
    for (unsigned i = 0; i < BOX_IMAGE_PARTS; i++) {
        BoxImagePart *part = &plan->Parts[i];
        part->Rights  = rights[i];
        part->Present = BoxPartOfKind(&load->Table, kinds[i], &part->Part);
        if (!part->Present || i == BOX_IMAGE_NAMES)
            continue;
        part->Place  = part->Part.Place;
        uint64_t end = part->Place + BoxPagesOf(part->Part.Memory) * VMM_PAGE_SIZE;
        if (part->Place < plan->Low) plan->Low = part->Place;
        if (end > image_end) image_end = end;
    }

    BoxImagePart *names = &plan->Parts[BOX_IMAGE_NAMES];
    names->Place     = image_end;
    plan->CarriedAt  = image_end;
    bool     fits    = !names->Present || BoxPlanAdd(&plan->CarriedAt, names->Part.Size);
    uint64_t at      = plan->CarriedAt;
    uint64_t letters = 0;
    for (uint32_t i = 0; i < load->Table.Head.PartCount && fits; i++) {
        BoxPart part;
        BoxPartAt(&load->Table, i, &part);
        if (!BoxPlanFixed(plan, i)) {
            if ((part.Flags & BOX_FLAG_LOAD_AT_START) == 0)
                continue;
            fits = BoxPlanAdd(&at, part.Size);
        }
        plan->Listed++;
        letters += part.NameLength;
    }

    plan->TableAt    = at;
    plan->TableBytes = sizeof(BoxLoadedHead) + (uint64_t)plan->Listed * sizeof(BoxLoadedPart) + letters;
    plan->ThreadAt   = at;
    fits             = fits && BoxPlanAdd(&plan->ThreadAt, plan->TableBytes);
    plan->End        = plan->ThreadAt;
    if (!fits || !BoxPlanAdd(&plan->End, ImageThreadPages(&load->Thread) * VMM_PAGE_SIZE)) {
        kprintf("[IMAGE] '%s' is not started: what it puts into memory reaches past where any "
                "image can\n", load->Name);
        return ERR_BINARY_TOO_LARGE;
    }
    return OK;
}

/* Where it all goes: anywhere in the room a program's image has, chosen at
 * random, when the box carries fixups; where its places say, which must lie in
 * that room, when it does not. */
static error_t BoxPlanBase(BoxLoading *load, const BoxPlan *plan, bool movable, uint64_t *base)
{
    uint64_t room = CABIN_CODE_END_ADDR - CABIN_CODE_START_ADDR;
    if (movable) {
        if (plan->End > room) {
            kprintf("[IMAGE] '%s' is not started: what it puts into memory, %llu bytes, is larger "
                    "than the room a program's image has (%llu)\n", load->Name,
                    (unsigned long long)plan->End, (unsigned long long)room);
            return ERR_BINARY_TOO_LARGE;
        }
        *base = CABIN_CODE_START_ADDR + aslr_random_offset(room - plan->End + VMM_PAGE_SIZE);
        return OK;
    }
    if (plan->Low < CABIN_CODE_START_ADDR || plan->End > CABIN_CODE_END_ADDR) {
        kprintf("[IMAGE] '%s' is not started: it cannot be moved, and what it puts into memory at "
                "0x%llx-0x%llx lies outside the room a program's image has\n", load->Name,
                (unsigned long long)plan->Low, (unsigned long long)plan->End);
        return ERR_INVALID_ELF;
    }
    *base = 0;
    return OK;
}

/*
 * The parts carried in and the table of what is where, on the pages after the
 * names: each part the box marks to be loaded at start, read from the file,
 * held to its hash and mapped read-only; then the table naming those and the
 * image's parts, in the order of the box's table, mapped read-only after them.
 */
static error_t BoxCarry(BoxLoading *load, BoxPlan *plan, uint64_t base)
{
    BoxImagePart table;
    memset(&table, 0, sizeof(table));
    table.Place  = plan->TableAt;
    table.Rights = BOX_RIGHTS_READ;
    error_t rc   = BoxImageTake(load, &table, BoxPagesOf(plan->TableBytes));

    BoxLoadedHead head = { plan->Listed, (uint32_t)plan->TableBytes, { 0 } };
    memcpy(head.Seal, load->Table.Prefix + BOX_AT_SEAL, BOX_HASH_BYTES);
    if (rc == OK)
        BoxImagePut(&table, 0, &head, sizeof(head));

    uint64_t entry_at = sizeof(head);
    uint64_t name_at  = entry_at + (uint64_t)plan->Listed * sizeof(BoxLoadedPart);
    uint64_t carried  = plan->CarriedAt;
    for (uint32_t i = 0; i < load->Table.Head.PartCount && rc == OK; i++) {
        BoxImagePart  carry;
        BoxImagePart *part = BoxPlanFixed(plan, i);
        if (!part) {
            memset(&carry, 0, sizeof(carry));
            BoxPartAt(&load->Table, i, &carry.Part);
            if ((carry.Part.Flags & BOX_FLAG_LOAD_AT_START) == 0)
                continue;
            carry.Present = true;
            carry.Place   = carried;
            carry.Rights  = BOX_RIGHTS_READ;
            rc = BoxImageRead(load, &carry);
            if (rc == OK)
                rc = BoxImageMap(load, &carry, base);
            BoxImageLetGo(&carry);
            if (rc != OK)
                break;
            carried += carry.Pages * VMM_PAGE_SIZE;
            part = &carry;
        }
        BoxLoadedPart entry = { base + part->Place, part->Part.Memory, (uint32_t)name_at,
                                part->Part.NameLength, part->Part.Version };
        BoxImagePut(&table, entry_at, &entry, sizeof(entry));
        BoxImagePut(&table, name_at, part->Part.Name, part->Part.NameLength);
        entry_at += sizeof(entry);
        name_at  += part->Part.NameLength;
    }

    if (rc == OK)
        rc = BoxImageMap(load, &table, base);
    BoxImageLetGo(&table);
    return rc;
}

/* ── the box ────────────────────────────────────────────────────────────── */

static error_t BoxLoadImage(BoxLoading *load, ImageLoaded *loaded, uint64_t start, const BoxHeld *fixups,
                            const BoxPart *fixups_part)
{
    BoxPlan plan;
    memset(&plan, 0, sizeof(plan));

    uint64_t base = 0;
    error_t  rc   = BoxPlanLay(load, &plan);
    if (rc == OK)
        rc = BoxPlanBase(load, &plan, fixups_part != NULL, &base);
    for (unsigned i = 0; i < BOX_IMAGE_PARTS && rc == OK; i++)
        if (plan.Parts[i].Present)
            rc = BoxImageRead(load, &plan.Parts[i]);
    if (rc == OK && fixups_part)
        BoxImageFix(plan.Parts, fixups->Bytes, fixups_part->Size, base);
    for (unsigned i = 0; i < BOX_IMAGE_PARTS && rc == OK; i++)
        if (plan.Parts[i].Present)
            rc = BoxImageMap(load, &plan.Parts[i], base);
    if (rc == OK)
        rc = BoxCarry(load, &plan, base);
    if (rc == OK && load->Thread.TotalBytes > 0)
        rc = ImageThreadPlace(load->Vmm, &load->Thread, (uintptr_t)(base + plan.ThreadAt), load->Name,
                              &loaded->ThreadPointer);
    if (rc == OK)
        rc = ImageThreadKeep(&load->Thread, load->Name, &loaded->Thread);

    if (rc == OK) {
        loaded->Entry     = (uintptr_t)(base + start);
        loaded->ImageBase = (uintptr_t)base;
        loaded->PartsVa   = (uintptr_t)(base + plan.TableAt);
        BoxImagePart *names = &plan.Parts[BOX_IMAGE_NAMES];
        if (names->Present && names->Pages > 0 && names->Part.Size >= sizeof(NameplateHeader)) {
            const NameplateHeader *header = (const NameplateHeader *)vmm_phys_to_virt(names->Frames[0]);
            if (NameplateHeaderValid(header, names->Part.Size)) {
                loaded->NameplateVa    = (uintptr_t)(base + names->Place);
                loaded->NameplateBytes = header->TotalBytes;
            }
        }
        debug_printf("[IMAGE] '%s': a box at 0x%llx, %s; %u parts named at 0x%llx\n", load->Name,
                     (unsigned long long)base,
                     fixups_part ? "placed where the kernel chose" : "where it was linked",
                     (unsigned)plan.Listed, (unsigned long long)loaded->PartsVa);
    }
    for (unsigned i = 0; i < BOX_IMAGE_PARTS; i++)
        BoxImageLetGo(&plan.Parts[i]);
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
    BoxHeld held = { 0 }, fixups = { 0 }, thread = { 0 };
    bool    movable = false;
    uint64_t start  = 0;

    if (BoxPartOfKind(&load.Table, BOX_KIND_NEEDS, &part)) {
        rc = BoxHoldPart(&load, &part, &held);
        if (rc == OK)
            rc = BoxCheckNeeds(&load, &part, held.Bytes);
        BoxLetGo(&held);
    }
    if (rc == OK && BoxPartOfKind(&load.Table, BOX_KIND_HINTS, &part)) {
        rc = BoxHoldPart(&load, &part, &held);
        if (rc == OK)
            rc = BoxCheckHints(&load, &part, held.Bytes, &loaded->StackReserve);
        BoxLetGo(&held);
    }
    /* How it lives, for the one word the system acts on: rise (rise.h). */
    if (rc == OK && BoxPartOfKind(&load.Table, BOX_KIND_LIFE, &part)) {
        BoxLife life;
        rc = BoxHoldPart(&load, &part, &held);
        if (rc == OK && (verdict = BoxLifeRead(held.Bytes, part.Size, &life, NULL)) != BOX_OK)
            rc = BoxRefuse(&load, verdict, &part);
        if (rc == OK)
            loaded->Rise = life.Rise != 0;
        BoxLetGo(&held);
    }
    /* What it asks to be allowed (box_reader.h); what it does not ask it is not
     * given, and `system` is not its to ask. */
    if (rc == OK && BoxPartOfKind(&load.Table, BOX_KIND_ASKS, &part)) {
        BoxAsks asks;
        rc = BoxHoldPart(&load, &part, &held);
        if (rc == OK && (verdict = BoxAsksRead(held.Bytes, part.Size, &asks, NULL)) != BOX_OK)
            rc = BoxRefuse(&load, verdict, &part);
        if (rc == OK)
            loaded->Asks = (asks.Utility ? AUTH_TAG_UTILITY : 0u) | (asks.Network ? AUTH_TAG_NETWORK : 0u);
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
        BoxThread layout;
        rc = BoxHoldPart(&load, &part, &thread);
        if (rc == OK && (verdict = BoxThreadRead(thread.Bytes, part.Size, &layout)) != BOX_OK)
            rc = BoxRefuse(&load, verdict, &part);
        if (rc == OK)
            load.Thread = (ImageThread){ layout.Align, layout.InitBytes, layout.TotalBytes,
                                         layout.Init };
    }
    if (rc == OK && BoxPartOfKind(&load.Table, BOX_KIND_FIXUPS, &fixups_part)) {
        movable = true;
        rc = BoxHoldPart(&load, &fixups_part, &fixups);
        if (rc == OK && (verdict = BoxFixupsCheck(fixups.Bytes, fixups_part.Size, &load.Table)) != BOX_OK)
            rc = BoxRefuse(&load, verdict, &fixups_part);
    }

    if (rc == OK)
        rc = BoxLoadImage(&load, loaded, start, &fixups, movable ? &fixups_part : NULL);

    BoxLetGo(&thread);
    BoxLetGo(&fixups);
    BoxLetGo(&prefix);
    return rc;
}
