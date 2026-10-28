#include "boxasm_link_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define BOXASM_OUTPUT_USER_BASE 0xC000ull
#define BOXASM_OUTPUT_PAGE 0x1000ull

static uint64_t BoxAsmOutputAlign(uint64_t Value, uint64_t Alignment)
{
    uint64_t Mask;

    if (Alignment <= 1u)
        return Value;
    if ((Alignment & (Alignment - 1u)) != 0 ||
        Value > UINT64_MAX - (Alignment - 1u))
        return UINT64_MAX;
    Mask = Alignment - 1u;
    return (Value + Mask) & ~Mask;
}

static int BoxAsmOutputBuildSectionNames(const BoxAsmLinkImage *Image,
                                          BoxAsmBuffer *Names,
                                          uint32_t Offsets[12],
                                          uint32_t *ShstrtabName)
{
    size_t Index;

    if (BoxAsmBufferAppend(Names, "", 1u) != 0)
        return -ERR_NO_MEMORY;
    for (Index = 1; Index < 12; Index++) {
        if (Names->Size > UINT32_MAX)
            return -ERR_INVALID_ARGUMENT;
        Offsets[Index] = (uint32_t)Names->Size;
        if (BoxAsmBufferAppend(Names, Image->Sections[Index].Name,
                               strlen(Image->Sections[Index].Name) + 1u) != 0)
            return -ERR_NO_MEMORY;
    }
    if (Names->Size > UINT32_MAX)
        return -ERR_INVALID_ARGUMENT;
    *ShstrtabName = (uint32_t)Names->Size;
    if (BoxAsmBufferAppend(Names, ".shstrtab", 10u) != 0)
        return -ERR_NO_MEMORY;
    return 0;
}

static int BoxAsmOutputAppendReadOnly(BoxAsmLinkImage *Image,
                                      BoxAsmBuffer *File,
                                      uint64_t *FirstFileEnd)
{
    size_t Index;

    for (Index = 1; Index <= 5; Index++) {
        BoxAsmOutputSection *Section = &Image->Sections[Index];
        uint64_t Wanted;

        if (!Section->Present)
            continue;
        if (Section->Address < BOXASM_OUTPUT_USER_BASE)
            return -ERR_INVALID_ARGUMENT;
        Wanted = BOXASM_OUTPUT_PAGE + Section->Address - BOXASM_OUTPUT_USER_BASE;
        if (Wanted < File->Size)
            return -ERR_INVALID_ARGUMENT;
        if (BoxAsmBufferAppendZeroes(File, Wanted - File->Size) != 0)
            return -ERR_NO_MEMORY;
        Section->FileOffset = Wanted;
        if (Section->Data.Size != 0 &&
            BoxAsmBufferAppend(File, Section->Data.Data, Section->Data.Size) != 0)
            return -ERR_NO_MEMORY;
        *FirstFileEnd = File->Size;
    }
    return 0;
}

static int BoxAsmOutputAppendWritable(BoxAsmLinkImage *Image,
                                      BoxAsmBuffer *File,
                                      uint64_t *DataFileStart,
                                      uint64_t *DataFileEnd)
{
    size_t Index;

    *DataFileStart = BoxAsmOutputAlign(File->Size, BOXASM_OUTPUT_PAGE);
    if (*DataFileStart == UINT64_MAX ||
        BoxAsmBufferAppendZeroes(File, *DataFileStart - File->Size) != 0)
        return -ERR_NO_MEMORY;
    for (Index = 6; Index <= 9; Index++) {
        BoxAsmOutputSection *Section = &Image->Sections[Index];
        uint64_t Wanted;

        if (!Section->Present)
            continue;
        if (Section->Address < Image->Sections[6].Address ||
            *DataFileStart > UINT64_MAX -
                (Section->Address - Image->Sections[6].Address))
            return -ERR_INVALID_ARGUMENT;
        Wanted = *DataFileStart + Section->Address - Image->Sections[6].Address;
        if (Wanted < File->Size)
            return -ERR_INVALID_ARGUMENT;
        if (BoxAsmBufferAppendZeroes(File, Wanted - File->Size) != 0)
            return -ERR_NO_MEMORY;
        Section->FileOffset = Wanted;
        if (Section->Data.Size != 0 &&
            BoxAsmBufferAppend(File, Section->Data.Data, Section->Data.Size) != 0)
            return -ERR_NO_MEMORY;
        *DataFileEnd = File->Size;
    }
    if (*DataFileEnd == 0)
        *DataFileEnd = *DataFileStart;
    if (Image->TlsMemorySize != 0 &&
        BoxAsmBufferAlign(File, Image->TlsAlign) != 0)
        return -ERR_NO_MEMORY;
    for (Index = 10; Index <= 11; Index++)
        if (Image->Sections[Index].Present)
            Image->Sections[Index].FileOffset = File->Size;

    Image->TlsFileOffset = Image->Sections[9].Present
        ? Image->Sections[9].FileOffset
        : (Image->Sections[10].Present ? Image->Sections[10].FileOffset : 0);
    Image->TlsAddress = Image->Sections[9].Present
        ? Image->Sections[9].Address
        : (Image->Sections[10].Present ? Image->Sections[10].Address : 0);
    Image->TlsFileSize = Image->Sections[9].Size;
    Image->TlsMemorySize = Image->Sections[10].Present
        ? Image->Sections[10].Address + Image->Sections[10].Size - Image->TlsAddress
        : Image->Sections[9].Size;
    Image->TlsAlign = Image->Sections[9].Align > Image->Sections[10].Align
        ? Image->Sections[9].Align : Image->Sections[10].Align;
    return 0;
}

static void BoxAsmOutputBuildSectionHeaders(const BoxAsmLinkImage *Image,
                                            BoxAsmElfSectionHeader Sections[13],
                                            const uint32_t Offsets[12],
                                            uint32_t ShstrtabName,
                                            uint64_t SectionNamesOffset,
                                            uint64_t SectionNamesSize)
{
    size_t Index;

    memset(Sections, 0, sizeof(BoxAsmElfSectionHeader) * 13u);
    for (Index = 1; Index < 12; Index++) {
        const BoxAsmOutputSection *Section = &Image->Sections[Index];
        Sections[Index].Name = Offsets[Index];
        Sections[Index].Type = Section->Type;
        Sections[Index].Flags = Section->Flags;
        Sections[Index].Address = Section->Address;
        Sections[Index].Offset = Section->FileOffset;
        Sections[Index].Size = Section->Size;
        Sections[Index].AddressAlign = Section->Align;
    }
    Sections[12] = (BoxAsmElfSectionHeader){ ShstrtabName, BOXASM_SHT_STRTAB, 0, 0,
                                             SectionNamesOffset, SectionNamesSize,
                                             0, 0, 1, 0 };
}

static void BoxAsmOutputBuildProgramHeaders(const BoxAsmLinkImage *Image,
                                            BoxAsmElfProgramHeader Programs[3],
                                            uint64_t FirstFileEnd,
                                            uint64_t DataFileStart,
                                            uint64_t DataFileEnd,
                                            uint16_t ProgramCount)
{
    memset(Programs, 0, sizeof(BoxAsmElfProgramHeader) * 3u);
    Programs[0] = (BoxAsmElfProgramHeader){ BOXASM_PT_LOAD, BOXASM_PF_R | BOXASM_PF_X,
                                            BOXASM_OUTPUT_PAGE, BOXASM_OUTPUT_USER_BASE,
                                            BOXASM_OUTPUT_USER_BASE,
                                            FirstFileEnd - BOXASM_OUTPUT_PAGE,
                                            FirstFileEnd - BOXASM_OUTPUT_PAGE,
                                            BOXASM_OUTPUT_PAGE };
    Programs[1] = (BoxAsmElfProgramHeader){ BOXASM_PT_LOAD, BOXASM_PF_R | BOXASM_PF_W,
                                            DataFileStart, Image->Sections[6].Address,
                                            Image->Sections[6].Address,
                                            DataFileEnd - DataFileStart,
                                            Image->ProgramEnd - Image->Sections[6].Address,
                                            BOXASM_OUTPUT_PAGE };
    if (ProgramCount == 3u)
        Programs[2] = (BoxAsmElfProgramHeader){ BOXASM_PT_TLS, BOXASM_PF_R,
                                                Image->TlsFileOffset, Image->TlsAddress,
                                                Image->TlsAddress, Image->TlsFileSize,
                                                Image->TlsMemorySize, Image->TlsAlign };
}

static void BoxAsmOutputBuildElfHeader(BoxAsmElfHeader *Header,
                                       const BoxAsmLinkImage *Image,
                                       uint64_t SectionHeaderOffset,
                                       uint16_t ProgramCount)
{
    memset(Header, 0, sizeof(*Header));
    Header->Ident[0] = BOXASM_ELF_MAGIC0;
    Header->Ident[1] = BOXASM_ELF_MAGIC1;
    Header->Ident[2] = BOXASM_ELF_MAGIC2;
    Header->Ident[3] = BOXASM_ELF_MAGIC3;
    Header->Ident[4] = BOXASM_ELF_CLASS64;
    Header->Ident[5] = BOXASM_ELF_LSB;
    Header->Ident[6] = BOXASM_ELF_CURRENT;
    Header->Type = BOXASM_ET_EXEC;
    Header->Machine = BOXASM_EM_X86_64;
    Header->Version = BOXASM_ELF_CURRENT;
    Header->Entry = Image->Entry;
    Header->ProgramHeaderOffset = sizeof(*Header);
    Header->SectionHeaderOffset = SectionHeaderOffset;
    Header->HeaderSize = sizeof(*Header);
    Header->ProgramHeaderSize = sizeof(BoxAsmElfProgramHeader);
    Header->ProgramHeaderCount = ProgramCount;
    Header->SectionHeaderSize = sizeof(BoxAsmElfSectionHeader);
    Header->SectionHeaderCount = 13;
    Header->SectionNameIndex = 12;
}

int BoxAsmWriteOutput(BoxAsmLinkImage *Image, BoxAsmBlob *Out,
                      char *Error, size_t ErrorCapacity)
{
    BoxAsmBuffer File = { 0 };
    BoxAsmBuffer SectionNames = { 0 };
    BoxAsmElfHeader Header;
    BoxAsmElfProgramHeader Programs[3];
    BoxAsmElfSectionHeader Sections[13];
    uint32_t SectionNameOffsets[12];
    uint32_t ShstrtabName;
    uint64_t FirstFileEnd = 0;
    uint64_t DataFileStart = 0;
    uint64_t DataFileEnd = 0;
    uint64_t SectionHeaderOffset;
    size_t HeaderBytes;
    uint16_t ProgramCount;
    int Rc;

    (void)Error;
    (void)ErrorCapacity;
    ProgramCount = Image->TlsMemorySize != 0 ? 3u : 2u;
    Rc = BoxAsmOutputBuildSectionNames(Image, &SectionNames,
                                       SectionNameOffsets, &ShstrtabName);
    if (Rc != 0)
        goto Fail;
    HeaderBytes = sizeof(BoxAsmElfHeader) +
                  (size_t)ProgramCount * sizeof(BoxAsmElfProgramHeader);
    if (BoxAsmBufferAppendZeroes(&File, HeaderBytes) != 0 ||
        BoxAsmBufferAlign(&File, BOXASM_OUTPUT_PAGE) != 0) {
        Rc = -ERR_NO_MEMORY;
        goto Fail;
    }
    Rc = BoxAsmOutputAppendReadOnly(Image, &File, &FirstFileEnd);
    if (Rc != 0)
        goto Fail;
    Rc = BoxAsmOutputAppendWritable(Image, &File, &DataFileStart, &DataFileEnd);
    if (Rc != 0)
        goto Fail;
    if (BoxAsmBufferAlign(&File, 8u) != 0 ||
        BoxAsmBufferAppend(&File, SectionNames.Data, SectionNames.Size) != 0) {
        Rc = -ERR_NO_MEMORY;
        goto Fail;
    }
    SectionHeaderOffset = BoxAsmOutputAlign(File.Size, 8u);
    if (SectionHeaderOffset == UINT64_MAX ||
        BoxAsmBufferAppendZeroes(&File, SectionHeaderOffset - File.Size) != 0) {
        Rc = -ERR_NO_MEMORY;
        goto Fail;
    }
    BoxAsmOutputBuildSectionHeaders(Image, Sections, SectionNameOffsets,
                                    ShstrtabName,
                                    SectionHeaderOffset - SectionNames.Size,
                                    SectionNames.Size);
    BoxAsmOutputBuildProgramHeaders(Image, Programs, FirstFileEnd,
                                    DataFileStart, DataFileEnd, ProgramCount);
    BoxAsmOutputBuildElfHeader(&Header, Image, SectionHeaderOffset, ProgramCount);
    memcpy(File.Data, &Header, sizeof(Header));
    memcpy(File.Data + Header.ProgramHeaderOffset, Programs,
           ProgramCount * sizeof(BoxAsmElfProgramHeader));
    if (BoxAsmBufferAppend(&File, Sections, sizeof(Sections)) != 0) {
        Rc = -ERR_NO_MEMORY;
        goto Fail;
    }
    Out->Data = File.Data;
    Out->Size = File.Size;
    File.Data = NULL;
    BoxAsmBufferRelease(&SectionNames);
    return 0;

Fail:
    BoxAsmBufferRelease(&SectionNames);
    BoxAsmBufferRelease(&File);
    return Rc;
}
