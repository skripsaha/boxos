#include "boxasm_link_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define BOXASM_ARCHIVE_MAGIC        "!<arch>\n"
#define BOXASM_ARCHIVE_MAGIC_BYTES  8u
#define BOXASM_ARCHIVE_HEADER_BYTES 60u
#define BOXASM_OBJECT_NAME_MAX      256u

static const char *const BoxAsmSlotNames[BOXASM_SLOT_COUNT] = {
    "", ".text", ".rodata", ".eh_frame", ".gcc_except_table", ".nameplate",
    ".boxos.form", ".data", ".init_array", ".fini_array", ".tdata", ".tbss", ".bss"
};

/* The alignment user.ld gives each output section; inputs may raise it. */
static const uint64_t BoxAsmSlotAlign[BOXASM_SLOT_COUNT] = {
    1, BOXASM_PAGE, BOXASM_PAGE, 8, 8, 8, 1, BOXASM_PAGE, 8, 8, 64, 1, BOXASM_PAGE
};

static const uint64_t BoxAsmSlotFlags[BOXASM_SLOT_COUNT] = {
    0,
    BOXASM_SHF_ALLOC | BOXASM_SHF_EXECINSTR,
    BOXASM_SHF_ALLOC,
    BOXASM_SHF_ALLOC,
    BOXASM_SHF_ALLOC,
    BOXASM_SHF_ALLOC,
    BOXASM_SHF_ALLOC,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE | BOXASM_SHF_TLS,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE | BOXASM_SHF_TLS,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE
};

static void BoxAsmMessageSite(BoxAsmMessage *Message, const BoxAsmLinkObject *Object,
                              uint16_t Section, uint64_t Offset)
{
    const char *SectionName = BoxAsmElfSectionName(&Object->Elf, Section);

    BoxAsmMessageText(Message, Object->Name);
    BoxAsmMessageText(Message, "(");
    BoxAsmMessageText(Message, SectionName ? SectionName : "?");
    BoxAsmMessageText(Message, "+");
    BoxAsmMessageHex(Message, Offset);
    BoxAsmMessageText(Message, "): ");
}

/* A symbol's own name, or for a section symbol the name of its section. */
static const char *BoxAsmSymbolLabel(const BoxAsmLinkObject *Object, uint32_t Index)
{
    const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[Index];
    const char *Name = BoxAsmElfSymbolName(&Object->Elf, Index);

    if (BOXASM_SYMBOL_TYPE(Symbol->Info) == BOXASM_STT_SECTION &&
        Symbol->SectionIndex < Object->Elf.Header->SectionHeaderCount)
        return BoxAsmElfSectionName(&Object->Elf, Symbol->SectionIndex);
    return Name && Name[0] != '\0' ? Name : NULL;
}

static int BoxAsmObjectRefuse(BoxAsmLinker *Link, const char *ObjectName,
                              const char *Why)
{
    BoxAsmMessage Message;

    BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
    BoxAsmMessageText(&Message, ObjectName);
    BoxAsmMessageText(&Message, ": ");
    BoxAsmMessageText(&Message, Why);
    return -ERR_INVALID_ARGUMENT;
}

/* ---- Inputs ------------------------------------------------------------ */

static int BoxAsmObjectAdd(BoxAsmLinker *Link, const BoxAsmBlob *Blob,
                           const char *Name, uint8_t Selected)
{
    BoxAsmObjectList *List = &Link->Objects;
    BoxAsmLinkObject *Object;
    char Reason[160];
    uint16_t Count;

    if (List->Count == List->Capacity) {
        size_t Capacity = List->Capacity ? List->Capacity * 2u : 32u;
        BoxAsmLinkObject *Fresh;

        if (Capacity > SIZE_MAX / sizeof(*Fresh))
            return -ERR_NO_MEMORY;
        Fresh = (BoxAsmLinkObject *)realloc(List->Items, Capacity * sizeof(*Fresh));
        if (!Fresh)
            return -ERR_NO_MEMORY;
        List->Items = Fresh;
        List->Capacity = Capacity;
    }
    Object = &List->Items[List->Count];
    memset(Object, 0, sizeof(*Object));
    if (BoxAsmElfOpen(Blob, BOXASM_ET_REL, &Object->Elf, Reason, sizeof(Reason)) != 0)
        return BoxAsmObjectRefuse(Link, Name, Reason);
    Count = Object->Elf.Header->SectionHeaderCount;
    Object->Name = BoxAsmCopyText(Name, strlen(Name));
    Object->SectionSlot = (uint16_t *)malloc(Count * sizeof(uint16_t));
    Object->SectionOffset = (uint64_t *)malloc(Count * sizeof(uint64_t));
    if (!Object->Name || !Object->SectionSlot || !Object->SectionOffset) {
        free(Object->Name);
        free(Object->SectionSlot);
        free(Object->SectionOffset);
        return -ERR_NO_MEMORY;
    }
    memset(Object->SectionSlot, 0, Count * sizeof(uint16_t));
    memset(Object->SectionOffset, 0, Count * sizeof(uint64_t));
    Object->Selected = Selected;
    List->Count++;
    return 0;
}

static void BoxAsmObjectListRelease(BoxAsmObjectList *List)
{
    size_t Index;

    for (Index = 0; Index < List->Count; Index++) {
        free(List->Items[Index].Name);
        free(List->Items[Index].SectionSlot);
        free(List->Items[Index].SectionOffset);
    }
    free(List->Items);
    memset(List, 0, sizeof(*List));
}

static int BoxAsmParseDecimalField(const uint8_t *Field, uint32_t Width,
                                   uint64_t *Out)
{
    uint64_t Value = 0;
    uint32_t Index = 0;

    while (Index < Width && Field[Index] >= '0' && Field[Index] <= '9') {
        uint64_t Digit = (uint64_t)(Field[Index] - '0');
        if (Value > (UINT64_MAX - Digit) / 10u)
            return -ERR_INVALID_ARGUMENT;
        Value = Value * 10u + Digit;
        Index++;
    }
    if (Index == 0)
        return -ERR_INVALID_ARGUMENT;
    while (Index < Width && Field[Index] == ' ')
        Index++;
    if (Index != Width)
        return -ERR_INVALID_ARGUMENT;
    *Out = Value;
    return 0;
}

/* A member's name in the GNU ar format: "name.o/" in the header, or "/N"
 * pointing at "name.o/\n" in the long-name member "//". */
static int BoxAsmArchiveMemberName(const uint8_t *Header, const uint8_t *LongNames,
                                   uint64_t LongBytes, char *Out, size_t Capacity)
{
    const uint8_t *Name = Header;
    uint64_t Limit = 16u;
    uint64_t Length = 0;

    if (Header[0] == '/' && Header[1] >= '0' && Header[1] <= '9') {
        uint64_t Offset;
        uint32_t Width = 1u;

        while (Width < 15u && Header[1u + Width] != ' ')
            Width++;
        if (BoxAsmParseDecimalField(Header + 1, Width, &Offset) != 0 ||
            !LongNames || Offset >= LongBytes)
            return -ERR_INVALID_ARGUMENT;
        Name = LongNames + Offset;
        Limit = LongBytes - Offset;
    }
    while (Length < Limit && Name[Length] != '/' && Name[Length] != '\n')
        Length++;
    if (Length == 0 || Length >= Limit || Name[Length] != '/' ||
        Length >= Capacity)
        return -ERR_INVALID_ARGUMENT;
    memcpy(Out, Name, (size_t)Length);
    Out[Length] = '\0';
    return 0;
}

static int BoxAsmArchiveAddMember(BoxAsmLinker *Link, const BoxAsmBlob *Member,
                                  const char *MemberName)
{
    char Name[BOXASM_OBJECT_NAME_MAX];
    BoxAsmMessage Message;

    BoxAsmMessageBegin(&Message, Name, sizeof(Name));
    BoxAsmMessageText(&Message, Link->ArchiveName);
    BoxAsmMessageText(&Message, "(");
    BoxAsmMessageText(&Message, MemberName);
    BoxAsmMessageText(&Message, ")");
    return BoxAsmObjectAdd(Link, Member, Name, 0);
}

/* Every ELF member of the archive becomes a candidate object. The symbol
 * index "/" is not consulted: the members' own symbol tables are the truth
 * it summarizes. A member that is not ELF is not an object and is passed
 * over; one that claims to be ELF and is malformed is refused. */
static int BoxAsmArchiveScan(BoxAsmLinker *Link, const BoxAsmBlob *Archive)
{
    const uint8_t *LongNames = NULL;
    uint64_t LongBytes = 0;
    uint64_t Position = BOXASM_ARCHIVE_MAGIC_BYTES;

    if (Archive->Size < BOXASM_ARCHIVE_MAGIC_BYTES ||
        memcmp(Archive->Data, BOXASM_ARCHIVE_MAGIC, BOXASM_ARCHIVE_MAGIC_BYTES) != 0)
        return BoxAsmObjectRefuse(Link, Link->ArchiveName, "not an ar archive");
    while (Position < Archive->Size) {
        const uint8_t *Header = Archive->Data + Position;
        uint64_t Size;
        uint64_t Data;
        BoxAsmBlob Member;
        char MemberName[BOXASM_OBJECT_NAME_MAX];
        int Rc;

        if (!BoxAsmRangeValid(Archive->Size, Position, BOXASM_ARCHIVE_HEADER_BYTES) ||
            Header[58] != '`' || Header[59] != '\n' ||
            BoxAsmParseDecimalField(Header + 48, 10u, &Size) != 0)
            return BoxAsmObjectRefuse(Link, Link->ArchiveName,
                                      "an archive member header is malformed");
        Data = Position + BOXASM_ARCHIVE_HEADER_BYTES;
        if (!BoxAsmRangeValid(Archive->Size, Data, Size))
            return BoxAsmObjectRefuse(Link, Link->ArchiveName,
                                      "an archive member runs past the archive");
        Member = (BoxAsmBlob){ Archive->Data + Data, Size, NULL };
        if (memcmp(Header, "//", 2u) == 0 && Header[2] == ' ') {
            LongNames = Member.Data;
            LongBytes = Size;
        } else if (Header[0] == '/' && (Header[1] == ' ' || Header[1] == 'S')) {
            /* The symbol index ("/" or "/SYM64/"). */
        } else if (Size >= 4u && Member.Data[0] == BOXASM_ELF_MAGIC0 &&
                   Member.Data[1] == BOXASM_ELF_MAGIC1 &&
                   Member.Data[2] == BOXASM_ELF_MAGIC2 &&
                   Member.Data[3] == BOXASM_ELF_MAGIC3) {
            if (BoxAsmArchiveMemberName(Header, LongNames, LongBytes, MemberName,
                                        sizeof(MemberName)) != 0)
                return BoxAsmObjectRefuse(Link, Link->ArchiveName,
                                          "an archive member's name is malformed");
            Rc = BoxAsmArchiveAddMember(Link, &Member, MemberName);
            if (Rc != 0)
                return Rc;
        }
        Position = Data + Size + (Size & 1u);
    }
    return 0;
}

/* ---- Sections ---------------------------------------------------------- */

static int BoxAsmNameIs(const char *Name, const char *Base)
{
    size_t Length = strlen(Base);

    return strncmp(Name, Base, Length) == 0 &&
           (Name[Length] == '\0' || Name[Length] == '.');
}

/* The output section user.ld gives an input section, BOXASM_SLOT_NONE for one
 * the image does not carry, or -1 for an allocated section the layout has no
 * place for. Prioritized constructor sections are refused rather than placed
 * in an order user.ld's SORT_BY_INIT_PRIORITY would not give them. */
static int BoxAsmSlotOf(const BoxAsmElfObject *Object, uint16_t Index)
{
    const BoxAsmElfSectionHeader *Section = &Object->Sections[Index];
    const char *Name = BoxAsmElfSectionName(Object, Index);
    int Slot = -1;

    if (Section->Type == BOXASM_SHT_NULL || Section->Type == BOXASM_SHT_RELA ||
        Section->Type == BOXASM_SHT_SYMTAB || Section->Type == BOXASM_SHT_STRTAB)
        return BOXASM_SLOT_NONE;
    if (!Name)
        return -1;
    if (strcmp(Name, ".comment") == 0 || strncmp(Name, ".note", 5u) == 0 ||
        (Section->Flags & BOXASM_SHF_ALLOC) == 0)
        return BOXASM_SLOT_NONE;
    if (BoxAsmNameIs(Name, ".text"))                   Slot = BOXASM_SLOT_TEXT;
    else if (BoxAsmNameIs(Name, ".rodata"))            Slot = BOXASM_SLOT_RODATA;
    else if (BoxAsmNameIs(Name, ".eh_frame"))          Slot = BOXASM_SLOT_EH_FRAME;
    else if (BoxAsmNameIs(Name, ".gcc_except_table"))  Slot = BOXASM_SLOT_EXCEPT_TABLE;
    else if (strcmp(Name, ".boxos.form") == 0)         Slot = BOXASM_SLOT_FORM;
    else if (BoxAsmNameIs(Name, ".data"))              Slot = BOXASM_SLOT_DATA;
    else if (strcmp(Name, ".init_array") == 0)         Slot = BOXASM_SLOT_INIT_ARRAY;
    else if (strcmp(Name, ".fini_array") == 0)         Slot = BOXASM_SLOT_FINI_ARRAY;
    else if (BoxAsmNameIs(Name, ".tdata"))             Slot = BOXASM_SLOT_TDATA;
    else if (BoxAsmNameIs(Name, ".tbss") || strcmp(Name, ".tcommon") == 0)
        Slot = BOXASM_SLOT_TBSS;
    else if (BoxAsmNameIs(Name, ".bss"))               Slot = BOXASM_SLOT_BSS;
    if (Slot < 0)
        return -1;
    if (((Section->Flags & BOXASM_SHF_TLS) != 0) !=
        ((BoxAsmSlotFlags[Slot] & BOXASM_SHF_TLS) != 0))
        return -1;
    return Slot;
}

static void BoxAsmImageInit(BoxAsmLinkImage *Image)
{
    size_t Slot;

    memset(Image, 0, sizeof(*Image));
    for (Slot = 1; Slot < BOXASM_SLOT_COUNT; Slot++) {
        BoxAsmOutputSection *Section = &Image->Sections[Slot];

        Section->Name = BoxAsmSlotNames[Slot];
        Section->Flags = BoxAsmSlotFlags[Slot];
        Section->Align = BoxAsmSlotAlign[Slot];
        Section->Type = Slot == BOXASM_SLOT_TBSS || Slot == BOXASM_SLOT_BSS
                        ? BOXASM_SHT_NOBITS : BOXASM_SHT_PROGBITS;
    }
}

static void BoxAsmImageRelease(BoxAsmLinkImage *Image)
{
    size_t Slot;

    for (Slot = 0; Slot < BOXASM_SLOT_COUNT; Slot++)
        BoxAsmBufferRelease(&Image->Sections[Slot].Data);
    BoxAsmBufferRelease(&Image->Symbols);
    BoxAsmBufferRelease(&Image->SymbolNames);
}

static int BoxAsmSectionRefuse(BoxAsmLinker *Link, const BoxAsmLinkObject *Object,
                               uint16_t Index, const char *Why)
{
    BoxAsmMessage Message;

    BoxAsmMessageBegin(&Message, Link->Error, Link->ErrorCapacity);
    BoxAsmMessageText(&Message, Object->Name);
    BoxAsmMessageText(&Message, ": section ");
    BoxAsmMessageName(&Message, BoxAsmElfSectionName(&Object->Elf, Index));
    BoxAsmMessageText(&Message, Why);
    return -ERR_INVALID_ARGUMENT;
}

static int BoxAsmCollectSection(BoxAsmLinker *Link, BoxAsmLinkObject *Object,
                                uint16_t Index)
{
    const BoxAsmElfSectionHeader *Input = &Object->Elf.Sections[Index];
    BoxAsmOutputSection *Output;
    int Slot = BoxAsmSlotOf(&Object->Elf, Index);
    uint64_t Align = Input->AddressAlign ? Input->AddressAlign : 1u;
    uint64_t Offset;

    if (Slot < 0)
        return BoxAsmSectionRefuse(Link, Object, Index,
                                   " has no place in the BoxOS image layout");
    if (Slot == BOXASM_SLOT_NONE)
        return 0;
    if ((Align & (Align - 1u)) != 0)
        return BoxAsmSectionRefuse(Link, Object, Index,
                                   " has an alignment that is not a power of two");
    Output = &Link->Image.Sections[Slot];
    if (Output->Type == BOXASM_SHT_NOBITS && Input->Type != BOXASM_SHT_NOBITS)
        return BoxAsmSectionRefuse(Link, Object, Index,
                                   " carries bytes into a section that has none");
    Offset = BoxAsmAlignUp(Output->Size, Align);
    if (Offset == UINT64_MAX || Input->Size > UINT64_MAX - Offset)
        return BoxAsmSectionRefuse(Link, Object, Index, " does not fit in the image");
    if (Align > Output->Align)
        Output->Align = Align;
    Object->SectionSlot[Index] = (uint16_t)Slot;
    Object->SectionOffset[Index] = Offset;
    Output->Size = Offset + Input->Size;
    if (Output->Type == BOXASM_SHT_NOBITS)
        return 0;
    if (BoxAsmBufferAppendZeroes(&Output->Data, Offset - Output->Data.Size) != 0)
        return -ERR_NO_MEMORY;
    if (Input->Type == BOXASM_SHT_NOBITS)
        return BoxAsmBufferAppendZeroes(&Output->Data, Input->Size);
    return BoxAsmBufferAppend(&Output->Data, BoxAsmElfSectionData(&Object->Elf, Index),
                              Input->Size);
}

/* Selected objects' sections in object order — the entry object first, so
 * its .text opens the image as user.ld requires. */
static int BoxAsmCollectSections(BoxAsmLinker *Link)
{
    size_t ObjectIndex;

    for (ObjectIndex = 0; ObjectIndex < Link->Objects.Count; ObjectIndex++) {
        BoxAsmLinkObject *Object = &Link->Objects.Items[ObjectIndex];
        uint16_t Index;

        if (!Object->Selected)
            continue;
        for (Index = 0; Index < Object->Elf.Header->SectionHeaderCount; Index++) {
            int Rc = BoxAsmCollectSection(Link, Object, Index);
            if (Rc != 0)
                return Rc;
        }
    }
    return 0;
}

/* Every output section gets an aligned address, empty or not, so the symbols
 * user.ld defines inside it have one. As in GNU ld, only a section with
 * contents moves the location counter — an empty one leaves no gap — and
 * .tbss never does: its addresses describe the TLS template, not memory. */
static int BoxAsmLayoutSections(BoxAsmLinker *Link)
{
    BoxAsmLinkImage *Image = &Link->Image;
    const BoxAsmOutputSection *Tdata = &Image->Sections[BOXASM_SLOT_TDATA];
    const BoxAsmOutputSection *Tbss = &Image->Sections[BOXASM_SLOT_TBSS];
    uint64_t Address = BOXASM_USER_BASE;
    size_t Slot;

    for (Slot = 1; Slot < BOXASM_SLOT_COUNT; Slot++) {
        BoxAsmOutputSection *Section = &Image->Sections[Slot];
        uint64_t Aligned = BoxAsmAlignUp(Address, Section->Align);

        if (Aligned == UINT64_MAX || Section->Size > UINT64_MAX - Aligned) {
            BoxAsmMessageSet(Link->Error, Link->ErrorCapacity,
                             "the image does not fit in the address space");
            return -ERR_NO_MEMORY;
        }
        Section->Address = Aligned;
        /* .data opens the RW segment: the writable pages begin there even
         * when it is empty, so no writable byte shares a page with code. */
        if (Slot == BOXASM_SLOT_DATA)
            Address = Aligned;
        if (Section->Size != 0 && Slot != BOXASM_SLOT_TBSS)
            Address = Aligned + Section->Size;
    }
    Image->ProgramEnd = Address;

    Image->TlsAddress = 0;
    Image->TlsMemorySize = 0;
    Image->TlsAlign = 0;
    Image->ThreadPointer = 0;
    if (Tdata->Size == 0 && Tbss->Size == 0)
        return 0;
    Image->TlsAddress = Tdata->Size != 0 ? Tdata->Address : Tbss->Address;
    Image->TlsMemorySize = (Tbss->Size != 0 ? Tbss->Address + Tbss->Size
                                            : Tdata->Address + Tdata->Size) -
                           Image->TlsAddress;
    Image->TlsAlign = Tdata->Size != 0 ? Tdata->Align : 1u;
    if (Tbss->Size != 0 && Tbss->Align > Image->TlsAlign)
        Image->TlsAlign = Tbss->Align;
    Image->ThreadPointer = Image->TlsAddress +
                           BoxAsmAlignUp(Image->TlsMemorySize, Image->TlsAlign);
    return 0;
}

/* ---- Symbols ----------------------------------------------------------- */

/* The names user.ld defines, valued by the layout. */
static int BoxAsmLinkerSymbol(const BoxAsmLinkImage *Image, const char *Name,
                              uint64_t *Out)
{
    static const struct {
        const char *Name;
        uint8_t     Slot;
        uint8_t     End;
    } Bounds[] = {
        { "__eh_frame_start",   BOXASM_SLOT_EH_FRAME,   0 },
        { "__eh_frame_end",     BOXASM_SLOT_EH_FRAME,   1 },
        { "__nameplate_start",  BOXASM_SLOT_NAMEPLATE,  0 },
        { "__nameplate_end",    BOXASM_SLOT_NAMEPLATE,  1 },
        { "__boxos_form_start", BOXASM_SLOT_FORM,       0 },
        { "__boxos_form_end",   BOXASM_SLOT_FORM,       1 },
        { "__init_array_start", BOXASM_SLOT_INIT_ARRAY, 0 },
        { "__init_array_end",   BOXASM_SLOT_INIT_ARRAY, 1 },
        { "__fini_array_start", BOXASM_SLOT_FINI_ARRAY, 0 },
        { "__fini_array_end",   BOXASM_SLOT_FINI_ARRAY, 1 },
        { "__tdata_start",      BOXASM_SLOT_TDATA,      0 },
        { "__tdata_end",        BOXASM_SLOT_TDATA,      1 },
        { "__bss_start",        BOXASM_SLOT_BSS,        0 },
        { "__bss_end",          BOXASM_SLOT_BSS,        1 },
    };
    size_t Index;

    for (Index = 0; Index < sizeof(Bounds) / sizeof(Bounds[0]); Index++) {
        const BoxAsmOutputSection *Section = &Image->Sections[Bounds[Index].Slot];
        if (strcmp(Name, Bounds[Index].Name) != 0)
            continue;
        *Out = Section->Address + (Bounds[Index].End ? Section->Size : 0);
        return 0;
    }
    if (strcmp(Name, "__tdata_size") == 0) {
        *Out = Image->Sections[BOXASM_SLOT_TDATA].Size;
        return 0;
    }
    if (strcmp(Name, "__tbss_size") == 0) {
        *Out = Image->Sections[BOXASM_SLOT_TBSS].Size;
        return 0;
    }
    if (strcmp(Name, "__program_end") == 0) {
        *Out = Image->ProgramEnd;
        return 0;
    }
    return -ERR_FILE_NOT_FOUND;
}

/* The address of a symbol its own object defines. -ERR_FILE_NOT_FOUND when
 * it lies in a section the image does not carry. */
int BoxAsmSymbolAddress(const BoxAsmLinker *Link, const BoxAsmLinkObject *Object,
                        const BoxAsmElfSymbol *Symbol, uint64_t *Out)
{
    uint16_t Slot;

    if (Symbol->SectionIndex == BOXASM_SHN_ABS) {
        *Out = Symbol->Value;
        return 0;
    }
    if (Symbol->SectionIndex == BOXASM_SHN_UNDEF ||
        Symbol->SectionIndex == BOXASM_SHN_COMMON)
        return -ERR_INVALID_ARGUMENT;
    Slot = Object->SectionSlot[Symbol->SectionIndex];
    if (Slot == BOXASM_SLOT_NONE)
        return -ERR_FILE_NOT_FOUND;
    *Out = Link->Image.Sections[Slot].Address +
           Object->SectionOffset[Symbol->SectionIndex] + Symbol->Value;
    return 0;
}

/* A symbol followed to the definition that prevails for its name, then to a
 * name user.ld defines, then — for a weak reference only — to zero. */
static int BoxAsmResolve(const BoxAsmLinker *Link, const BoxAsmLinkObject *Object,
                         uint32_t Index, BoxAsmResolved *Out)
{
    const BoxAsmElfSymbol *Symbol = &Object->Elf.Symbols[Index];
    const char *Name;
    const BoxAsmGlobal *Global;

    memset(Out, 0, sizeof(*Out));
    if (Symbol->SectionIndex != BOXASM_SHN_UNDEF) {
        Out->Object = Object;
        Out->Symbol = Symbol;
        return BoxAsmSymbolAddress(Link, Object, Symbol, &Out->Address);
    }
    Name = BoxAsmElfSymbolName(&Object->Elf, Index);
    if (!Name || Name[0] == '\0' ||
        BOXASM_SYMBOL_BIND(Symbol->Info) == BOXASM_STB_LOCAL)
        return -ERR_INVALID_ARGUMENT;
    Global = BoxAsmGlobalFind(&Link->Globals, Name);
    if (Global) {
        Out->Object = &Link->Objects.Items[Global->Object];
        Out->Symbol = &Out->Object->Elf.Symbols[Global->Symbol];
        return BoxAsmSymbolAddress(Link, Out->Object, Out->Symbol, &Out->Address);
    }
    if (BoxAsmLinkerSymbol(&Link->Image, Name, &Out->Address) == 0)
        return 0;
    if (BOXASM_SYMBOL_BIND(Symbol->Info) == BOXASM_STB_WEAK)
        return 0;
    return -ERR_FILE_NOT_FOUND;
}

static int BoxAsmResolvedIsTls(const BoxAsmResolved *Resolved)
{
    uint8_t Type;

    if (!Resolved->Symbol)
        return 0;
    Type = BOXASM_SYMBOL_TYPE(Resolved->Symbol->Info);
    if (Type == BOXASM_STT_TLS)
        return 1;
    return Type == BOXASM_STT_SECTION &&
           (Resolved->Object->Elf.Sections[Resolved->Symbol->SectionIndex].Flags &
            BOXASM_SHF_TLS) != 0;
}

/* ---- Relocations ------------------------------------------------------- */

static const char *BoxAsmRelocationName(uint32_t Type)
{
    switch (Type) {
    case BOXASM_R_X86_64_64:      return "R_X86_64_64";
    case BOXASM_R_X86_64_PC32:    return "R_X86_64_PC32";
    case BOXASM_R_X86_64_PLT32:   return "R_X86_64_PLT32";
    case BOXASM_R_X86_64_32:      return "R_X86_64_32";
    case BOXASM_R_X86_64_32S:     return "R_X86_64_32S";
    case BOXASM_R_X86_64_TPOFF32: return "R_X86_64_TPOFF32";
    default:                      return NULL;
    }
}

typedef struct {
    BoxAsmLinker           *Link;
    const BoxAsmLinkObject *Object;
    uint16_t                Section;
    uint64_t                Offset;
    uint32_t                Type;
    const char             *Symbol;
} BoxAsmRelocationSite;

static int BoxAsmRelocationRefuse(const BoxAsmRelocationSite *Site, const char *Why,
                                  const uint64_t *Value)
{
    BoxAsmMessage Message;
    const char *TypeName = BoxAsmRelocationName(Site->Type);

    BoxAsmMessageBegin(&Message, Site->Link->Error, Site->Link->ErrorCapacity);
    BoxAsmMessageSite(&Message, Site->Object, Site->Section, Site->Offset);
    if (TypeName) {
        BoxAsmMessageText(&Message, TypeName);
    } else {
        BoxAsmMessageText(&Message, "relocation type ");
        BoxAsmMessageDecimal(&Message, Site->Type);
    }
    BoxAsmMessageText(&Message, " against ");
    BoxAsmMessageName(&Message, Site->Symbol);
    BoxAsmMessageText(&Message, Why);
    if (Value) {
        BoxAsmMessageText(&Message, " (value ");
        BoxAsmMessageHex(&Message, *Value);
        BoxAsmMessageText(&Message, ")");
    }
    return -ERR_INVALID_ARGUMENT;
}

/* S + A, S + A - P or S + A - TP, checked to fit the field it is written to. */
static int BoxAsmRelocationValue(const BoxAsmRelocationSite *Site,
                                 const BoxAsmResolved *Resolved, int64_t Addend,
                                 uint64_t Place, uint64_t *Out)
{
    const BoxAsmLinkImage *Image = &Site->Link->Image;
    uint64_t Sum = Resolved->Address + (uint64_t)Addend;
    int64_t Value;

    if (Site->Type == BOXASM_R_X86_64_64) {
        *Out = Sum;
        return 0;
    }
    if (Resolved->Address > (uint64_t)INT64_MAX ||
        (Addend > 0 && Resolved->Address > (uint64_t)(INT64_MAX - Addend)))
        return BoxAsmRelocationRefuse(Site, " overflows its computation", &Sum);
    Value = (int64_t)Resolved->Address + Addend;
    if (Site->Type == BOXASM_R_X86_64_PC32 || Site->Type == BOXASM_R_X86_64_PLT32)
        Value -= (int64_t)Place;
    else if (Site->Type == BOXASM_R_X86_64_TPOFF32)
        Value -= (int64_t)Image->ThreadPointer;
    *Out = (uint64_t)Value;
    if (Site->Type == BOXASM_R_X86_64_32 ? (Value < 0 || Value > (int64_t)UINT32_MAX)
                                         : (Value < INT32_MIN || Value > INT32_MAX))
        return BoxAsmRelocationRefuse(Site, " does not fit in 32 bits", Out);
    return 0;
}

static int BoxAsmApplyRelocation(BoxAsmRelocationSite *Site, const BoxAsmElfRela *Entry,
                                 uint64_t TargetSize, uint8_t *Bytes,
                                 uint64_t SectionAddress)
{
    const BoxAsmLinkObject *Object = Site->Object;
    uint32_t SymbolIndex = BOXASM_RELA_SYMBOL(Entry->Info);
    uint32_t Width;
    BoxAsmResolved Resolved;
    uint64_t Value;
    int Rc;

    Site->Type = BOXASM_RELA_TYPE(Entry->Info);
    Site->Offset = Entry->Address;
    Site->Symbol = NULL;
    if (SymbolIndex >= Object->Elf.SymbolCount)
        return BoxAsmRelocationRefuse(Site, " names a symbol the object does not have", NULL);
    Site->Symbol = BoxAsmSymbolLabel(Object, SymbolIndex);
    if (!BoxAsmRelocationName(Site->Type))
        return BoxAsmRelocationRefuse(Site, " is a relocation this linker does not apply", NULL);
    Width = Site->Type == BOXASM_R_X86_64_64 ? 8u : 4u;
    if (!BoxAsmRangeValid(TargetSize, Entry->Address, Width))
        return BoxAsmRelocationRefuse(Site, " lies outside its section", NULL);
    Rc = BoxAsmResolve(Site->Link, Object, SymbolIndex, &Resolved);
    if (Rc == -ERR_FILE_NOT_FOUND && !Resolved.Object)
        return BoxAsmRelocationRefuse(Site, ": the symbol is defined nowhere", NULL);
    if (Rc == -ERR_FILE_NOT_FOUND)
        return BoxAsmRelocationRefuse(Site, ": the symbol lies in a section the image does not carry", NULL);
    if (Rc != 0)
        return BoxAsmRelocationRefuse(Site, ": the symbol cannot be given an address", NULL);
    if (Site->Type == BOXASM_R_X86_64_TPOFF32) {
        if (!BoxAsmResolvedIsTls(&Resolved))
            return BoxAsmRelocationRefuse(Site, ": the symbol is not thread-local", NULL);
        if (Site->Link->Image.TlsMemorySize == 0)
            return BoxAsmRelocationRefuse(Site, ": the image has no TLS block", NULL);
    } else if (BoxAsmResolvedIsTls(&Resolved)) {
        return BoxAsmRelocationRefuse(Site,
            ": a thread-local symbol is reached only through R_X86_64_TPOFF32", NULL);
    }
    Rc = BoxAsmRelocationValue(Site, &Resolved, Entry->Addend,
                               SectionAddress + Entry->Address, &Value);
    if (Rc != 0)
        return Rc;
    memcpy(Bytes + Entry->Address, &Value, Width);
    return 0;
}

static int BoxAsmRelocateSection(BoxAsmLinker *Link, BoxAsmLinkObject *Object,
                                 uint16_t RelaIndex)
{
    const BoxAsmElfSectionHeader *Rela = &Object->Elf.Sections[RelaIndex];
    const BoxAsmElfRela *Entries;
    BoxAsmOutputSection *Output;
    BoxAsmRelocationSite Site;
    uint16_t Slot;
    uint64_t Count;
    uint64_t Index;

    if (Rela->Info >= Object->Elf.Header->SectionHeaderCount ||
        Rela->EntrySize != sizeof(BoxAsmElfRela) ||
        Rela->Size % sizeof(BoxAsmElfRela) != 0)
        return BoxAsmSectionRefuse(Link, Object, RelaIndex, " is a malformed relocation table");
    Slot = Object->SectionSlot[Rela->Info];
    if (Slot == BOXASM_SLOT_NONE)
        return 0;
    Output = &Link->Image.Sections[Slot];
    if (Output->Type == BOXASM_SHT_NOBITS)
        return BoxAsmSectionRefuse(Link, Object, RelaIndex,
                                   " relocates a section that has no bytes");
    Site = (BoxAsmRelocationSite){ Link, Object, (uint16_t)Rela->Info, 0, 0, NULL };
    Entries = (const BoxAsmElfRela *)(Object->Elf.Blob.Data + Rela->Offset);
    Count = Rela->Size / sizeof(BoxAsmElfRela);
    if (Count != 0 && Object->Elf.Sections[Rela->Info].Size == 0)
        return BoxAsmSectionRefuse(Link, Object, RelaIndex,
                                   " relocates a section that is empty");
    for (Index = 0; Index < Count; Index++) {
        int Rc = BoxAsmApplyRelocation(
            &Site, &Entries[Index], Object->Elf.Sections[Rela->Info].Size,
            Output->Data.Data + Object->SectionOffset[Rela->Info],
            Output->Address + Object->SectionOffset[Rela->Info]);
        if (Rc != 0)
            return Rc;
    }
    return 0;
}

static int BoxAsmApplyRelocations(BoxAsmLinker *Link)
{
    size_t ObjectIndex;

    for (ObjectIndex = 0; ObjectIndex < Link->Objects.Count; ObjectIndex++) {
        BoxAsmLinkObject *Object = &Link->Objects.Items[ObjectIndex];
        uint16_t Index;

        if (!Object->Selected)
            continue;
        for (Index = 0; Index < Object->Elf.Header->SectionHeaderCount; Index++) {
            int Rc;
            if (Object->Elf.Sections[Index].Type != BOXASM_SHT_RELA)
                continue;
            Rc = BoxAsmRelocateSection(Link, Object, Index);
            if (Rc != 0)
                return Rc;
        }
    }
    return 0;
}

/* ---- The link ---------------------------------------------------------- */

static int BoxAsmFindEntry(BoxAsmLinker *Link)
{
    const BoxAsmGlobal *Start = BoxAsmGlobalFind(&Link->Globals, "_start");
    const BoxAsmLinkObject *Object;

    if (!Start) {
        BoxAsmMessageSet(Link->Error, Link->ErrorCapacity,
                         "the entry symbol '_start' is defined nowhere");
        return -ERR_FILE_NOT_FOUND;
    }
    Object = &Link->Objects.Items[Start->Object];
    if (BoxAsmSymbolAddress(Link, Object, &Object->Elf.Symbols[Start->Symbol],
                            &Link->Image.Entry) != 0 ||
        Link->Image.Entry < Link->Image.Sections[BOXASM_SLOT_TEXT].Address ||
        Link->Image.Entry >= Link->Image.Sections[BOXASM_SLOT_TEXT].Address +
                             Link->Image.Sections[BOXASM_SLOT_TEXT].Size)
        return BoxAsmObjectRefuse(Link, Object->Name,
                                  "the entry symbol '_start' is not in .text");
    return 0;
}

/* The Nameplate is sized after the code has addresses and placed after it, so
 * the second layout moves data and never a function; that is checked here
 * rather than assumed, because the table would name the wrong code. */
static int BoxAsmPlaceNameplate(BoxAsmLinker *Link)
{
    uint64_t Before[BOXASM_SLOT_NAMEPLATE];
    size_t Slot;
    int Rc;

    for (Slot = 1; Slot < BOXASM_SLOT_NAMEPLATE; Slot++)
        Before[Slot] = Link->Image.Sections[Slot].Address;
    Rc = BoxAsmBuildNameplate(Link);
    if (Rc == 0)
        Rc = BoxAsmLayoutSections(Link);
    for (Slot = 1; Rc == 0 && Slot < BOXASM_SLOT_NAMEPLATE; Slot++) {
        if (Before[Slot] != Link->Image.Sections[Slot].Address) {
            BoxAsmMessageSet(Link->Error, Link->ErrorCapacity,
                             "placing the Nameplate moved code it names");
            Rc = -ERR_INVALID_STATE;
        }
    }
    return Rc;
}

int BoxAsmLink(const BoxAsmBlob *Entry, const BoxAsmBlob *Program,
               const BoxAsmBlob *RuntimeArchive, const BoxAsmBlob *Recipe, uint32_t Flags,
               BoxAsmBlob *Out, char *Error, size_t ErrorCapacity)
{
    BoxAsmLinker Link;
    int Rc;

    BoxAsmMessageSet(Error, ErrorCapacity, "");
    if (!Entry || !Program || !RuntimeArchive || !Out) {
        BoxAsmMessageSet(Error, ErrorCapacity, "link input is missing");
        return -ERR_INVALID_ARGUMENT;
    }
    memset(Out, 0, sizeof(*Out));
    memset(&Link, 0, sizeof(Link));
    Link.Error = Error;
    Link.ErrorCapacity = ErrorCapacity;
    Link.Recipe = Recipe && Recipe->Size != 0 ? Recipe : NULL;
    Link.ArchiveName = RuntimeArchive->Name ? RuntimeArchive->Name : "runtime archive";
    BoxAsmImageInit(&Link.Image);

    Rc = BoxAsmObjectAdd(&Link, Entry, Entry->Name ? Entry->Name : "entry object", 1);
    if (Rc == 0)
        Rc = BoxAsmObjectAdd(&Link, Program, Program->Name ? Program->Name : "program", 1);
    if (Rc == 0)
        Rc = BoxAsmArchiveScan(&Link, RuntimeArchive);
    if (Rc == 0)
        Rc = BoxAsmLinkSelect(&Link);
    if (Rc == 0)
        Rc = BoxAsmCollectSections(&Link);
    if (Rc == 0)
        Rc = BoxAsmLayoutSections(&Link);
    if (Rc == 0)
        Rc = BoxAsmPlaceNameplate(&Link);
    if (Rc == 0)
        Rc = BoxAsmApplyRelocations(&Link);
    if (Rc == 0)
        Rc = BoxAsmFindEntry(&Link);
    if (Rc == 0 && (Flags & BOXASM_LINK_SYMBOLS) != 0)
        Rc = BoxAsmBuildSymbolTable(&Link);
    if (Rc == 0)
        Rc = BoxAsmWriteOutput(&Link, Out);
    if (Rc == -ERR_NO_MEMORY && Error && ErrorCapacity != 0 && Error[0] == '\0')
        BoxAsmMessageSet(Error, ErrorCapacity, "out of memory while linking");

    BoxAsmImageRelease(&Link.Image);
    BoxAsmGlobalRelease(&Link.Globals);
    BoxAsmObjectListRelease(&Link.Objects);
    return Rc;
}
