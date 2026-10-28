#include "boxasm_elf.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

static void BoxAsmSetError(char *Error, size_t Capacity, const char *Text)
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

static uint64_t BoxAsmAlignUp(uint64_t Value, uint64_t Alignment)
{
    uint64_t Mask;

    if (Alignment <= 1u)
        return Value;
    if ((Alignment & (Alignment - 1u)) != 0)
        return UINT64_MAX;
    Mask = Alignment - 1u;
    if (Value > UINT64_MAX - Mask)
        return UINT64_MAX;
    return (Value + Mask) & ~Mask;
}

int BoxAsmBufferReserve(BoxAsmBuffer *Buffer, uint64_t Additional)
{
    uint64_t Needed;
    uint64_t Capacity;
    uint8_t *Fresh;

    if (!Buffer || Additional > UINT64_MAX - Buffer->Size)
        return -ERR_INVALID_ARGUMENT;
    Needed = Buffer->Size + Additional;
    if (Needed <= Buffer->Capacity)
        return 0;

    Capacity = Buffer->Capacity ? Buffer->Capacity : 256u;
    while (Capacity < Needed) {
        if (Capacity > UINT64_MAX / 2u)
            Capacity = Needed;
        else
            Capacity *= 2u;
        if (Capacity < Needed && Capacity == UINT64_MAX)
            return -ERR_NO_MEMORY;
    }

    Fresh = (uint8_t *)realloc(Buffer->Data, (size_t)Capacity);
    if (!Fresh)
        return -ERR_NO_MEMORY;
    Buffer->Data = Fresh;
    Buffer->Capacity = Capacity;
    return 0;
}

int BoxAsmBufferAppend(BoxAsmBuffer *Buffer, const void *Data, uint64_t Size)
{
    int Rc;

    if (!Buffer || (Size != 0 && !Data))
        return -ERR_INVALID_ARGUMENT;
    Rc = BoxAsmBufferReserve(Buffer, Size);
    if (Rc != 0)
        return Rc;
    if (Size != 0)
        memcpy(Buffer->Data + Buffer->Size, Data, (size_t)Size);
    Buffer->Size += Size;
    return 0;
}

int BoxAsmBufferAppendZeroes(BoxAsmBuffer *Buffer, uint64_t Size)
{
    int Rc;

    if (!Buffer)
        return -ERR_INVALID_ARGUMENT;
    Rc = BoxAsmBufferReserve(Buffer, Size);
    if (Rc != 0)
        return Rc;
    if (Size != 0)
        memset(Buffer->Data + Buffer->Size, 0, (size_t)Size);
    Buffer->Size += Size;
    return 0;
}

int BoxAsmBufferAlign(BoxAsmBuffer *Buffer, uint64_t Alignment)
{
    uint64_t Aligned;

    if (!Buffer || Alignment == 0 || (Alignment & (Alignment - 1u)) != 0)
        return -ERR_INVALID_ARGUMENT;
    Aligned = BoxAsmAlignUp(Buffer->Size, Alignment);
    if (Aligned == UINT64_MAX)
        return -ERR_NO_MEMORY;
    return BoxAsmBufferAppendZeroes(Buffer, Aligned - Buffer->Size);
}

void BoxAsmBufferRelease(BoxAsmBuffer *Buffer)
{
    if (!Buffer)
        return;
    free(Buffer->Data);
    memset(Buffer, 0, sizeof(*Buffer));
}

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

int BoxAsmElfOpen(const BoxAsmBlob *Blob, uint16_t ExpectedType,
                  BoxAsmElfObject *Out, char *Error, size_t ErrorCapacity)
{
    const BoxAsmElfHeader *Header;
    const BoxAsmElfSectionHeader *Sections;
    const BoxAsmElfSectionHeader *SectionNames;
    const BoxAsmElfSectionHeader *Symtab = NULL;
    const BoxAsmElfSectionHeader *Strings = NULL;
    uint16_t Index;

    if (!Blob || !Out || !Blob->Data || Blob->Size < sizeof(BoxAsmElfHeader)) {
        BoxAsmSetError(Error, ErrorCapacity, "ELF header is missing");
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
        BoxAsmSetError(Error, ErrorCapacity, "unsupported ELF identity");
        return -ERR_INVALID_ARGUMENT;
    }
    if (Header->Type != ExpectedType || Header->Machine != BOXASM_EM_X86_64 ||
        Header->Version != BOXASM_ELF_CURRENT) {
        BoxAsmSetError(Error, ErrorCapacity, "unsupported ELF type or machine");
        return -ERR_INVALID_ARGUMENT;
    }
    if (!BoxAsmSectionTableValid(Blob, Header) ||
        Header->SectionNameIndex >= Header->SectionHeaderCount) {
        BoxAsmSetError(Error, ErrorCapacity, "invalid ELF section table");
        return -ERR_INVALID_ARGUMENT;
    }

    Sections = (const BoxAsmElfSectionHeader *)(Blob->Data +
                                                Header->SectionHeaderOffset);
    SectionNames = &Sections[Header->SectionNameIndex];
    if (!BoxAsmStringTableValid(Blob, SectionNames)) {
        BoxAsmSetError(Error, ErrorCapacity, "invalid ELF section names");
        return -ERR_INVALID_ARGUMENT;
    }

    for (Index = 0; Index < Header->SectionHeaderCount; Index++) {
        const BoxAsmElfSectionHeader *Section = &Sections[Index];
        if (Section->Type != BOXASM_SHT_NOBITS &&
            !BoxAsmRangeValid(Blob->Size, Section->Offset, Section->Size)) {
            BoxAsmSetError(Error, ErrorCapacity, "ELF section exceeds image");
            return -ERR_INVALID_ARGUMENT;
        }
        if (Section->Name >= SectionNames->Size) {
            BoxAsmSetError(Error, ErrorCapacity, "ELF section name is invalid");
            return -ERR_INVALID_ARGUMENT;
        }
        if (Section->Type == BOXASM_SHT_SYMTAB) {
            if (Symtab) {
                BoxAsmSetError(Error, ErrorCapacity, "multiple ELF symbol tables");
                return -ERR_INVALID_ARGUMENT;
            }
            Symtab = Section;
        }
    }
    if (!Symtab || Symtab->EntrySize != sizeof(BoxAsmElfSymbol) ||
        Symtab->Link >= Header->SectionHeaderCount ||
        Symtab->Size % sizeof(BoxAsmElfSymbol) != 0) {
        BoxAsmSetError(Error, ErrorCapacity, "invalid ELF symbol table");
        return -ERR_INVALID_ARGUMENT;
    }
    Strings = &Sections[Symtab->Link];
    if (!BoxAsmStringTableValid(Blob, Strings)) {
        BoxAsmSetError(Error, ErrorCapacity, "invalid ELF symbol strings");
        return -ERR_INVALID_ARGUMENT;
    }
    if (Symtab->Info > Symtab->Size / sizeof(BoxAsmElfSymbol)) {
        BoxAsmSetError(Error, ErrorCapacity, "invalid ELF local symbol boundary");
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

int BoxAsmElfSectionClass(const BoxAsmElfObject *Object, uint16_t Index)
{
    const char *Name;
    uint32_t Type;

    if (!Object || Index >= Object->Header->SectionHeaderCount)
        return -1;
    Type = Object->Sections[Index].Type;
    if (Type == BOXASM_SHT_NULL || Type == BOXASM_SHT_RELA ||
        Type == BOXASM_SHT_SYMTAB || Type == BOXASM_SHT_STRTAB)
        return 0;
    Name = BoxAsmElfSectionName(Object, Index);
    if (!Name)
        return -1;
    if (strcmp(Name, ".text") == 0 || strncmp(Name, ".text.", 6) == 0)
        return 1;
    if (strcmp(Name, ".eh_frame") == 0 || strncmp(Name, ".eh_frame.", 10) == 0)
        return 3;
    if (strcmp(Name, ".gcc_except_table") == 0 ||
        strncmp(Name, ".gcc_except_table.", 18) == 0)
        return 4;
    if (strncmp(Name, ".rodata", 7) == 0)
        return 2;
    if (strcmp(Name, ".data") == 0 || strncmp(Name, ".data.", 6) == 0)
        return 6;
    if (strcmp(Name, ".init_array") == 0 || strncmp(Name, ".init_array.", 12) == 0)
        return 7;
    if (strcmp(Name, ".fini_array") == 0 || strncmp(Name, ".fini_array.", 12) == 0)
        return 8;
    if (strcmp(Name, ".tdata") == 0 || strncmp(Name, ".tdata.", 7) == 0)
        return 9;
    if (strcmp(Name, ".tbss") == 0 || strcmp(Name, ".tcommon") == 0)
        return 10;
    if (strcmp(Name, ".bss") == 0 || strncmp(Name, ".bss.", 5) == 0)
        return 11;
    if (strcmp(Name, ".comment") == 0 || strncmp(Name, ".note", 5) == 0 ||
        strncmp(Name, ".debug", 6) == 0 || strcmp(Name, ".shstrtab") == 0)
        return 0;
    return -1;
}
