#include "boxasm_link_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/sha256.h"
#include "box/string.h"

#include "box_pen.h"
#include "settings_reader.h"

/*
 * The link written as a box (src/include/box_format.h) by the one pen
 * (box_pen.h) — what tools/pack makes of a program the tree links, made here
 * from the link itself:
 *
 *   code, const, data   the segments of user.ld, each at its place from the base
 *   doors               the door `start`, where _start is
 *   fixups              every word that holds an address, the base added at start
 *   thread              the template every strand's TLS block is made from
 *   names               the Nameplate
 *   form, settings      the Form and the settings, loaded at start; recipe, the
 *                       Recipe — as text
 */

#define BOXASM_BOX_PARTS 11u

typedef struct {
    BoxPenPart Parts[BOXASM_BOX_PARTS];
    uint32_t   Count;
    uint8_t   *Made[BOXASM_BOX_PARTS];   /* the bytes made here for a part */
    uint32_t   MadeCount;
} BoxAsmBox;

int BoxAsmFixupAdd(BoxAsmFixups *Fixups, uint64_t Place)
{
    if (Fixups->Count == Fixups->Capacity) {
        size_t Capacity = Fixups->Capacity ? Fixups->Capacity * 2u : 64u;
        uint64_t *Fresh;

        if (Capacity > SIZE_MAX / sizeof(*Fresh))
            return -ERR_NO_MEMORY;
        Fresh = (uint64_t *)realloc(Fixups->Places, Capacity * sizeof(*Fresh));
        if (!Fresh)
            return -ERR_NO_MEMORY;
        Fixups->Places = Fresh;
        Fixups->Capacity = Capacity;
    }
    Fixups->Places[Fixups->Count++] = Place;
    return 0;
}

void BoxAsmFixupsRelease(BoxAsmFixups *Fixups)
{
    free(Fixups->Places);
    memset(Fixups, 0, sizeof(*Fixups));
}

static void BoxAsmSiftDown(uint64_t *Places, size_t At, size_t Count)
{
    for (;;) {
        size_t Larger = At;
        size_t Child = 2u * At + 1u;
        uint64_t Held;

        if (Child < Count && Places[Child] > Places[Larger])
            Larger = Child;
        if (Child + 1u < Count && Places[Child + 1u] > Places[Larger])
            Larger = Child + 1u;
        if (Larger == At)
            return;
        Held = Places[At];
        Places[At] = Places[Larger];
        Places[Larger] = Held;
        At = Larger;
    }
}

/* The places in rising order, as the fixups part has them: the relocations
 * that made them came object by object, section by section. Two relocations
 * that write one word are refused rather than made one fixup. */
static int BoxAsmFixupsOrder(BoxAsmLinker *Link)
{
    BoxAsmFixups *Fixups = &Link->Image.Fixups;
    size_t Index;

    for (Index = Fixups->Count / 2u; Index > 0; Index--)
        BoxAsmSiftDown(Fixups->Places, Index - 1u, Fixups->Count);
    for (Index = Fixups->Count; Index > 1; Index--) {
        uint64_t Held = Fixups->Places[0];

        Fixups->Places[0] = Fixups->Places[Index - 1u];
        Fixups->Places[Index - 1u] = Held;
        BoxAsmSiftDown(Fixups->Places, 0, Index - 1u);
    }
    for (Index = 1; Index < Fixups->Count; Index++) {
        if (Fixups->Places[Index] == Fixups->Places[Index - 1u]) {
            BoxAsmMessage Message;

            BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
            BoxAsmMessageText(&Message, "two relocations write the address at ");
            BoxAsmMessageHex(&Message, Fixups->Places[Index]);
            BoxAsmMessageText(&Message, " from the base");
            return -ERR_INVALID_ARGUMENT;
        }
    }
    return 0;
}

static void BoxAsmBoxPart(BoxAsmBox *Box, const char *Name, const uint8_t *Bytes, uint64_t Size,
                          uint64_t Memory, uint64_t Place, uint32_t Flags)
{
    Box->Parts[Box->Count++] = (BoxPenPart){ Name, Bytes, Size, Memory, Place, Flags,
                                             BOX_PART_VERSION };
}

/* Room for a part's bytes, freed with the box. */
static uint8_t *BoxAsmBoxMake(BoxAsmBox *Box, uint64_t Size)
{
    uint8_t *Bytes;

    if (Size > SIZE_MAX)
        return NULL;
    Bytes = (uint8_t *)malloc(Size ? (size_t)Size : 1u);
    if (Bytes)
        Box->Made[Box->MadeCount++] = Bytes;
    return Bytes;
}

static void BoxAsmBoxRelease(BoxAsmBox *Box)
{
    uint32_t Index;

    for (Index = 0; Index < Box->MadeCount; Index++)
        free(Box->Made[Index]);
}

static int BoxAsmBoxSegments(BoxAsmLinker *Link, BoxAsmBox *Box)
{
    static const char *const Names[BOXASM_SEGMENT_COUNT] = { "code", "const", "data" };
    const BoxAsmLinkImage *Image = &Link->Image;
    size_t Kind;

    for (Kind = 0; Kind < BOXASM_SEGMENT_COUNT; Kind++) {
        BoxAsmSegment Segment;
        uint8_t *Bytes;

        BoxAsmSegmentOf(Image, (BoxAsmSegmentKind)Kind, &Segment);
        if (Segment.Memory == 0)
            continue;
        if (!(Bytes = BoxAsmBoxMake(Box, Segment.FileBytes)))
            return -ERR_NO_MEMORY;
        BoxAsmSegmentCopy(Image, &Segment, Bytes);
        BoxAsmBoxPart(Box, Names[Kind], Bytes, Segment.FileBytes, Segment.Memory,
                      Segment.Address - Image->Base, BOX_FLAG_MUST_UNDERSTAND);
    }
    return 0;
}

/* The parts with a layout of their own: doors, fixups, thread. */
static int BoxAsmBoxLaidParts(BoxAsmLinker *Link, BoxAsmBox *Box)
{
    const BoxAsmLinkImage *Image = &Link->Image;
    const BoxAsmOutputSection *Tdata = &Image->Sections[BOXASM_SLOT_TDATA];
    BoxPenDoor Door = { BOX_DOOR_START, Image->Entry - Image->Base };
    uint64_t Size;
    uint8_t *Bytes;

    BoxPenDoors(&Door, 1, NULL, &Size);
    if (!(Bytes = BoxAsmBoxMake(Box, Size)))
        return -ERR_NO_MEMORY;
    BoxPenDoors(&Door, 1, Bytes, &Size);
    BoxAsmBoxPart(Box, "doors", Bytes, Size, Size, 0, BOX_FLAG_MUST_UNDERSTAND);

    /* A box that can go anywhere says so by its fixups part, empty or not. */
    if (!BoxPenFixups(Image->Fixups.Places, Image->Fixups.Count, NULL, &Size)) {
        BoxAsmMessageSet(Link->Error, Link->ErrorCapacity,
                         "the fixups are not whole words in rising order");
        return -ERR_INVALID_STATE;
    }
    if (!(Bytes = BoxAsmBoxMake(Box, Size)))
        return -ERR_NO_MEMORY;
    BoxPenFixups(Image->Fixups.Places, Image->Fixups.Count, Bytes, &Size);
    BoxAsmBoxPart(Box, "fixups", Bytes, Size, Size, 0, BOX_FLAG_MUST_UNDERSTAND);

    if (Image->TlsMemorySize != 0) {
        BoxPenThread(Image->TlsAlign, Tdata->Data.Data, Tdata->Size, Image->TlsMemorySize, NULL,
                     &Size);
        if (!(Bytes = BoxAsmBoxMake(Box, Size)))
            return -ERR_NO_MEMORY;
        BoxPenThread(Image->TlsAlign, Tdata->Data.Data, Tdata->Size, Image->TlsMemorySize, Bytes,
                     &Size);
        BoxAsmBoxPart(Box, "thread", Bytes, Size, Size, 0, BOX_FLAG_MUST_UNDERSTAND);
    }
    return 0;
}

/* The Form and the Recipe as the parts keep them: text, and none when there
 * is none. Zeros that end them, a C string's, are not the text's. */
static int BoxAsmBoxText(BoxAsmLinker *Link, BoxAsmBox *Box, const char *Name, BoxKind Kind,
                         const uint8_t *Bytes, uint64_t Size, uint32_t Flags)
{
    BoxText Text;

    while (Size > 0 && Bytes[Size - 1u] == 0)
        Size--;
    if (Size == 0)
        return 0;
    if (BoxTextBegin(Bytes, Size, Kind, &Text) != BOX_OK) {
        BoxAsmMessage Message;

        BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
        BoxAsmMessageText(&Message, "the program's ");
        BoxAsmMessageText(&Message, Name);
        BoxAsmMessageText(&Message, " is not text as a box keeps its part ");
        BoxAsmMessageText(&Message, Name);
        return -ERR_INVALID_ARGUMENT;
    }
    BoxAsmBoxPart(Box, Name, Bytes, Size, Size, 0, Flags);
    return 0;
}

/* Every setting the declaration says, read as the program will read it: one
 * it would pass over is refused, as assemble refuses it where it is written. */
static int BoxAsmBoxSettings(BoxAsmLinker *Link, const BoxAsmOutputSection *Section)
{
    SettingsDeclared Declared;
    Setting Setting;
    SettingsVerdict Verdict;
    BoxAsmMessage Message;
    uint64_t Size = Section->Data.Size;

    while (Size > 0 && Section->Data.Data[Size - 1u] == 0)
        Size--;
    if (Size == 0)
        return 0;
    BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
    if (SettingsDeclaredBegin(Section->Data.Data, Size, &Declared) != SETTINGS_OK) {
        BoxAsmMessageText(&Message, "the program's settings do not begin with the line \"settings "
                                    SETTINGS_VERSION_WORD "\"");
        return -ERR_INVALID_ARGUMENT;
    }
    while (SettingsDeclaredNext(&Declared, &Setting, &Verdict)) {
        if (Verdict == SETTINGS_OK)
            continue;
        BoxAsmMessageText(&Message, "the program's settings, line ");
        BoxAsmMessageDecimal(&Message, Setting.Line);
        BoxAsmMessageText(&Message, Setting.Name.Length != 0 ? ": the setting " : ": the line");
        BoxAsmMessageSpan(&Message, (const char *)Setting.Name.At, (size_t)Setting.Name.Length);
        BoxAsmMessageText(&Message, " ");
        BoxAsmMessageText(&Message, SettingsVerdictText(Verdict));
        return -ERR_INVALID_ARGUMENT;
    }
    return 0;
}

/* How the program lives, read as whoever starts it reads it: a word that
 * reader would pass over is one nobody would ever hear, refused here. */
static int BoxAsmBoxLife(BoxAsmLinker *Link, const BoxAsmOutputSection *Section)
{
    BoxAsmMessage Message;
    BoxLife Life;
    uint64_t Unknown;
    uint64_t Size = Section->Data.Size;

    while (Size > 0 && Section->Data.Data[Size - 1u] == 0)
        Size--;
    if (Size == 0)
        return 0;
    BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
    if (BoxLifeRead(Section->Data.Data, Size, &Life, &Unknown) != BOX_OK) {
        BoxAsmMessageText(&Message, "the program's life is not written as one: one of task, stays and serves at "
                                    "most, rise at most once, a word a line");
        return -ERR_INVALID_ARGUMENT;
    }
    if (Unknown != 0) {
        BoxAsmMessageText(&Message, "the program's life, line ");
        BoxAsmMessageDecimal(&Message, (uint32_t)Unknown);
        BoxAsmMessageText(&Message, ": no word of how a program lives — task, stays, serves or rise");
        return -ERR_INVALID_ARGUMENT;
    }
    return 0;
}

static void BoxAsmHashBegin(void *State)
{
    sha256_begin((Sha256 *)State);
}

static void BoxAsmHashAdd(void *State, const uint8_t *Bytes, uint64_t Count)
{
    sha256_add((Sha256 *)State, Bytes, (size_t)Count);
}

static void BoxAsmHashEnd(void *State, uint8_t Digest[BOX_HASH_BYTES])
{
    sha256_end((Sha256 *)State, Digest);
}

/* The parts put into a box by the pen, which reads back what it wrote with
 * the reader the kernel reads it with. */
static int BoxAsmBoxWrite(BoxAsmLinker *Link, BoxAsmBox *Box, BoxAsmBlob *Out)
{
    Sha256 State;
    BoxHasher Hasher = { &State, BoxAsmHashBegin, BoxAsmHashAdd, BoxAsmHashEnd };
    uint32_t Refused = BOX_NO_PART;
    uint64_t Bytes;
    uint8_t *Written = NULL;
    BoxVerdict Verdict = BoxPenMeasure(Box->Parts, Box->Count, &Bytes);
    BoxAsmMessage Message;

    if (Verdict == BOX_OK) {
        if (Bytes > SIZE_MAX || !(Written = (uint8_t *)malloc((size_t)Bytes)))
            return -ERR_NO_MEMORY;
        Verdict = BoxPenWrite(Box->Parts, Box->Count, Written, Bytes, &Hasher, &Refused);
    }
    if (Verdict == BOX_OK) {
        Out->Data = Written;
        Out->Size = Bytes;
        return 0;
    }
    free(Written);
    BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
    BoxAsmMessageText(&Message, "the box would not be read: ");
    BoxAsmMessageText(&Message, BoxVerdictText(Verdict));
    if (Refused < Box->Count) {
        BoxAsmMessageText(&Message, " (part ");
        BoxAsmMessageText(&Message, Box->Parts[Refused].Name);
        BoxAsmMessageText(&Message, ")");
    }
    return -ERR_INVALID_ARGUMENT;
}

int BoxAsmWriteBox(BoxAsmLinker *Link, BoxAsmBlob *Out)
{
    const BoxAsmOutputSection *Names = &Link->Image.Sections[BOXASM_SLOT_NAMEPLATE];
    const BoxAsmOutputSection *Form = &Link->Image.Sections[BOXASM_SLOT_FORM];
    const BoxAsmOutputSection *Settings = &Link->Image.Sections[BOXASM_SLOT_SETTINGS];
    const BoxAsmOutputSection *Life = &Link->Image.Sections[BOXASM_SLOT_LIFE];
    BoxAsmBox Box;
    int Rc;

    memset(&Box, 0, sizeof(Box));
    Rc = BoxAsmFixupsOrder(Link);
    if (Rc == 0)
        Rc = BoxAsmBoxSegments(Link, &Box);
    if (Rc == 0)
        Rc = BoxAsmBoxLaidParts(Link, &Box);
    if (Rc == 0 && Names->Size != 0)
        BoxAsmBoxPart(&Box, "names", Names->Data.Data, Names->Size, Names->Size, 0, 0);
    if (Rc == 0)
        Rc = BoxAsmBoxText(Link, &Box, "form", BOX_KIND_FORM, Form->Data.Data, Form->Size,
                           BOX_FLAG_LOAD_AT_START);
    if (Rc == 0)
        Rc = BoxAsmBoxSettings(Link, Settings);
    if (Rc == 0)
        Rc = BoxAsmBoxText(Link, &Box, "settings", BOX_KIND_SETTINGS, Settings->Data.Data, Settings->Size,
                           BOX_FLAG_LOAD_AT_START);
    if (Rc == 0)
        Rc = BoxAsmBoxLife(Link, Life);
    if (Rc == 0)
        Rc = BoxAsmBoxText(Link, &Box, "life", BOX_KIND_LIFE, Life->Data.Data, Life->Size, 0);
    if (Rc == 0 && Link->Recipe)
        Rc = BoxAsmBoxText(Link, &Box, "recipe", BOX_KIND_RECIPE, Link->Recipe->Data,
                           Link->Recipe->Size, 0);
    if (Rc == 0)
        Rc = BoxAsmBoxWrite(Link, &Box, Out);
    BoxAsmBoxRelease(&Box);
    return Rc;
}
