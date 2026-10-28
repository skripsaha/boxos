#include "boxasm_link.h"

#include "box/types.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"
#include "nameplate_format.h"
#include "boxasm_link_internal.h"

#define BOXASM_USER_BASE 0xC000ull
#define BOXASM_PAGE 0x1000ull
#define BOXASM_ARCHIVE_HEADER 8u
#define BOXASM_ARCHIVE_MEMBER_HEADER 60u

typedef struct {
    BoxAsmElfObject Elf;
    const char *Name;
    uint8_t Selected;
    uint16_t *SectionOutput;
    uint64_t *SectionOffset;
} BoxAsmLinkObject;

typedef struct {
    BoxAsmLinkObject *Items;
    size_t Count;
    size_t Capacity;
} BoxAsmObjectList;

typedef struct {
    const char *Name;
    uint64_t Address;
    uint64_t Size;
    uint8_t Typed;
    uint8_t Global;
} BoxAsmNameEntry;

typedef struct {
    BoxAsmNameEntry *Items;
    size_t Count;
    size_t Capacity;
} BoxAsmNameList;

static const char *const BoxAsmSectionNames[12] = {
    "", ".text", ".rodata", ".eh_frame", ".gcc_except_table",
    ".nameplate", ".data", ".init_array", ".fini_array", ".tdata",
    ".tbss", ".bss"
};

static void BoxAsmLinkError(char *Error, size_t Capacity, const char *Text)
{
    size_t Length;

    if (!Error || Capacity == 0)
        return;
    Length = strlen(Text);
    if (Length >= Capacity)
        Length = Capacity - 1u;
    memcpy(Error, Text, Length);
    Error[Length] = '\0';
}

static int BoxAsmRangeValid(uint64_t Size, uint64_t Offset, uint64_t Length)
{
    return Offset <= Size && Length <= Size - Offset;
}

static char *BoxAsmDuplicateSpan(const char *Text, size_t Length)
{
    char *Copy = (char *)malloc(Length + 1u);
    if (!Copy)
        return NULL;
    memcpy(Copy, Text, Length);
    Copy[Length] = '\0';
    return Copy;
}

uint64_t BoxAsmLinkAlign(uint64_t Value, uint64_t Alignment)
{
    uint64_t Mask;

    if (Alignment <= 1u)
        return Value;
    if ((Alignment & (Alignment - 1u)) != 0 || Value > UINT64_MAX - (Alignment - 1u))
        return UINT64_MAX;
    Mask = Alignment - 1u;
    return (Value + Mask) & ~Mask;
}

static int BoxAsmObjectListGrow(BoxAsmObjectList *List)
{
    size_t NewCapacity = List->Capacity ? List->Capacity * 2u : 32u;
    BoxAsmLinkObject *Fresh;

    if (NewCapacity < List->Count || NewCapacity > SIZE_MAX / sizeof(*Fresh))
        return -ERR_NO_MEMORY;
    Fresh = (BoxAsmLinkObject *)realloc(List->Items,
                                        NewCapacity * sizeof(*Fresh));
    if (!Fresh)
        return -ERR_NO_MEMORY;
    List->Items = Fresh;
    List->Capacity = NewCapacity;
    return 0;
}

static int BoxAsmObjectAdd(BoxAsmObjectList *List, const BoxAsmBlob *Blob,
                           const char *Name, uint8_t Selected,
                           char *Error, size_t ErrorCapacity)
{
    BoxAsmLinkObject *Object;
    int Rc;

    if (List->Count == List->Capacity && BoxAsmObjectListGrow(List) != 0)
        return -ERR_NO_MEMORY;
    Object = &List->Items[List->Count];
    memset(Object, 0, sizeof(*Object));
    Rc = BoxAsmElfOpen(Blob, BOXASM_ET_REL, &Object->Elf,
                       Error, ErrorCapacity);
    if (Rc != 0)
        return Rc;
    Object->Name = Name;
    Object->Selected = Selected;
    Object->SectionOutput = (uint16_t *)malloc(
        Object->Elf.Header->SectionHeaderCount * sizeof(uint16_t));
    Object->SectionOffset = (uint64_t *)malloc(
        Object->Elf.Header->SectionHeaderCount * sizeof(uint64_t));
    if (!Object->SectionOutput || !Object->SectionOffset) {
        free(Object->SectionOutput);
        free(Object->SectionOffset);
        memset(Object, 0, sizeof(*Object));
        return -ERR_NO_MEMORY;
    }
    memset(Object->SectionOutput, 0xFF,
           Object->Elf.Header->SectionHeaderCount * sizeof(uint16_t));
    memset(Object->SectionOffset, 0,
           Object->Elf.Header->SectionHeaderCount * sizeof(uint64_t));
    List->Count++;
    return 0;
}

static void BoxAsmObjectListRelease(BoxAsmObjectList *List)
{
    size_t Index;

    if (!List)
        return;
    for (Index = 0; Index < List->Count; Index++) {
        free(List->Items[Index].SectionOutput);
        free(List->Items[Index].SectionOffset);
    }
    free(List->Items);
    memset(List, 0, sizeof(*List));
}

static uint64_t BoxAsmParseDecimal(const uint8_t *Bytes, uint32_t Length)
{
    uint64_t Value = 0;
    uint32_t Index;

    for (Index = 0; Index < Length && Bytes[Index] != ' '; Index++) {
        if (Bytes[Index] < '0' || Bytes[Index] > '9')
            return UINT64_MAX;
        if (Value > (UINT64_MAX - (uint64_t)(Bytes[Index] - '0')) / 10u)
            return UINT64_MAX;
        Value = Value * 10u + (uint64_t)(Bytes[Index] - '0');
    }
    return Value;
}

static void BoxAsmArchiveName(const uint8_t *Header, char *Out, size_t Capacity)
{
    size_t Length = 16u;
    size_t Index;

    if (Length >= Capacity)
        Length = Capacity - 1u;
    while (Length > 0 && (Header[Length - 1u] == ' ' || Header[Length - 1u] == '/'))
        Length--;
    for (Index = 0; Index < Length; Index++)
        Out[Index] = (char)Header[Index];
    Out[Length] = '\0';
}

static int BoxAsmArchiveScan(const BoxAsmBlob *Archive, BoxAsmObjectList *List,
                             char *Error, size_t ErrorCapacity)
{
    uint64_t Position = BOXASM_ARCHIVE_HEADER;

    if (Archive->Size < BOXASM_ARCHIVE_HEADER ||
        memcmp(Archive->Data, "!<arch>\n", BOXASM_ARCHIVE_HEADER) != 0) {
        BoxAsmLinkError(Error, ErrorCapacity, "runtime archive has no ar header");
        return -ERR_INVALID_ARGUMENT;
    }
    while (Position < Archive->Size) {
        const uint8_t *Header;
        uint64_t MemberSize;
        uint64_t DataOffset;
        char Name[32];
        BoxAsmBlob Member;
        BoxAsmElfObject Probe;

        if (!BoxAsmRangeValid(Archive->Size, Position,
                              BOXASM_ARCHIVE_MEMBER_HEADER)) {
            BoxAsmLinkError(Error, ErrorCapacity, "runtime archive member header is truncated");
            return -ERR_INVALID_ARGUMENT;
        }
        Header = Archive->Data + Position;
        if (Header[58] != '`' || Header[59] != '\n') {
            BoxAsmLinkError(Error, ErrorCapacity, "runtime archive member header is invalid");
            return -ERR_INVALID_ARGUMENT;
        }
        MemberSize = BoxAsmParseDecimal(Header + 48u, 10u);
        if (MemberSize == UINT64_MAX) {
            BoxAsmLinkError(Error, ErrorCapacity, "runtime archive member size is invalid");
            return -ERR_INVALID_ARGUMENT;
        }
        DataOffset = Position + BOXASM_ARCHIVE_MEMBER_HEADER;
        if (!BoxAsmRangeValid(Archive->Size, DataOffset, MemberSize)) {
            BoxAsmLinkError(Error, ErrorCapacity, "runtime archive member exceeds archive");
            return -ERR_INVALID_ARGUMENT;
        }
        BoxAsmArchiveName(Header, Name, sizeof(Name));
        Member = (BoxAsmBlob){ Archive->Data + DataOffset, MemberSize, NULL };
        if (BoxAsmElfOpen(&Member, BOXASM_ET_REL, &Probe, NULL, 0) == 0) {
            int Rc = BoxAsmObjectAdd(List, &Member, NULL, 0, Error, ErrorCapacity);
            if (Rc != 0)
                return Rc;
            List->Items[List->Count - 1u].Name =
                BoxAsmDuplicateSpan(Name, strlen(Name));
            if (!List->Items[List->Count - 1u].Name)
                return -ERR_NO_MEMORY;
        }
        Position = DataOffset + MemberSize;
        if (Position & 1u)
            Position++;
    }
    return 0;
}

static int BoxAsmSymbolDefined(const BoxAsmElfSymbol *Symbol,
                               const BoxAsmElfObject *Object)
{
    return Symbol->SectionIndex == BOXASM_SHN_ABS ||
           (Symbol->SectionIndex != BOXASM_SHN_UNDEF &&
            Symbol->SectionIndex != BOXASM_SHN_COMMON &&
            Symbol->SectionIndex < Object->Header->SectionHeaderCount);
}

static int BoxAsmSymbolGlobal(const BoxAsmElfSymbol *Symbol)
{
    uint8_t Bind = BOXASM_SYMBOL_BIND(Symbol->Info);
    return Bind == BOXASM_STB_GLOBAL || Bind == BOXASM_STB_WEAK;
}

static int BoxAsmSymbolStrong(const BoxAsmElfSymbol *Symbol)
{
    return BOXASM_SYMBOL_BIND(Symbol->Info) == BOXASM_STB_GLOBAL;
}

static int BoxAsmFindDefinition(const BoxAsmObjectList *Objects, const char *Name,
                                BoxAsmLinkObject **OutObject,
                                uint32_t *OutIndex,
                                char *Error, size_t ErrorCapacity)
{
    BoxAsmLinkObject *WeakObject = NULL;
    uint32_t WeakIndex = 0;
    size_t ObjectIndex;

    for (ObjectIndex = 0; ObjectIndex < Objects->Count; ObjectIndex++) {
        BoxAsmLinkObject *Object = &Objects->Items[ObjectIndex];
        uint32_t SymbolIndex;

        if (!Object->Selected)
            continue;
        for (SymbolIndex = 0; SymbolIndex < Object->Elf.SymbolCount; SymbolIndex++) {
            const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[SymbolIndex];
            const char *SymbolName;

            if (!BoxAsmSymbolGlobal(Symbol) || !BoxAsmSymbolDefined(Symbol, &Object->Elf))
                continue;
            SymbolName = BoxAsmElfSymbolName(&Object->Elf, SymbolIndex);
            if (!SymbolName || strcmp(SymbolName, Name) != 0)
                continue;
            if (BoxAsmSymbolStrong(Symbol)) {
                if (*OutObject && *OutObject != Object) {
                    BoxAsmLinkError(Error, ErrorCapacity, "conflicting strong symbol definition");
                    return -ERR_INVALID_ARGUMENT;
                }
                *OutObject = Object;
                *OutIndex = SymbolIndex;
                return 0;
            }
            if (!WeakObject) {
                WeakObject = Object;
                WeakIndex = SymbolIndex;
            }
        }
    }
    if (!*OutObject && WeakObject) {
        *OutObject = WeakObject;
        *OutIndex = WeakIndex;
    }
    return *OutObject ? 0 : -ERR_FILE_NOT_FOUND;
}

static int BoxAsmArchiveSelect(BoxAsmObjectList *Objects,
                               char *Error, size_t ErrorCapacity)
{
    uint8_t Changed = 1;

    while (Changed) {
        size_t ObjectIndex;
        Changed = 0;
        for (ObjectIndex = 0; ObjectIndex < Objects->Count; ObjectIndex++) {
            BoxAsmLinkObject *Object = &Objects->Items[ObjectIndex];
            uint32_t SymbolIndex;

            if (!Object->Selected)
                continue;
            for (SymbolIndex = 0; SymbolIndex < Object->Elf.SymbolCount; SymbolIndex++) {
                const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[SymbolIndex];
                const char *Name;
                BoxAsmLinkObject *Definition = NULL;
                uint32_t DefinitionIndex = 0;
                size_t Candidate;

                if (Symbol->SectionIndex != BOXASM_SHN_UNDEF ||
                    !BoxAsmSymbolGlobal(Symbol) || BOXASM_SYMBOL_BIND(Symbol->Info) == BOXASM_STB_WEAK)
                    continue;
                Name = BoxAsmElfSymbolName(&Object->Elf, SymbolIndex);
                if (!Name || Name[0] == '\0')
                    continue;
                for (Candidate = 0; Candidate < Objects->Count; Candidate++) {
                    if (Objects->Items[Candidate].Selected)
                        (void)BoxAsmFindDefinition(Objects, Name, &Definition,
                                                   &DefinitionIndex, Error, ErrorCapacity);
                    if (Definition)
                        break;
                }
                if (Definition)
                    continue;
                for (Candidate = 0; Candidate < Objects->Count; Candidate++) {
                    BoxAsmLinkObject *CandidateObject = &Objects->Items[Candidate];
                    uint32_t CandidateSymbol;

                    if (CandidateObject->Selected)
                        continue;
                    for (CandidateSymbol = 0;
                         CandidateSymbol < CandidateObject->Elf.SymbolCount;
                         CandidateSymbol++) {
                        const BoxAsmElfSymbol *DefinitionSymbol =
                            &CandidateObject->Elf.Symbols[CandidateSymbol];
                        const char *DefinitionName;

                        if (!BoxAsmSymbolDefined(DefinitionSymbol, &CandidateObject->Elf) ||
                            !BoxAsmSymbolGlobal(DefinitionSymbol))
                            continue;
                        DefinitionName = BoxAsmElfSymbolName(&CandidateObject->Elf,
                                                             CandidateSymbol);
                        if (DefinitionName && strcmp(DefinitionName, Name) == 0) {
                            CandidateObject->Selected = 1;
                            Changed = 1;
                            break;
                        }
                    }
                    if (Changed)
                        break;
                }
            }
        }
    }
    return 0;
}

static void BoxAsmImageInit(BoxAsmLinkImage *Image)
{
    static const uint64_t Alignments[12] = {
        1, 4096, 4096, 8, 8, 8, 4096, 8, 8, 1, 1, 4096
    };
    static const uint32_t Types[12] = {
        BOXASM_SHT_NULL, BOXASM_SHT_PROGBITS, BOXASM_SHT_PROGBITS,
        BOXASM_SHT_PROGBITS, BOXASM_SHT_PROGBITS, BOXASM_SHT_PROGBITS,
        BOXASM_SHT_PROGBITS, BOXASM_SHT_PROGBITS, BOXASM_SHT_PROGBITS,
        BOXASM_SHT_PROGBITS, BOXASM_SHT_NOBITS, BOXASM_SHT_NOBITS
    };
    static const uint64_t Flags[12] = {
        0, BOXASM_SHF_ALLOC | BOXASM_SHF_EXECINSTR, BOXASM_SHF_ALLOC,
        BOXASM_SHF_ALLOC, BOXASM_SHF_ALLOC, BOXASM_SHF_ALLOC,
        BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE,
        BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE,
        BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE,
        BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE | BOXASM_SHF_TLS,
        BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE | BOXASM_SHF_TLS,
        BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE
    };
    size_t Index;

    memset(Image, 0, sizeof(*Image));
    for (Index = 0; Index < 12; Index++) {
        Image->Sections[Index].Name = BoxAsmSectionNames[Index];
        Image->Sections[Index].Type = Types[Index];
        Image->Sections[Index].Flags = Flags[Index];
        Image->Sections[Index].Align = Alignments[Index];
        Image->Sections[Index].NoBits = Types[Index] == BOXASM_SHT_NOBITS;
    }
}

static int BoxAsmClassToOutput(int Class)
{
    return Class;
}

static int BoxAsmCollectSections(BoxAsmObjectList *Objects,
                                 BoxAsmLinkImage *Image,
                                 char *Error, size_t ErrorCapacity)
{
    size_t ObjectIndex;

    for (ObjectIndex = 0; ObjectIndex < Objects->Count; ObjectIndex++) {
        BoxAsmLinkObject *Object = &Objects->Items[ObjectIndex];
        uint16_t SectionIndex;

        if (!Object->Selected)
            continue;
        for (SectionIndex = 0; SectionIndex < Object->Elf.Header->SectionHeaderCount;
             SectionIndex++) {
            const BoxAsmElfSectionHeader *Input = &Object->Elf.Sections[SectionIndex];
            int Class = BoxAsmElfSectionClass(&Object->Elf, SectionIndex);
            BoxAsmOutputSection *Output;
            uint64_t Offset;
            uint64_t Align;

            if (Class == 0 || Input->Size == 0) {
                Object->SectionOutput[SectionIndex] = 0xFFFFu;
                continue;
            }
            if (Class < 0 || Class >= 12) {
                BoxAsmLinkError(Error, ErrorCapacity, "unsupported input section");
                return -ERR_INVALID_ARGUMENT;
            }
            Output = &Image->Sections[BoxAsmClassToOutput(Class)];
            Align = Input->AddressAlign ? Input->AddressAlign : 1u;
            if ((Align & (Align - 1u)) != 0) {
                BoxAsmLinkError(Error, ErrorCapacity, "input section alignment is invalid");
                return -ERR_INVALID_ARGUMENT;
            }
            Output->Present = 1;
            if (Align > Output->Align)
                Output->Align = Align;
            Offset = BoxAsmLinkAlign(Output->Size, Align);
            if (Offset == UINT64_MAX || Input->Size > UINT64_MAX - Offset) {
                BoxAsmLinkError(Error, ErrorCapacity, "output section is too large");
                return -ERR_NO_MEMORY;
            }
            Object->SectionOutput[SectionIndex] = (uint16_t)Class;
            Object->SectionOffset[SectionIndex] = Offset;
            Output->Size = Offset + Input->Size;
            if (!Output->NoBits) {
                if (BoxAsmBufferAlign(&Output->Data, Align) != 0 ||
                    BoxAsmBufferAppend(&Output->Data,
                                       BoxAsmElfSectionData(&Object->Elf, SectionIndex),
                                       Input->Size) != 0) {
                    BoxAsmLinkError(Error, ErrorCapacity, "cannot copy input section");
                    return -ERR_NO_MEMORY;
                }
            }
        }
    }
    return 0;
}

static int BoxAsmLayoutSections(BoxAsmLinkImage *Image)
{
    uint64_t Address = BOXASM_USER_BASE;
    size_t Index;

    for (Index = 1; Index <= 5; Index++) {
        BoxAsmOutputSection *Section = &Image->Sections[Index];
        if (!Section->Present)
            continue;
        Address = BoxAsmLinkAlign(Address, Section->Align);
        if (Address == UINT64_MAX)
            return -ERR_NO_MEMORY;
        Section->Address = Address;
        Address += Section->Size;
    }
    Address = BoxAsmLinkAlign(Address, Image->Sections[6].Align);
    if (Address == UINT64_MAX)
        return -ERR_NO_MEMORY;
    for (Index = 6; Index <= 11; Index++) {
        BoxAsmOutputSection *Section = &Image->Sections[Index];
        if (!Section->Present && Index != 6)
            continue;
        if (Index != 6)
            Address = BoxAsmLinkAlign(Address, Section->Align);
        Section->Address = Address;
        Address += Section->Size;
    }
    Image->ProgramEnd = Address;
    Image->TlsAddress = Image->Sections[9].Present ? Image->Sections[9].Address :
                        (Image->Sections[10].Present ? Image->Sections[10].Address : 0);
    Image->TlsMemorySize = Image->Sections[10].Present
        ? Image->Sections[10].Address + Image->Sections[10].Size - Image->TlsAddress
        : Image->Sections[9].Size;
    Image->TlsAlign = Image->Sections[9].Align > Image->Sections[10].Align
        ? Image->Sections[9].Align : Image->Sections[10].Align;
    return 0;
}

static int BoxAsmAddName(BoxAsmNameList *List, const char *Name,
                         uint64_t Address, uint64_t Size, uint8_t Typed,
                         uint8_t Global)
{
    size_t Index;

    if (!Name || Name[0] == '\0' || Address == 0)
        return 0;
    for (Index = 0; Index < List->Count; Index++) {
        if (List->Items[Index].Address != Address)
            continue;
        if (Typed > List->Items[Index].Typed ||
            (Typed == List->Items[Index].Typed && Size > List->Items[Index].Size) ||
            (Typed == List->Items[Index].Typed && Size == List->Items[Index].Size &&
             Global > List->Items[Index].Global)) {
            List->Items[Index] = (BoxAsmNameEntry){ Name, Address, Size, Typed, Global };
        }
        return 0;
    }
    if (List->Count == List->Capacity) {
        size_t NewCapacity = List->Capacity ? List->Capacity * 2u : 64u;
        BoxAsmNameEntry *Fresh = (BoxAsmNameEntry *)realloc(
            List->Items, NewCapacity * sizeof(*Fresh));
        if (!Fresh)
            return -ERR_NO_MEMORY;
        List->Items = Fresh;
        List->Capacity = NewCapacity;
    }
    List->Items[List->Count++] = (BoxAsmNameEntry){ Name, Address, Size, Typed, Global };
    return 0;
}

static void BoxAsmNameSort(BoxAsmNameList *List)
{
    size_t Index;

    for (Index = 1; Index < List->Count; Index++) {
        BoxAsmNameEntry Value = List->Items[Index];
        size_t At = Index;
        while (At > 0 && List->Items[At - 1u].Address > Value.Address) {
            List->Items[At] = List->Items[At - 1u];
            At--;
        }
        List->Items[At] = Value;
    }
}

static int BoxAsmBuildNameplate(BoxAsmObjectList *Objects,
                                BoxAsmLinkImage *Image,
                                char *Error, size_t ErrorCapacity)
{
    BoxAsmNameList Names = { 0 };
    BoxAsmBuffer NameBytes = { 0 };
    uint32_t *Offsets = NULL;
    uint32_t *Sizes = NULL;
    uint32_t *NameOffsets = NULL;
    NameplateHeader Header;
    uint64_t Total;
    size_t Index;
    int Rc = -ERR_NO_MEMORY;

    for (Index = 0; Index < Objects->Count; Index++) {
        BoxAsmLinkObject *Object = &Objects->Items[Index];
        uint32_t SymbolIndex;

        if (!Object->Selected)
            continue;
        for (SymbolIndex = 0; SymbolIndex < Object->Elf.SymbolCount; SymbolIndex++) {
            const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[SymbolIndex];
            const char *Name = BoxAsmElfSymbolName(&Object->Elf, SymbolIndex);
            int Class;
            uint64_t Address;

            if (!Name || !BoxAsmSymbolDefined(Symbol, &Object->Elf) ||
                Symbol->SectionIndex == BOXASM_SHN_ABS)
                continue;
            Class = Object->SectionOutput[Symbol->SectionIndex];
            if (Class != 1 || Symbol->Value == 0)
                continue;
            Address = Image->Sections[Class].Address +
                      Object->SectionOffset[Symbol->SectionIndex] + Symbol->Value;
            if (BoxAsmAddName(&Names, Name, Address, Symbol->Size,
                              BOXASM_SYMBOL_TYPE(Symbol->Info) == BOXASM_STT_FUNC,
                              BOXASM_SYMBOL_BIND(Symbol->Info) != BOXASM_STB_LOCAL) != 0)
                goto Done;
        }
    }
    BoxAsmNameSort(&Names);
    if (Names.Count == 0 || Names.Count > NAMEPLATE_MAX_ENTRIES) {
        BoxAsmLinkError(Error, ErrorCapacity, "no usable executable symbols for Nameplate");
        Rc = -ERR_INVALID_ARGUMENT;
        goto Done;
    }
    for (Index = 0; Index < Names.Count; Index++) {
        if (strlen(Names.Items[Index].Name) > SIZE_MAX - NameBytes.Size - 1u ||
            BoxAsmBufferAppend(&NameBytes, Names.Items[Index].Name,
                               strlen(Names.Items[Index].Name) + 1u) != 0)
            goto Done;
    }
    if (NameBytes.Size > UINT32_MAX || Names.Count > UINT32_MAX ||
        NameBytes.Size > UINT32_MAX)
        goto Done;
    Total = NAMEPLATE_TOTAL_BYTES(Names.Count, NameBytes.Size);
    if (Total > SIZE_MAX)
        goto Done;
    Image->Sections[5].Present = 1;
    Image->Sections[5].Size = Total;
    Image->Sections[5].Align = 8u;
    Offsets = (uint32_t *)malloc(Names.Count * sizeof(uint32_t));
    Sizes = (uint32_t *)malloc(Names.Count * sizeof(uint32_t));
    NameOffsets = (uint32_t *)malloc(Names.Count * sizeof(uint32_t));
    if (!Offsets || !Sizes || !NameOffsets)
        goto Done;
    {
        uint64_t Cursor = 0;
        for (Index = 0; Index < Names.Count; Index++) {
            uint64_t Delta = Names.Items[Index].Address - Names.Items[0].Address;
            size_t Length = strlen(Names.Items[Index].Name) + 1u;
            if (Delta > UINT32_MAX || Names.Items[Index].Size > UINT32_MAX ||
                Cursor > UINT32_MAX) {
                BoxAsmLinkError(Error, ErrorCapacity, "Nameplate address or name is too large");
                Rc = -ERR_INVALID_ARGUMENT;
                goto Done;
            }
            Offsets[Index] = (uint32_t)Delta;
            Sizes[Index] = (uint32_t)Names.Items[Index].Size;
            NameOffsets[Index] = (uint32_t)Cursor;
            Cursor += Length;
        }
    }
    {
        uint8_t *Table = (uint8_t *)malloc((size_t)Total);
        uint32_t *TableOffsets;
        uint32_t *TableSizes;
        uint32_t *TableNameOffsets;
        if (!Table)
            goto Done;
        memset(Table, 0, (size_t)Total);
        Header.Magic = NAMEPLATE_MAGIC;
        Header.Version = NAMEPLATE_VERSION;
        Header.EntryCount = (uint32_t)Names.Count;
        Header.NameBytes = (uint32_t)NameBytes.Size;
        Header.BaseAddress = Names.Items[0].Address;
        Header.TotalBytes = Total;
        memcpy(Table, &Header, sizeof(Header));
        TableOffsets = (uint32_t *)(Table + NAMEPLATE_OFFSETS_AT(Names.Count));
        TableSizes = (uint32_t *)(Table + NAMEPLATE_SIZES_AT(Names.Count));
        TableNameOffsets = (uint32_t *)(Table + NAMEPLATE_NAME_OFFSETS_AT(Names.Count));
        memcpy(TableOffsets, Offsets, Names.Count * sizeof(uint32_t));
        memcpy(TableSizes, Sizes, Names.Count * sizeof(uint32_t));
        memcpy(TableNameOffsets, NameOffsets, Names.Count * sizeof(uint32_t));
        memcpy(Table + NAMEPLATE_NAMES_AT(Names.Count), NameBytes.Data,
               (size_t)NameBytes.Size);
        Image->Sections[5].Data.Data = Table;
        Image->Sections[5].Data.Size = Total;
        Image->Sections[5].Data.Capacity = Total;
        Rc = 0;
    }
Done:
    free(Offsets);
    free(Sizes);
    free(NameOffsets);
    free(Names.Items);
    BoxAsmBufferRelease(&NameBytes);
    return Rc;
}

static int BoxAsmSyntheticSymbol(const BoxAsmLinkImage *Image, const char *Name,
                                 uint64_t *Out)
{
    const BoxAsmOutputSection *Data = &Image->Sections[6];
    const BoxAsmOutputSection *Init = &Image->Sections[7];
    const BoxAsmOutputSection *Fini = &Image->Sections[8];
    const BoxAsmOutputSection *Tdata = &Image->Sections[9];
    const BoxAsmOutputSection *Tbss = &Image->Sections[10];
    const BoxAsmOutputSection *Bss = &Image->Sections[11];

    if (strcmp(Name, "_GLOBAL_OFFSET_TABLE_") == 0 || strcmp(Name, "__cxa_finalize") == 0) {
        *Out = 0;
        return 0;
    }
    if (strcmp(Name, "__eh_frame_start") == 0) { *Out = Image->Sections[3].Address; return 0; }
    if (strcmp(Name, "__eh_frame_end") == 0) { *Out = Image->Sections[3].Address + Image->Sections[3].Size; return 0; }
    if (strcmp(Name, "__nameplate_start") == 0) { *Out = Image->Sections[5].Address; return 0; }
    if (strcmp(Name, "__nameplate_end") == 0) { *Out = Image->Sections[5].Address + Image->Sections[5].Size; return 0; }
    if (strcmp(Name, "__init_array_start") == 0) { *Out = Init->Present ? Init->Address : Data->Address + Data->Size; return 0; }
    if (strcmp(Name, "__init_array_end") == 0) { *Out = Init->Present ? Init->Address + Init->Size : Data->Address + Data->Size; return 0; }
    if (strcmp(Name, "__fini_array_start") == 0) { *Out = Fini->Present ? Fini->Address : (Init->Present ? Init->Address + Init->Size : Data->Address + Data->Size); return 0; }
    if (strcmp(Name, "__fini_array_end") == 0) { *Out = Fini->Present ? Fini->Address + Fini->Size : (Init->Present ? Init->Address + Init->Size : Data->Address + Data->Size); return 0; }
    if (strcmp(Name, "__tdata_start") == 0) { *Out = Tdata->Present ? Tdata->Address : (Tbss->Present ? Tbss->Address : Data->Address + Data->Size); return 0; }
    if (strcmp(Name, "__tdata_end") == 0) { *Out = Tdata->Present ? Tdata->Address + Tdata->Size : (Tbss->Present ? Tbss->Address : Data->Address + Data->Size); return 0; }
    if (strcmp(Name, "__tdata_size") == 0) { *Out = Tdata->Size; return 0; }
    if (strcmp(Name, "__tbss_size") == 0) { *Out = Tbss->Size; return 0; }
    if (strcmp(Name, "__bss_start") == 0) { *Out = Bss->Present ? Bss->Address : Image->ProgramEnd; return 0; }
    if (strcmp(Name, "__bss_end") == 0) { *Out = Bss->Present ? Bss->Address + Bss->Size : Image->ProgramEnd; return 0; }
    if (strcmp(Name, "__program_end") == 0) { *Out = Image->ProgramEnd; return 0; }
    return -ERR_FILE_NOT_FOUND;
}

static int BoxAsmResolveSymbol(const BoxAsmLinkImage *Image,
                               const BoxAsmObjectList *Objects,
                               BoxAsmLinkObject *Object,
                               const BoxAsmElfSymbol *Symbol,
                               uint64_t *Out,
                               char *Error, size_t ErrorCapacity)
{
    const char *Name;
    BoxAsmLinkObject *Definition = NULL;
    uint32_t DefinitionIndex = 0;

    if (Symbol->SectionIndex == BOXASM_SHN_ABS) {
        *Out = Symbol->Value;
        return 0;
    }
    if (Symbol->SectionIndex == BOXASM_SHN_COMMON) {
        BoxAsmLinkError(Error, ErrorCapacity, "COMMON symbols are unsupported");
        return -ERR_INVALID_ARGUMENT;
    }
    if (Symbol->SectionIndex != BOXASM_SHN_UNDEF) {
        uint16_t Output = Object->SectionOutput[Symbol->SectionIndex];
        if (Output == 0xFFFFu || Output >= 12u) {
            BoxAsmLinkError(Error, ErrorCapacity, "symbol refers to discarded section");
            return -ERR_INVALID_ARGUMENT;
        }
        *Out = Image->Sections[Output].Address +
               Object->SectionOffset[Symbol->SectionIndex] + Symbol->Value;
        return 0;
    }
    Name = BoxAsmElfSymbolName(&Object->Elf,
                               (uint32_t)(Symbol - Object->Elf.Symbols));
    if (!Name)
        return -ERR_INVALID_ARGUMENT;
    if (BoxAsmFindDefinition(Objects, Name, &Definition, &DefinitionIndex,
                             Error, ErrorCapacity) == 0) {
        return BoxAsmResolveSymbol(Image, Objects, Definition,
                                   &Definition->Elf.Symbols[DefinitionIndex],
                                   Out, Error, ErrorCapacity);
    }
    if (BoxAsmSyntheticSymbol(Image, Name, Out) == 0)
        return 0;
    if (BOXASM_SYMBOL_BIND(Symbol->Info) == BOXASM_STB_WEAK) {
        *Out = 0;
        return 0;
    }
    BoxAsmLinkError(Error, ErrorCapacity, "unresolved symbol");
    return -ERR_FILE_NOT_FOUND;
}

static int BoxAsmApplyRelocations(BoxAsmObjectList *Objects,
                                  BoxAsmLinkImage *Image,
                                  char *Error, size_t ErrorCapacity)
{
    size_t ObjectIndex;

    for (ObjectIndex = 0; ObjectIndex < Objects->Count; ObjectIndex++) {
        BoxAsmLinkObject *Object = &Objects->Items[ObjectIndex];
        uint16_t SectionIndex;

        if (!Object->Selected)
            continue;
        for (SectionIndex = 0; SectionIndex < Object->Elf.Header->SectionHeaderCount;
             SectionIndex++) {
            const BoxAsmElfSectionHeader *Relocations = &Object->Elf.Sections[SectionIndex];
            const BoxAsmElfSectionHeader *Target;
            const BoxAsmElfRela *Entries;
            uint64_t Count;
            uint64_t EntryIndex;
            uint16_t Output;

            if (Relocations->Type != BOXASM_SHT_RELA)
                continue;
            if (Relocations->Info >= Object->Elf.Header->SectionHeaderCount ||
                Relocations->EntrySize != sizeof(BoxAsmElfRela) ||
                Relocations->Size % sizeof(BoxAsmElfRela) != 0)
                return -ERR_INVALID_ARGUMENT;
            Target = &Object->Elf.Sections[Relocations->Info];
            Output = Object->SectionOutput[Relocations->Info];
            if (Output == 0xFFFFu || Output >= 12u || Image->Sections[Output].NoBits)
                return -ERR_INVALID_ARGUMENT;
            Entries = (const BoxAsmElfRela *)(Object->Elf.Blob.Data + Relocations->Offset);
            Count = Relocations->Size / sizeof(BoxAsmElfRela);
            for (EntryIndex = 0; EntryIndex < Count; EntryIndex++) {
                const BoxAsmElfRela *Entry = &Entries[EntryIndex];
                uint32_t SymbolIndex = BOXASM_RELA_SYMBOL(Entry->Info);
                uint32_t Type = BOXASM_RELA_TYPE(Entry->Info);
                uint64_t SymbolValue;
                uint64_t Place;
                int64_t Value;
                uint8_t *At;
                uint32_t Width = (Type == BOXASM_R_X86_64_64) ? 8u : 4u;

                if (SymbolIndex >= Object->Elf.SymbolCount ||
                    Entry->Address > Target->Size ||
                    Target->Size - Entry->Address < Width)
                    return -ERR_INVALID_ARGUMENT;
                {
                    int ResolveRc = BoxAsmResolveSymbol(
                        Image, Objects, Object, &Object->Elf.Symbols[SymbolIndex],
                        &SymbolValue, Error, ErrorCapacity);
                    if (ResolveRc != 0)
                        return ResolveRc;
                }
                Place = Image->Sections[Output].Address +
                        Object->SectionOffset[Relocations->Info] + Entry->Address;
                At = Image->Sections[Output].Data.Data +
                     Object->SectionOffset[Relocations->Info] + Entry->Address;
                if (Type == BOXASM_R_X86_64_TPOFF32)
                    Value = (int64_t)SymbolValue + Entry->Addend -
                            (int64_t)(Image->TlsAddress + Image->TlsMemorySize);
                else if (Type == BOXASM_R_X86_64_PC32 || Type == BOXASM_R_X86_64_PLT32)
                    Value = (int64_t)SymbolValue + Entry->Addend - (int64_t)Place;
                else if (Type == BOXASM_R_X86_64_64)
                    Value = (int64_t)((uint64_t)SymbolValue + (uint64_t)Entry->Addend);
                else if (Type == BOXASM_R_X86_64_32 || Type == BOXASM_R_X86_64_32S)
                    Value = (int64_t)((uint64_t)SymbolValue + (uint64_t)Entry->Addend);
                else
                    return -ERR_INVALID_ARGUMENT;

                if (Type == BOXASM_R_X86_64_64) {
                    uint64_t Full = (uint64_t)Value;
                    memcpy(At, &Full, sizeof(Full));
                } else if (Type == BOXASM_R_X86_64_32) {
                    uint64_t Unsigned = (uint64_t)Value;
                    uint32_t Word;
                    if (Unsigned > UINT32_MAX)
                        return -ERR_INVALID_ARGUMENT;
                    Word = (uint32_t)Unsigned;
                    memcpy(At, &Word, sizeof(Word));
                } else {
                    int64_t Minimum = Type == BOXASM_R_X86_64_TPOFF32 ? INT32_MIN : INT32_MIN;
                    if (Value < Minimum || Value > INT32_MAX)
                        return -ERR_INVALID_ARGUMENT;
                    {
                        int32_t Word = (int32_t)Value;
                        memcpy(At, &Word, sizeof(Word));
                    }
                }
            }
        }
    }
    return 0;
}

int BoxAsmLink(const BoxAsmBlob *Entry,
               const BoxAsmBlob *Program,
               const BoxAsmBlob *RuntimeArchive,
               BoxAsmBlob *Out,
               char *Error,
               size_t ErrorCapacity)
{
    BoxAsmObjectList Objects = { 0 };
    BoxAsmLinkImage Image;
    BoxAsmBlob EntryBlob;
    BoxAsmBlob ProgramBlob;
    BoxAsmLinkObject *StartObject;
    BoxAsmLinkObject *MainObject;
    uint32_t StartIndex = 0;
    uint32_t MainIndex = 0;
    size_t Index;
    int Rc;

    if (!Entry || !Program || !RuntimeArchive || !Out) {
        BoxAsmLinkError(Error, ErrorCapacity, "link input is missing");
        return -ERR_INVALID_ARGUMENT;
    }
    memset(Out, 0, sizeof(*Out));
    EntryBlob = *Entry;
    ProgramBlob = *Program;
    Rc = BoxAsmObjectAdd(&Objects, &EntryBlob, "boxlib_start.o", 1,
                         Error, ErrorCapacity);
    if (Rc != 0)
        goto Done;
    Rc = BoxAsmObjectAdd(&Objects, &ProgramBlob, "program.asm", 1,
                         Error, ErrorCapacity);
    if (Rc != 0)
        goto Done;
    Rc = BoxAsmArchiveScan(RuntimeArchive, &Objects, Error, ErrorCapacity);
    if (Rc != 0)
        goto Done;
    Rc = BoxAsmArchiveSelect(&Objects, Error, ErrorCapacity);
    if (Rc != 0)
        goto Done;

    memset(&Image, 0, sizeof(Image));
    BoxAsmImageInit(&Image);
    Rc = BoxAsmCollectSections(&Objects, &Image, Error, ErrorCapacity);
    if (Rc != 0)
        goto DoneImage;
    Rc = BoxAsmLayoutSections(&Image);
    if (Rc != 0)
        goto DoneImage;
    Rc = BoxAsmBuildNameplate(&Objects, &Image, Error, ErrorCapacity);
    if (Rc != 0)
        goto DoneImage;
    Rc = BoxAsmLayoutSections(&Image);
    if (Rc != 0)
        goto DoneImage;
    /* The second layout includes the generated Nameplate before writable data. */
    Rc = BoxAsmApplyRelocations(&Objects, &Image, Error, ErrorCapacity);
    if (Rc != 0)
        goto DoneImage;

    StartObject = &Objects.Items[0];
    MainObject = &Objects.Items[1];
    for (Index = 0; Index < StartObject->Elf.SymbolCount; Index++) {
        const char *Name = BoxAsmElfSymbolName(&StartObject->Elf, (uint32_t)Index);
        if (Name && strcmp(Name, "_start") == 0) StartIndex = (uint32_t)Index;
    }
    for (Index = 0; Index < MainObject->Elf.SymbolCount; Index++) {
        const char *Name = BoxAsmElfSymbolName(&MainObject->Elf, (uint32_t)Index);
        if (Name && strcmp(Name, "main") == 0) MainIndex = (uint32_t)Index;
    }
    if (BoxAsmResolveSymbol(&Image, &Objects, StartObject,
                            &StartObject->Elf.Symbols[StartIndex], &Image.Entry,
                            Error, ErrorCapacity) != 0 || Image.Entry == 0) {
        BoxAsmLinkError(Error, ErrorCapacity, "entry symbol _start is missing");
        Rc = -ERR_FILE_NOT_FOUND;
        goto DoneImage;
    }
    (void)MainIndex;
    Rc = BoxAsmWriteOutput(&Image, Out, Error, ErrorCapacity);

DoneImage:
    for (Index = 0; Index < 12; Index++)
        BoxAsmBufferRelease(&Image.Sections[Index].Data);
Done:
    for (Index = 2; Index < Objects.Count; Index++)
        free((void *)Objects.Items[Index].Name);
    BoxAsmObjectListRelease(&Objects);
    return Rc;
}
