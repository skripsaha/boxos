#include "boxasm_link_internal.h"

#include "nameplate_format.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

typedef int (*BoxAsmSymbolVisitor)(BoxAsmLinker *Link,
                                   const BoxAsmLinkObject *Object,
                                   uint32_t Index, uint64_t Address,
                                   void *Context);

/* The symbols an image carries, as GNU ld writes them into its .symtab: every
 * named symbol of a selected object that lies in an emitted section, and of
 * the globals only the definition that prevails for each name. Section and
 * file symbols name no address. Locals or globals alone, in object order. */
static int BoxAsmVisitImageSymbols(BoxAsmLinker *Link, uint8_t Globals,
                                   BoxAsmSymbolVisitor Visit, void *Context)
{
    size_t ObjectIndex;

    for (ObjectIndex = 0; ObjectIndex < Link->Objects.Count; ObjectIndex++) {
        const BoxAsmLinkObject *Object = &Link->Objects.Items[ObjectIndex];
        uint32_t Index;

        if (!Object->Selected)
            continue;
        for (Index = 1; Index < Object->Elf.SymbolCount; Index++) {
            const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[Index];
            const char *Name = BoxAsmElfSymbolName(&Object->Elf, Index);
            uint8_t Type = BOXASM_SYMBOL_TYPE(Symbol->Info);
            uint8_t Global = BOXASM_SYMBOL_BIND(Symbol->Info) != BOXASM_STB_LOCAL;
            uint64_t Address;
            int Rc;

            if (Type == BOXASM_STT_SECTION || Type == BOXASM_STT_FILE ||
                Symbol->SectionIndex == BOXASM_SHN_UNDEF ||
                Symbol->SectionIndex == BOXASM_SHN_ABS ||
                Symbol->SectionIndex == BOXASM_SHN_COMMON ||
                !Name || Name[0] == '\0' || Global != Globals ||
                Object->SectionSlot[Symbol->SectionIndex] == BOXASM_SLOT_NONE)
                continue;
            if (Global && !BoxAsmSymbolPrevails(Link, (uint32_t)ObjectIndex, Index))
                continue;
            Rc = BoxAsmSymbolAddress(Link, Object, Symbol, &Address);
            if (Rc == 0)
                Rc = Visit(Link, Object, Index, Address, Context);
            if (Rc != 0)
                return Rc;
        }
    }
    return 0;
}

/* ---- Nameplate --------------------------------------------------------- */

typedef struct {
    uint64_t    Address;
    uint64_t    Length;
    const char *Name;
    uint8_t     Typed;
    uint8_t     Global;
} BoxAsmNameEntry;

typedef struct {
    BoxAsmNameEntry *Items;
    size_t           Count;
    size_t           Capacity;
} BoxAsmNameList;

/* The order tools/nameplate gives: by address, and at one address the
 * function before the bare label, the sized before the unsized, the global
 * before the local, then by name — so the winner never depends on input
 * order and two builds of one program carry one table. */
static int BoxAsmNameCompare(const BoxAsmNameEntry *Left, const BoxAsmNameEntry *Right)
{
    if (Left->Address != Right->Address)
        return Left->Address < Right->Address ? -1 : 1;
    if (Left->Typed != Right->Typed)
        return (int)Right->Typed - (int)Left->Typed;
    if ((Left->Length != 0) != (Right->Length != 0))
        return (int)(Right->Length != 0) - (int)(Left->Length != 0);
    if (Left->Global != Right->Global)
        return (int)Right->Global - (int)Left->Global;
    return strcmp(Left->Name, Right->Name);
}

static int BoxAsmNameSort(BoxAsmNameList *List)
{
    BoxAsmNameEntry *Scratch;
    BoxAsmNameEntry *From = List->Items;
    BoxAsmNameEntry *To;
    size_t Width;

    if (List->Count < 2u)
        return 0;
    Scratch = (BoxAsmNameEntry *)malloc(List->Count * sizeof(BoxAsmNameEntry));
    if (!Scratch)
        return -ERR_NO_MEMORY;
    To = Scratch;
    for (Width = 1; Width < List->Count; Width *= 2u) {
        size_t Start;
        BoxAsmNameEntry *Swap;

        for (Start = 0; Start < List->Count; Start += 2u * Width) {
            size_t Middle = Start + Width < List->Count ? Start + Width : List->Count;
            size_t End = Middle + Width < List->Count ? Middle + Width : List->Count;
            size_t Left = Start;
            size_t Right = Middle;
            size_t At = Start;

            while (Left < Middle && Right < End)
                To[At++] = BoxAsmNameCompare(&From[Left], &From[Right]) <= 0
                           ? From[Left++] : From[Right++];
            while (Left < Middle)
                To[At++] = From[Left++];
            while (Right < End)
                To[At++] = From[Right++];
        }
        Swap = From;
        From = To;
        To = Swap;
    }
    if (From != List->Items)
        memcpy(List->Items, From, List->Count * sizeof(BoxAsmNameEntry));
    free(Scratch);
    return 0;
}

/* tools/nameplate's harvest: a function, or an untyped label, in executable
 * code, inside its own output section. Address 0 is a place like any other:
 * a box is linked at 0 and begins its code there, _start first. */
static int BoxAsmHarvestName(BoxAsmLinker *Link, const BoxAsmLinkObject *Object,
                             uint32_t Index, uint64_t Address, void *Context)
{
    BoxAsmNameList *List = (BoxAsmNameList *)Context;
    const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[Index];
    const BoxAsmOutputSection *Text = &Link->Image.Sections[BOXASM_SLOT_TEXT];
    uint8_t Type = BOXASM_SYMBOL_TYPE(Symbol->Info);

    if (Object->SectionSlot[Symbol->SectionIndex] != BOXASM_SLOT_TEXT ||
        (Type != BOXASM_STT_FUNC && Type != BOXASM_STT_NOTYPE) ||
        Address < Text->Address ||
        Address > Text->Address + Text->Size)
        return 0;
    if (List->Count == List->Capacity) {
        size_t Capacity = List->Capacity ? List->Capacity * 2u : 256u;
        BoxAsmNameEntry *Fresh;

        if (Capacity > SIZE_MAX / sizeof(*Fresh))
            return -ERR_NO_MEMORY;
        Fresh = (BoxAsmNameEntry *)realloc(List->Items, Capacity * sizeof(*Fresh));
        if (!Fresh)
            return -ERR_NO_MEMORY;
        List->Items = Fresh;
        List->Capacity = Capacity;
    }
    List->Items[List->Count++] = (BoxAsmNameEntry){
        Address, Symbol->Size, BoxAsmElfSymbolName(&Object->Elf, Index),
        (uint8_t)(Type == BOXASM_STT_FUNC),
        (uint8_t)(BOXASM_SYMBOL_BIND(Symbol->Info) != BOXASM_STB_LOCAL)
    };
    return 0;
}

static int BoxAsmNameplateRefuse(BoxAsmLinker *Link, const char *Why)
{
    BoxAsmMessage Message;

    BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
    BoxAsmMessageText(&Message, "Nameplate: ");
    BoxAsmMessageText(&Message, Why);
    return -ERR_INVALID_ARGUMENT;
}

/* The table of nameplate_format.h over the names, one per address. */
static int BoxAsmWriteNameplate(BoxAsmLinker *Link, const BoxAsmNameList *Names)
{
    BoxAsmOutputSection *Section = &Link->Image.Sections[BOXASM_SLOT_NAMEPLATE];
    NameplateHeader Header;
    uint64_t NameBytes = 0;
    uint64_t Total;
    uint64_t Cursor = 0;
    uint8_t *Table;
    size_t Index;

    if (Names->Count == 0)
        return BoxAsmNameplateRefuse(Link, "the image has no function to name");
    if (Names->Count > NAMEPLATE_MAX_ENTRIES)
        return BoxAsmNameplateRefuse(Link, "more functions than the format admits");
    for (Index = 0; Index < Names->Count; Index++)
        NameBytes += strlen(Names->Items[Index].Name) + 1u;
    if (NameBytes > UINT32_MAX)
        return BoxAsmNameplateRefuse(Link, "the names exceed 4 GiB");
    Total = NAMEPLATE_TOTAL_BYTES(Names->Count, NameBytes);
    if (Total > SIZE_MAX)
        return -ERR_NO_MEMORY;
    Table = (uint8_t *)malloc((size_t)Total);
    if (!Table)
        return -ERR_NO_MEMORY;
    memset(Table, 0, (size_t)Total);
    Header.Magic = NAMEPLATE_MAGIC;
    Header.Version = NAMEPLATE_VERSION;
    Header.EntryCount = (uint32_t)Names->Count;
    Header.NameBytes = (uint32_t)NameBytes;
    Header.BaseAddress = Names->Items[0].Address;
    Header.TotalBytes = Total;
    memcpy(Table, &Header, sizeof(Header));
    for (Index = 0; Index < Names->Count; Index++) {
        const BoxAsmNameEntry *Entry = &Names->Items[Index];
        uint64_t Delta = Entry->Address - Header.BaseAddress;
        uint32_t Word;
        size_t Length = strlen(Entry->Name) + 1u;

        if (Delta > UINT32_MAX) {
            free(Table);
            return BoxAsmNameplateRefuse(Link, "the code spans more than 4 GiB");
        }
        Word = (uint32_t)Delta;
        memcpy(Table + NAMEPLATE_OFFSETS_AT(Names->Count) + 4u * Index, &Word, 4u);
        Word = Entry->Length > UINT32_MAX ? UINT32_MAX : (uint32_t)Entry->Length;
        memcpy(Table + NAMEPLATE_SIZES_AT(Names->Count) + 4u * Index, &Word, 4u);
        Word = (uint32_t)Cursor;
        memcpy(Table + NAMEPLATE_NAME_OFFSETS_AT(Names->Count) + 4u * Index, &Word, 4u);
        memcpy(Table + NAMEPLATE_NAMES_AT(Names->Count) + Cursor, Entry->Name, Length);
        Cursor += Length;
    }
    if (!NameplateHeaderValid((const NameplateHeader *)Table, Total)) {
        free(Table);
        return BoxAsmNameplateRefuse(Link, "the table fails its own format check");
    }
    BoxAsmBufferRelease(&Section->Data);
    Section->Data = (BoxAsmBuffer){ Table, Total, Total };
    Section->Size = Total;
    return 0;
}

int BoxAsmBuildNameplate(BoxAsmLinker *Link)
{
    BoxAsmNameList Names = { 0 };
    size_t Unique = 0;
    size_t Index;
    int Rc;

    Rc = BoxAsmVisitImageSymbols(Link, 0, BoxAsmHarvestName, &Names);
    if (Rc == 0)
        Rc = BoxAsmVisitImageSymbols(Link, 1, BoxAsmHarvestName, &Names);
    if (Rc == 0)
        Rc = BoxAsmNameSort(&Names);
    if (Rc == 0) {
        for (Index = 0; Index < Names.Count; Index++) {
            if (Unique > 0 && Names.Items[Unique - 1u].Address == Names.Items[Index].Address)
                continue;
            Names.Items[Unique++] = Names.Items[Index];
        }
        Names.Count = Unique;
        Rc = BoxAsmWriteNameplate(Link, &Names);
    }
    free(Names.Items);
    return Rc;
}

/* ---- Symbol table ------------------------------------------------------ */

/* A symbol as an executable carries it: placed in its output section, valued
 * by its address — a thread-local one by its offset in the TLS block. */
static int BoxAsmEmitSymbol(BoxAsmLinker *Link, const BoxAsmLinkObject *Object,
                            uint32_t Index, uint64_t Address, void *Context)
{
    BoxAsmLinkImage *Image = &Link->Image;
    const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[Index];
    const char *Name = BoxAsmElfSymbolName(&Object->Elf, Index);
    BoxAsmElfSymbol Entry;

    (void)Context;
    if (Image->SymbolNames.Size > UINT32_MAX ||
        Image->Symbols.Size / sizeof(Entry) >= UINT32_MAX) {
        BoxAsmMessageSet(Link->Error, Link->ErrorCapacity,
                         "the symbol table exceeds the ELF format");
        return -ERR_INVALID_ARGUMENT;
    }
    memset(&Entry, 0, sizeof(Entry));
    Entry.Name = (uint32_t)Image->SymbolNames.Size;
    Entry.Info = Symbol->Info;
    Entry.Other = Symbol->Other;
    Entry.SectionIndex = Object->SectionSlot[Symbol->SectionIndex];
    Entry.Value = BOXASM_SYMBOL_TYPE(Symbol->Info) == BOXASM_STT_TLS
                  ? Address - Image->TlsAddress : Address;
    Entry.Size = Symbol->Size;
    if (BoxAsmBufferAppend(&Image->SymbolNames, Name, strlen(Name) + 1u) != 0 ||
        BoxAsmBufferAppend(&Image->Symbols, &Entry, sizeof(Entry)) != 0)
        return -ERR_NO_MEMORY;
    return 0;
}

int BoxAsmBuildSymbolTable(BoxAsmLinker *Link)
{
    BoxAsmLinkImage *Image = &Link->Image;
    BoxAsmElfSymbol Null;
    int Rc;

    memset(&Null, 0, sizeof(Null));
    if (BoxAsmBufferAppend(&Image->Symbols, &Null, sizeof(Null)) != 0 ||
        BoxAsmBufferAppend(&Image->SymbolNames, "", 1u) != 0)
        return -ERR_NO_MEMORY;
    Rc = BoxAsmVisitImageSymbols(Link, 0, BoxAsmEmitSymbol, NULL);
    if (Rc != 0)
        return Rc;
    Image->FirstGlobal = (uint32_t)(Image->Symbols.Size / sizeof(BoxAsmElfSymbol));
    return BoxAsmVisitImageSymbols(Link, 1, BoxAsmEmitSymbol, NULL);
}
