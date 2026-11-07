#include "boxasm_elf.h"

#include "box/error.h"
#include "box/string.h"

static int BoxAsmSectionTableValid(const BoxAsmBlob *Blob,
                                   const BoxAsmElfHeader *Header)
{
    uint64_t Bytes;

    if (Header->SectionHeaderCount == 0 ||
        Header->SectionHeaderSize != sizeof(BoxAsmElfSectionHeader))
        return 0;
    Bytes = (uint64_t)Header->SectionHeaderCount * sizeof(BoxAsmElfSectionHeader);
    return BoxAsmRangeValid(Blob->Size, Header->SectionHeaderOffset, Bytes);
}

static int BoxAsmStringTableValid(const BoxAsmBlob *Blob,
                                  const BoxAsmElfSectionHeader *Section)
{
    const uint8_t *Data;

    if (Section->Type != BOXASM_SHT_STRTAB || Section->Size == 0 ||
        !BoxAsmRangeValid(Blob->Size, Section->Offset, Section->Size))
        return 0;
    Data = Blob->Data + Section->Offset;
    return Data[Section->Size - 1u] == '\0';
}

/* A symbol names a section the object has, or one of the three reserved
 * meanings this toolchain understands. Anything else (SHN_XINDEX, processor
 * or OS ranges) is refused here, so no later stage indexes past the table.
 * The locals come first and sh_info is the first global, as ELF requires and
 * GNU ld enforces. */
static int BoxAsmSymbolsValid(const BoxAsmElfObject *Object, uint32_t FirstGlobal)
{
    uint32_t Index;

    for (Index = 0; Index < Object->SymbolCount; Index++) {
        const BoxAsmElfSymbol *Symbol = &Object->Symbols[Index];
        uint16_t Section = Symbol->SectionIndex;
        int Local = BOXASM_SYMBOL_BIND(Symbol->Info) == BOXASM_STB_LOCAL;

        if ((uint64_t)Symbol->Name >= Object->StringBytes ||
            Local != (Index < FirstGlobal))
            return 0;
        if (Section == BOXASM_SHN_UNDEF || Section == BOXASM_SHN_ABS ||
            Section == BOXASM_SHN_COMMON)
            continue;
        if (Section >= Object->Header->SectionHeaderCount)
            return 0;
    }
    return 1;
}

int BoxAsmElfOpen(const BoxAsmBlob *Blob, uint16_t ExpectedType,
                  BoxAsmElfObject *Out, char *Error, size_t ErrorCapacity)
{
    const BoxAsmElfHeader *Header;
    const BoxAsmElfSectionHeader *Sections;
    const BoxAsmElfSectionHeader *SectionNames;
    const BoxAsmElfSectionHeader *Symtab = NULL;
    const BoxAsmElfSectionHeader *Strings;
    uint16_t Index;

    if (!Blob || !Out || !Blob->Data || Blob->Size < sizeof(BoxAsmElfHeader)) {
        BoxAsmMessageSet(Error, ErrorCapacity, "ELF header is missing");
        return -ERR_INVALID_ARGUMENT;
    }
    memset(Out, 0, sizeof(*Out));
    Header = (const BoxAsmElfHeader *)Blob->Data;
    if (Header->Ident[0] != BOXASM_ELF_MAGIC0 ||
        Header->Ident[1] != BOXASM_ELF_MAGIC1 ||
        Header->Ident[2] != BOXASM_ELF_MAGIC2 ||
        Header->Ident[3] != BOXASM_ELF_MAGIC3 ||
        Header->Ident[4] != BOXASM_ELF_CLASS64 ||
        Header->Ident[5] != BOXASM_ELF_LSB ||
        Header->Ident[6] != BOXASM_ELF_CURRENT) {
        BoxAsmMessageSet(Error, ErrorCapacity, "unsupported ELF identity");
        return -ERR_INVALID_ARGUMENT;
    }
    if (Header->Type != ExpectedType || Header->Machine != BOXASM_EM_X86_64 ||
        Header->Version != BOXASM_ELF_CURRENT) {
        BoxAsmMessageSet(Error, ErrorCapacity, "unsupported ELF type or machine");
        return -ERR_INVALID_ARGUMENT;
    }
    if (!BoxAsmSectionTableValid(Blob, Header) ||
        Header->SectionNameIndex >= Header->SectionHeaderCount) {
        BoxAsmMessageSet(Error, ErrorCapacity, "invalid ELF section table");
        return -ERR_INVALID_ARGUMENT;
    }

    Sections = (const BoxAsmElfSectionHeader *)(Blob->Data +
                                                Header->SectionHeaderOffset);
    SectionNames = &Sections[Header->SectionNameIndex];
    if (!BoxAsmStringTableValid(Blob, SectionNames)) {
        BoxAsmMessageSet(Error, ErrorCapacity, "invalid ELF section names");
        return -ERR_INVALID_ARGUMENT;
    }

    for (Index = 0; Index < Header->SectionHeaderCount; Index++) {
        const BoxAsmElfSectionHeader *Section = &Sections[Index];
        if (Section->Type != BOXASM_SHT_NOBITS &&
            !BoxAsmRangeValid(Blob->Size, Section->Offset, Section->Size)) {
            BoxAsmMessageSet(Error, ErrorCapacity, "ELF section exceeds image");
            return -ERR_INVALID_ARGUMENT;
        }
        if (Section->Name >= SectionNames->Size) {
            BoxAsmMessageSet(Error, ErrorCapacity, "ELF section name is invalid");
            return -ERR_INVALID_ARGUMENT;
        }
        if (Section->Type == BOXASM_SHT_SYMTAB) {
            if (Symtab) {
                BoxAsmMessageSet(Error, ErrorCapacity, "multiple ELF symbol tables");
                return -ERR_INVALID_ARGUMENT;
            }
            Symtab = Section;
        }
    }
    if (!Symtab || Symtab->EntrySize != sizeof(BoxAsmElfSymbol) ||
        Symtab->Link >= Header->SectionHeaderCount ||
        Symtab->Size % sizeof(BoxAsmElfSymbol) != 0 ||
        Symtab->Size / sizeof(BoxAsmElfSymbol) > UINT32_MAX) {
        BoxAsmMessageSet(Error, ErrorCapacity, "invalid ELF symbol table");
        return -ERR_INVALID_ARGUMENT;
    }
    Strings = &Sections[Symtab->Link];
    if (!BoxAsmStringTableValid(Blob, Strings)) {
        BoxAsmMessageSet(Error, ErrorCapacity, "invalid ELF symbol strings");
        return -ERR_INVALID_ARGUMENT;
    }
    if (Symtab->Info > Symtab->Size / sizeof(BoxAsmElfSymbol)) {
        BoxAsmMessageSet(Error, ErrorCapacity, "invalid ELF local symbol boundary");
        return -ERR_INVALID_ARGUMENT;
    }

    Out->Blob = *Blob;
    Out->Header = Header;
    Out->Sections = Sections;
    Out->SectionNames = (const char *)(Blob->Data + SectionNames->Offset);
    Out->SectionNameBytes = SectionNames->Size;
    Out->Symbols = (const BoxAsmElfSymbol *)(Blob->Data + Symtab->Offset);
    Out->SymbolCount = (uint32_t)(Symtab->Size / sizeof(BoxAsmElfSymbol));
    Out->Strings = (const char *)(Blob->Data + Strings->Offset);
    Out->StringBytes = Strings->Size;
    if (!BoxAsmSymbolsValid(Out, Symtab->Info)) {
        memset(Out, 0, sizeof(*Out));
        BoxAsmMessageSet(Error, ErrorCapacity,
                         "ELF symbol table is malformed: a symbol names a section or "
                         "string the object does not have, or a local follows sh_info");
        return -ERR_INVALID_ARGUMENT;
    }
    return 0;
}

const char *BoxAsmElfSectionName(const BoxAsmElfObject *Object, uint16_t Index)
{
    uint32_t Offset;

    if (!Object || !Object->Header || Index >= Object->Header->SectionHeaderCount)
        return NULL;
    Offset = Object->Sections[Index].Name;
    if ((uint64_t)Offset >= Object->SectionNameBytes)
        return NULL;
    return Object->SectionNames + Offset;
}

const uint8_t *BoxAsmElfSectionData(const BoxAsmElfObject *Object,
                                    uint16_t Index)
{
    const BoxAsmElfSectionHeader *Section;

    if (!Object || !Object->Header || Index >= Object->Header->SectionHeaderCount)
        return NULL;
    Section = &Object->Sections[Index];
    if (Section->Type == BOXASM_SHT_NOBITS)
        return NULL;
    if (!BoxAsmRangeValid(Object->Blob.Size, Section->Offset, Section->Size))
        return NULL;
    return Object->Blob.Data + Section->Offset;
}

const char *BoxAsmElfSymbolName(const BoxAsmElfObject *Object, uint32_t Index)
{
    uint32_t Offset;

    if (!Object || Index >= Object->SymbolCount)
        return NULL;
    Offset = Object->Symbols[Index].Name;
    if ((uint64_t)Offset >= Object->StringBytes)
        return NULL;
    return Object->Strings + Offset;
}
