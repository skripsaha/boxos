#include "boxasm_asm.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define BOXASM_SHF_INFO_LINK 0x40ull

static const uint64_t BoxAsmSectionFlags[BOXASM_SECTION_COUNT] = {
    BOXASM_SHF_ALLOC | BOXASM_SHF_EXECINSTR,
    BOXASM_SHF_ALLOC,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE,
    BOXASM_SHF_ALLOC | BOXASM_SHF_WRITE
};

/* Which header index each piece of the object takes. */
typedef struct {
    uint16_t Section[BOXASM_SECTION_COUNT];   /* 0: not in the object */
    uint16_t Form;                            /* .boxos.form: the program's Form */
    uint16_t Settings;                        /* .boxos.settings: the settings it declares */
    uint16_t Life;                            /* .boxos.life: how it lives */
    uint16_t Rela[BOXASM_SECTION_COUNT];
    uint16_t Symtab;
    uint16_t Strtab;
    uint16_t Shstrtab;
    uint16_t Count;
} BoxAsmPlan;

typedef struct {
    BoxAsmBuffer Symbols;
    BoxAsmBuffer Names;
    uint32_t     FirstGlobal;
} BoxAsmSymbolTable;

static void BoxAsmPlanHeaders(const BoxAsmAssembler *Asm, BoxAsmPlan *Plan)
{
    uint16_t Next = 1;
    size_t Index;

    memset(Plan, 0, sizeof(*Plan));
    for (Index = 0; Index < BOXASM_SECTION_COUNT; Index++)
        if (Asm->Sections[Index].Used)
            Plan->Section[Index] = Next++;
    if (Asm->Form.Text.Size != 0)
        Plan->Form = Next++;
    if (Asm->Settings.Size != 0)
        Plan->Settings = Next++;
    if (Asm->Life.Size != 0)
        Plan->Life = Next++;
    for (Index = 0; Index < BOXASM_SECTION_COUNT; Index++)
        if (Asm->Sections[Index].RelocationCount != 0)
            Plan->Rela[Index] = Next++;
    Plan->Symtab = Next++;
    Plan->Strtab = Next++;
    Plan->Shstrtab = Next++;
    Plan->Count = Next;
}

static int BoxAsmAddElfSymbol(BoxAsmSymbolTable *Table, const char *Name, uint8_t Info,
                              uint16_t Section, uint64_t Value, uint32_t *Index)
{
    BoxAsmElfSymbol Entry;

    if (Table->Names.Size > UINT32_MAX || Table->Symbols.Size / sizeof(Entry) >= UINT32_MAX)
        return -ERR_NO_MEMORY;
    memset(&Entry, 0, sizeof(Entry));
    Entry.Name = (uint32_t)Table->Names.Size;
    Entry.Info = Info;
    Entry.SectionIndex = Section;
    Entry.Value = Value;
    *Index = (uint32_t)(Table->Symbols.Size / sizeof(Entry));
    if (BoxAsmBufferAppend(&Table->Names, Name, strlen(Name) + 1u) != 0 ||
        BoxAsmBufferAppend(&Table->Symbols, &Entry, sizeof(Entry)) != 0)
        return -ERR_NO_MEMORY;
    return 0;
}

/* Locals first — the null symbol, the sections, the labels no one exports —
 * then the global labels and the outside names a relocation uses. */
static int BoxAsmBuildSymbols(BoxAsmAssembler *Asm, const BoxAsmPlan *Plan,
                              BoxAsmSymbolTable *Table)
{
    uint32_t Ignored;
    size_t Index;
    uint8_t Pass;
    int Rc = BoxAsmAddElfSymbol(Table, "", 0, 0, 0, &Ignored);

    for (Index = 0; Rc == 0 && Index < BOXASM_SECTION_COUNT; Index++)
        if (Plan->Section[Index] != 0)
            Rc = BoxAsmAddElfSymbol(Table, "",
                                    BOXASM_SYMBOL_INFO(BOXASM_STB_LOCAL, BOXASM_STT_SECTION),
                                    Plan->Section[Index], 0,
                                    &Asm->Symbols[BoxAsmSectionSymbol((BoxAsmSection)Index)].ElfIndex);
    for (Pass = 0; Rc == 0 && Pass < 2; Pass++) {
        if (Pass == 1)
            Table->FirstGlobal = (uint32_t)(Table->Symbols.Size / sizeof(BoxAsmElfSymbol));
        for (Index = BOXASM_SECTION_COUNT + 1u; Rc == 0 && Index < Asm->SymbolCount; Index++) {
            BoxAsmSymbol *Symbol = &Asm->Symbols[Index];

            if (Symbol->Kind == BOXASM_SYMBOL_LABEL && Symbol->Global == Pass)
                Rc = BoxAsmAddElfSymbol(Table, Symbol->Name,
                                        BOXASM_SYMBOL_INFO(Pass ? BOXASM_STB_GLOBAL : BOXASM_STB_LOCAL,
                                                           BOXASM_STT_NOTYPE),
                                        Plan->Section[Symbol->Section], Symbol->Offset,
                                        &Symbol->ElfIndex);
            else if (Pass == 1 && Symbol->Kind == BOXASM_SYMBOL_EXTERN && Symbol->Referenced)
                Rc = BoxAsmAddElfSymbol(Table, Symbol->Name,
                                        BOXASM_SYMBOL_INFO(BOXASM_STB_GLOBAL, BOXASM_STT_NOTYPE),
                                        BOXASM_SHN_UNDEF, 0, &Symbol->ElfIndex);
        }
    }
    return Rc;
}

static int BoxAsmBuildRela(const BoxAsmAssembler *Asm, size_t Section, BoxAsmBuffer *Out)
{
    const BoxAsmSectionImage *Image = &Asm->Sections[Section];
    size_t Index;

    for (Index = 0; Index < Image->RelocationCount; Index++) {
        const BoxAsmRelocation *Relocation = &Image->Relocations[Index];
        BoxAsmElfRela Entry = {
            Relocation->Offset,
            BOXASM_RELA_INFO(Asm->Symbols[Relocation->Symbol].ElfIndex, Relocation->Type),
            Relocation->Addend
        };
        if (BoxAsmBufferAppend(Out, &Entry, sizeof(Entry)) != 0)
            return -ERR_NO_MEMORY;
    }
    return 0;
}

/* The name's offset goes straight into the packed header, never through a
 * pointer to it. */
static int BoxAsmAppendSectionName(BoxAsmBuffer *Names, const char *Prefix, const char *Name,
                                   BoxAsmElfSectionHeader *Header)
{
    if (Names->Size > UINT32_MAX)
        return -ERR_NO_MEMORY;
    Header->Name = (uint32_t)Names->Size;
    if (BoxAsmBufferAppend(Names, Prefix, strlen(Prefix)) != 0 ||
        BoxAsmBufferAppend(Names, Name, strlen(Name) + 1u) != 0)
        return -ERR_NO_MEMORY;
    return 0;
}

/* A declaration the program carries as text — its Form, its settings, its life — as a
 * section of its own name. */
static int BoxAsmLayText(uint16_t Index, const char *Name, const BoxAsmBuffer *Text,
                         BoxAsmBuffer *Names, BoxAsmBuffer *File, BoxAsmElfSectionHeader *Header)
{
    int Rc;

    if (Index == 0)
        return 0;
    Rc = BoxAsmAppendSectionName(Names, "", Name, Header);
    *Header = (BoxAsmElfSectionHeader){ Header->Name, BOXASM_SHT_PROGBITS, BOXASM_SHF_ALLOC, 0,
                                        File->Size, Text->Size, 0, 0, 1, 0 };
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(File, Text->Data, Text->Size);
    return Rc;
}

/* Contents and headers of every section, laid into File in plan order. */
static int BoxAsmLayContents(BoxAsmAssembler *Asm, const BoxAsmPlan *Plan,
                             const BoxAsmSymbolTable *Table, BoxAsmBuffer *File,
                             BoxAsmElfSectionHeader *Headers)
{
    BoxAsmBuffer Names = { 0 };
    size_t Index;
    int Rc = BoxAsmBufferAppend(&Names, "", 1u);

    for (Index = 0; Rc == 0 && Index < BOXASM_SECTION_COUNT; Index++) {
        BoxAsmElfSectionHeader *Header = &Headers[Plan->Section[Index]];
        const BoxAsmSectionImage *Image = &Asm->Sections[Index];

        if (Plan->Section[Index] == 0)
            continue;
        Rc = BoxAsmAppendSectionName(&Names, "", BoxAsmSectionNames[Index], Header);
        if (Rc == 0)
            Rc = BoxAsmBufferAlign(File, 16u);
        Header->Type = Index == BOXASM_SECTION_BSS ? BOXASM_SHT_NOBITS : BOXASM_SHT_PROGBITS;
        Header->Flags = BoxAsmSectionFlags[Index];
        Header->Offset = File->Size;
        Header->Size = Image->Size;
        Header->AddressAlign = Image->Align;
        if (Rc == 0 && Index != BOXASM_SECTION_BSS)
            Rc = BoxAsmBufferAppend(File, Image->Bytes.Data, Image->Bytes.Size);
    }
    if (Rc == 0)
        Rc = BoxAsmLayText(Plan->Form, ".boxos.form", &Asm->Form.Text, &Names, File,
                           &Headers[Plan->Form]);
    if (Rc == 0)
        Rc = BoxAsmLayText(Plan->Settings, ".boxos.settings", &Asm->Settings, &Names, File,
                           &Headers[Plan->Settings]);
    if (Rc == 0)
        Rc = BoxAsmLayText(Plan->Life, ".boxos.life", &Asm->Life, &Names, File, &Headers[Plan->Life]);
    for (Index = 0; Rc == 0 && Index < BOXASM_SECTION_COUNT; Index++) {
        BoxAsmElfSectionHeader *Header = &Headers[Plan->Rela[Index]];

        if (Plan->Rela[Index] == 0)
            continue;
        Rc = BoxAsmAppendSectionName(&Names, ".rela", BoxAsmSectionNames[Index], Header);
        if (Rc == 0)
            Rc = BoxAsmBufferAlign(File, 8u);
        *Header = (BoxAsmElfSectionHeader){ Header->Name, BOXASM_SHT_RELA, BOXASM_SHF_INFO_LINK,
                                            0, File->Size, 0, Plan->Symtab, Plan->Section[Index],
                                            8, sizeof(BoxAsmElfRela) };
        if (Rc == 0)
            Rc = BoxAsmBuildRela(Asm, Index, File);
        Header->Size = File->Size - Header->Offset;
    }
    if (Rc == 0)
        Rc = BoxAsmAppendSectionName(&Names, "", ".symtab", &Headers[Plan->Symtab]);
    if (Rc == 0)
        Rc = BoxAsmAppendSectionName(&Names, "", ".strtab", &Headers[Plan->Strtab]);
    if (Rc == 0)
        Rc = BoxAsmAppendSectionName(&Names, "", ".shstrtab", &Headers[Plan->Shstrtab]);
    if (Rc == 0)
        Rc = BoxAsmBufferAlign(File, 8u);
    Headers[Plan->Symtab] = (BoxAsmElfSectionHeader){
        Headers[Plan->Symtab].Name, BOXASM_SHT_SYMTAB, 0, 0, File->Size, Table->Symbols.Size,
        Plan->Strtab, Table->FirstGlobal, 8, sizeof(BoxAsmElfSymbol)
    };
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(File, Table->Symbols.Data, Table->Symbols.Size);
    Headers[Plan->Strtab] = (BoxAsmElfSectionHeader){
        Headers[Plan->Strtab].Name, BOXASM_SHT_STRTAB, 0, 0, File->Size, Table->Names.Size,
        0, 0, 1, 0
    };
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(File, Table->Names.Data, Table->Names.Size);
    Headers[Plan->Shstrtab] = (BoxAsmElfSectionHeader){
        Headers[Plan->Shstrtab].Name, BOXASM_SHT_STRTAB, 0, 0, File->Size, Names.Size, 0, 0, 1, 0
    };
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(File, Names.Data, Names.Size);
    BoxAsmBufferRelease(&Names);
    return Rc;
}

int BoxAsmWriteObject(BoxAsmAssembler *Asm, BoxAsmBlob *Out)
{
    BoxAsmPlan Plan;
    BoxAsmSymbolTable Table = { { 0 }, { 0 }, 0 };
    BoxAsmBuffer File = { 0 };
    BoxAsmElfSectionHeader *Headers = NULL;
    BoxAsmElfHeader Header;
    int Rc;

    BoxAsmPlanHeaders(Asm, &Plan);
    Headers = (BoxAsmElfSectionHeader *)malloc(Plan.Count * sizeof(BoxAsmElfSectionHeader));
    Rc = Headers ? BoxAsmBuildSymbols(Asm, &Plan, &Table) : -ERR_NO_MEMORY;
    if (Rc == 0) {
        memset(Headers, 0, Plan.Count * sizeof(BoxAsmElfSectionHeader));
        Rc = BoxAsmBufferAppendZeroes(&File, sizeof(Header));
    }
    if (Rc == 0)
        Rc = BoxAsmLayContents(Asm, &Plan, &Table, &File, Headers);
    if (Rc == 0)
        Rc = BoxAsmBufferAlign(&File, 8u);
    memset(&Header, 0, sizeof(Header));
    Header.Ident[0] = BOXASM_ELF_MAGIC0;
    Header.Ident[1] = BOXASM_ELF_MAGIC1;
    Header.Ident[2] = BOXASM_ELF_MAGIC2;
    Header.Ident[3] = BOXASM_ELF_MAGIC3;
    Header.Ident[4] = BOXASM_ELF_CLASS64;
    Header.Ident[5] = BOXASM_ELF_LSB;
    Header.Ident[6] = BOXASM_ELF_CURRENT;
    Header.Type = BOXASM_ET_REL;
    Header.Machine = BOXASM_EM_X86_64;
    Header.Version = BOXASM_ELF_CURRENT;
    Header.SectionHeaderOffset = File.Size;
    Header.HeaderSize = sizeof(Header);
    Header.SectionHeaderSize = sizeof(BoxAsmElfSectionHeader);
    Header.SectionHeaderCount = Plan.Count;
    Header.SectionNameIndex = Plan.Shstrtab;
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(&File, Headers, Plan.Count * sizeof(BoxAsmElfSectionHeader));
    if (Rc == 0) {
        memcpy(File.Data, &Header, sizeof(Header));
        Out->Data = File.Data;
        Out->Size = File.Size;
        File.Data = NULL;
    }
    free(Headers);
    BoxAsmBufferRelease(&Table.Symbols);
    BoxAsmBufferRelease(&Table.Names);
    BoxAsmBufferRelease(&File);
    return Rc;
}
