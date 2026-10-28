#include "boxasm_link_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* Header indices after the output sections. */
typedef struct {
    uint16_t Symtab;
    uint16_t Strtab;
    uint16_t Shstrtab;
    uint16_t Count;
} BoxAsmTrailer;

typedef struct {
    uint64_t ReadOnlyEnd;    /* file offset where the R+X segment's bytes end */
    uint64_t DataStart;      /* file offset of the RW segment */
    uint64_t DataEnd;        /* file offset where the RW segment's bytes end */
    uint64_t SymtabOffset;
    uint64_t StrtabOffset;
    uint64_t ShstrtabOffset;
    uint64_t HeadersOffset;
    uint32_t Names[BOXASM_SLOT_COUNT + 3];
} BoxAsmFileLayout;

static int BoxAsmPadTo(BoxAsmBuffer *File, uint64_t Offset)
{
    if (Offset < File->Size)
        return -ERR_INVALID_STATE;
    return BoxAsmBufferAppendZeroes(File, Offset - File->Size);
}

static int BoxAsmAppendName(BoxAsmBuffer *Names, const char *Name, uint32_t *Out)
{
    if (Names->Size > UINT32_MAX)
        return -ERR_INVALID_ARGUMENT;
    *Out = (uint32_t)Names->Size;
    return BoxAsmBufferAppend(Names, Name, strlen(Name) + 1u);
}

static int BoxAsmBuildSectionNames(const BoxAsmLinkImage *Image,
                                   const BoxAsmTrailer *Trailer,
                                   BoxAsmBuffer *Names, BoxAsmFileLayout *Layout)
{
    size_t Slot;
    int Rc = BoxAsmBufferAppend(Names, "", 1u);

    for (Slot = 1; Rc == 0 && Slot < BOXASM_SLOT_COUNT; Slot++)
        Rc = BoxAsmAppendName(Names, Image->Sections[Slot].Name, &Layout->Names[Slot]);
    if (Rc == 0 && Trailer->Symtab != 0)
        Rc = BoxAsmAppendName(Names, ".symtab", &Layout->Names[Trailer->Symtab]);
    if (Rc == 0 && Trailer->Strtab != 0)
        Rc = BoxAsmAppendName(Names, ".strtab", &Layout->Names[Trailer->Strtab]);
    if (Rc == 0)
        Rc = BoxAsmAppendName(Names, ".shstrtab", &Layout->Names[Trailer->Shstrtab]);
    return Rc;
}

/* The R+X segment: the image's first page of file holds the headers, and from
 * the second on, file offset and address advance together from USER_BASE. */
static int BoxAsmWriteReadOnly(BoxAsmLinkImage *Image, BoxAsmBuffer *File,
                               BoxAsmFileLayout *Layout)
{
    size_t Slot;

    for (Slot = BOXASM_SLOT_TEXT; Slot <= BOXASM_SLOT_NAMEPLATE; Slot++) {
        BoxAsmOutputSection *Section = &Image->Sections[Slot];
        int Rc;

        Section->FileOffset = BOXASM_PAGE + Section->Address - BOXASM_USER_BASE;
        if (Section->Size == 0)
            continue;
        Rc = BoxAsmPadTo(File, Section->FileOffset);
        if (Rc == 0)
            Rc = BoxAsmBufferAppend(File, Section->Data.Data, Section->Data.Size);
        if (Rc != 0)
            return Rc;
        Layout->ReadOnlyEnd = File->Size;
    }
    return 0;
}

/* The RW segment starts on a fresh page of file, congruent with its address.
 * Sections without bytes are placed, as GNU ld places them, where the
 * segment's bytes end. */
static int BoxAsmWriteWritable(BoxAsmLinkImage *Image, BoxAsmBuffer *File,
                               BoxAsmFileLayout *Layout)
{
    uint64_t DataAddress = Image->Sections[BOXASM_SLOT_DATA].Address;
    size_t Slot;
    int Rc;

    Layout->DataStart = BoxAsmAlignUp(File->Size, BOXASM_PAGE);
    if (Layout->DataStart == UINT64_MAX)
        return -ERR_NO_MEMORY;
    Rc = BoxAsmPadTo(File, Layout->DataStart);
    if (Rc != 0)
        return Rc;
    Layout->DataEnd = Layout->DataStart;
    for (Slot = BOXASM_SLOT_DATA; Slot < BOXASM_SLOT_COUNT; Slot++) {
        BoxAsmOutputSection *Section = &Image->Sections[Slot];

        if (Section->Type == BOXASM_SHT_NOBITS)
            continue;
        Section->FileOffset = Layout->DataStart + Section->Address - DataAddress;
        if (Section->Size == 0)
            continue;
        Rc = BoxAsmPadTo(File, Section->FileOffset);
        if (Rc == 0)
            Rc = BoxAsmBufferAppend(File, Section->Data.Data, Section->Data.Size);
        if (Rc != 0)
            return Rc;
        Layout->DataEnd = File->Size;
    }
    Image->Sections[BOXASM_SLOT_TBSS].FileOffset = Layout->DataEnd;
    Image->Sections[BOXASM_SLOT_BSS].FileOffset = Layout->DataEnd;
    return 0;
}

static int BoxAsmWriteTrailer(BoxAsmLinkImage *Image, BoxAsmBuffer *File,
                              const BoxAsmBuffer *SectionNames,
                              BoxAsmFileLayout *Layout)
{
    int Rc = BoxAsmBufferAlign(File, 8u);

    if (Rc == 0 && Image->Symbols.Size != 0) {
        Layout->SymtabOffset = File->Size;
        Rc = BoxAsmBufferAppend(File, Image->Symbols.Data, Image->Symbols.Size);
        if (Rc == 0) {
            Layout->StrtabOffset = File->Size;
            Rc = BoxAsmBufferAppend(File, Image->SymbolNames.Data,
                                    Image->SymbolNames.Size);
        }
    }
    if (Rc == 0) {
        Layout->ShstrtabOffset = File->Size;
        Rc = BoxAsmBufferAppend(File, SectionNames->Data, SectionNames->Size);
    }
    if (Rc == 0)
        Rc = BoxAsmBufferAlign(File, 8u);
    Layout->HeadersOffset = File->Size;
    return Rc;
}

static int BoxAsmAppendSectionHeaders(const BoxAsmLinkImage *Image,
                                      const BoxAsmTrailer *Trailer,
                                      const BoxAsmFileLayout *Layout,
                                      uint64_t SectionNamesSize, BoxAsmBuffer *File)
{
    BoxAsmElfSectionHeader Header;
    size_t Slot;
    int Rc;

    memset(&Header, 0, sizeof(Header));
    Rc = BoxAsmBufferAppend(File, &Header, sizeof(Header));
    for (Slot = 1; Rc == 0 && Slot < BOXASM_SLOT_COUNT; Slot++) {
        const BoxAsmOutputSection *Section = &Image->Sections[Slot];

        Header = (BoxAsmElfSectionHeader){
            Layout->Names[Slot], Section->Type, Section->Flags, Section->Address,
            Section->FileOffset, Section->Size, 0, 0, Section->Align, 0
        };
        Rc = BoxAsmBufferAppend(File, &Header, sizeof(Header));
    }
    if (Rc == 0 && Trailer->Symtab != 0) {
        Header = (BoxAsmElfSectionHeader){
            Layout->Names[Trailer->Symtab], BOXASM_SHT_SYMTAB, 0, 0,
            Layout->SymtabOffset, Image->Symbols.Size, Trailer->Strtab,
            Image->FirstGlobal, 8, sizeof(BoxAsmElfSymbol)
        };
        Rc = BoxAsmBufferAppend(File, &Header, sizeof(Header));
        Header = (BoxAsmElfSectionHeader){
            Layout->Names[Trailer->Strtab], BOXASM_SHT_STRTAB, 0, 0,
            Layout->StrtabOffset, Image->SymbolNames.Size, 0, 0, 1, 0
        };
        if (Rc == 0)
            Rc = BoxAsmBufferAppend(File, &Header, sizeof(Header));
    }
    Header = (BoxAsmElfSectionHeader){
        Layout->Names[Trailer->Shstrtab], BOXASM_SHT_STRTAB, 0, 0,
        Layout->ShstrtabOffset, SectionNamesSize, 0, 0, 1, 0
    };
    if (Rc == 0)
        Rc = BoxAsmBufferAppend(File, &Header, sizeof(Header));
    return Rc;
}

static void BoxAsmWriteFileHeaders(const BoxAsmLinkImage *Image,
                                   const BoxAsmTrailer *Trailer,
                                   const BoxAsmFileLayout *Layout,
                                   uint16_t ProgramCount, BoxAsmBuffer *File)
{
    const BoxAsmOutputSection *Data = &Image->Sections[BOXASM_SLOT_DATA];
    const BoxAsmOutputSection *Tdata = &Image->Sections[BOXASM_SLOT_TDATA];
    BoxAsmElfProgramHeader Programs[3];
    BoxAsmElfHeader Header;

    memset(Programs, 0, sizeof(Programs));
    Programs[0] = (BoxAsmElfProgramHeader){
        BOXASM_PT_LOAD, BOXASM_PF_R | BOXASM_PF_X, BOXASM_PAGE,
        BOXASM_USER_BASE, BOXASM_USER_BASE,
        Layout->ReadOnlyEnd - BOXASM_PAGE, Layout->ReadOnlyEnd - BOXASM_PAGE,
        BOXASM_PAGE
    };
    Programs[1] = (BoxAsmElfProgramHeader){
        BOXASM_PT_LOAD, BOXASM_PF_R | BOXASM_PF_W, Layout->DataStart,
        Data->Address, Data->Address, Layout->DataEnd - Layout->DataStart,
        Image->ProgramEnd - Data->Address, BOXASM_PAGE
    };
    if (ProgramCount == 3u)
        Programs[2] = (BoxAsmElfProgramHeader){
            BOXASM_PT_TLS, BOXASM_PF_R,
            Layout->DataStart + Image->TlsAddress - Data->Address,
            Image->TlsAddress, Image->TlsAddress, Tdata->Size,
            Image->TlsMemorySize, Image->TlsAlign
        };

    memset(&Header, 0, sizeof(Header));
    Header.Ident[0] = BOXASM_ELF_MAGIC0;
    Header.Ident[1] = BOXASM_ELF_MAGIC1;
    Header.Ident[2] = BOXASM_ELF_MAGIC2;
    Header.Ident[3] = BOXASM_ELF_MAGIC3;
    Header.Ident[4] = BOXASM_ELF_CLASS64;
    Header.Ident[5] = BOXASM_ELF_LSB;
    Header.Ident[6] = BOXASM_ELF_CURRENT;
    Header.Type = BOXASM_ET_EXEC;
    Header.Machine = BOXASM_EM_X86_64;
    Header.Version = BOXASM_ELF_CURRENT;
    Header.Entry = Image->Entry;
    Header.ProgramHeaderOffset = sizeof(Header);
    Header.SectionHeaderOffset = Layout->HeadersOffset;
    Header.HeaderSize = sizeof(Header);
    Header.ProgramHeaderSize = sizeof(BoxAsmElfProgramHeader);
    Header.ProgramHeaderCount = ProgramCount;
    Header.SectionHeaderSize = sizeof(BoxAsmElfSectionHeader);
    Header.SectionHeaderCount = Trailer->Count;
    Header.SectionNameIndex = Trailer->Shstrtab;
    memcpy(File->Data, &Header, sizeof(Header));
    memcpy(File->Data + sizeof(Header), Programs,
           ProgramCount * sizeof(BoxAsmElfProgramHeader));
}

/* The image as the BoxOS loader takes it: ELF header and program headers in
 * the first page, two W^X PT_LOAD segments, PT_TLS when there is a TLS block,
 * then the section names and headers. */
int BoxAsmWriteOutput(BoxAsmLinker *Link, BoxAsmBlob *Out)
{
    BoxAsmLinkImage *Image = &Link->Image;
    BoxAsmBuffer File = { 0 };
    BoxAsmBuffer SectionNames = { 0 };
    BoxAsmFileLayout Layout;
    BoxAsmTrailer Trailer;
    uint16_t ProgramCount = Image->TlsMemorySize != 0 ? 3u : 2u;
    int Rc;

    memset(&Layout, 0, sizeof(Layout));
    memset(&Trailer, 0, sizeof(Trailer));
    Trailer.Count = BOXASM_SLOT_COUNT;
    if (Image->Symbols.Size != 0) {
        Trailer.Symtab = Trailer.Count++;
        Trailer.Strtab = Trailer.Count++;
    }
    Trailer.Shstrtab = Trailer.Count++;

    Rc = BoxAsmBuildSectionNames(Image, &Trailer, &SectionNames, &Layout);
    if (Rc == 0)
        Rc = BoxAsmBufferAppendZeroes(&File, sizeof(BoxAsmElfHeader) +
                                      ProgramCount * sizeof(BoxAsmElfProgramHeader));
    if (Rc == 0)
        Rc = BoxAsmWriteReadOnly(Image, &File, &Layout);
    if (Rc == 0)
        Rc = BoxAsmWriteWritable(Image, &File, &Layout);
    if (Rc == 0)
        Rc = BoxAsmWriteTrailer(Image, &File, &SectionNames, &Layout);
    if (Rc == 0)
        Rc = BoxAsmAppendSectionHeaders(Image, &Trailer, &Layout,
                                        SectionNames.Size, &File);
    if (Rc == 0) {
        BoxAsmWriteFileHeaders(Image, &Trailer, &Layout, ProgramCount, &File);
        Out->Data = File.Data;
        Out->Size = File.Size;
        File.Data = NULL;
    } else if (Rc != -ERR_NO_MEMORY) {
        BoxAsmMessageSet(Link->Error, Link->ErrorCapacity,
                         "the image layout cannot be written as an ELF file");
    }
    BoxAsmBufferRelease(&SectionNames);
    BoxAsmBufferRelease(&File);
    return Rc;
}
