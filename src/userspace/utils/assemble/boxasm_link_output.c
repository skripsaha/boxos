#include "boxasm_link_internal.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* Header indices after the output sections. */
typedef struct {
    uint16_t Recipe;
    uint16_t Symtab;
    uint16_t Strtab;
    uint16_t Shstrtab;
    uint16_t Count;
} BoxAsmTrailer;

typedef struct {
    BoxAsmSegment Segments[BOXASM_SEGMENT_COUNT];
    uint64_t SegmentOffset[BOXASM_SEGMENT_COUNT];
    uint64_t RecipeOffset;
    uint64_t SymtabOffset;
    uint64_t StrtabOffset;
    uint64_t ShstrtabOffset;
    uint64_t HeadersOffset;
    uint32_t Names[BOXASM_SLOT_COUNT + 4];
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
    if (Rc == 0 && Trailer->Recipe != 0)
        Rc = BoxAsmAppendName(Names, ".boxos.recipe", &Layout->Names[Trailer->Recipe]);
    if (Rc == 0 && Trailer->Symtab != 0)
        Rc = BoxAsmAppendName(Names, ".symtab", &Layout->Names[Trailer->Symtab]);
    if (Rc == 0 && Trailer->Strtab != 0)
        Rc = BoxAsmAppendName(Names, ".strtab", &Layout->Names[Trailer->Strtab]);
    if (Rc == 0)
        Rc = BoxAsmAppendName(Names, ".shstrtab", &Layout->Names[Trailer->Shstrtab]);
    return Rc;
}

/* Each segment from a fresh page of file, its file offset and its address
 * advancing together; the first page holds the headers. A section without
 * bytes is placed, as GNU ld places one, where its segment's bytes end. */
static int BoxAsmWriteSegments(BoxAsmLinkImage *Image, BoxAsmBuffer *File,
                               BoxAsmFileLayout *Layout)
{
    size_t Kind;

    for (Kind = 0; Kind < BOXASM_SEGMENT_COUNT; Kind++) {
        const BoxAsmSegment *Segment = &Layout->Segments[Kind];
        uint64_t Offset = BoxAsmAlignUp(File->Size, BOXASM_PAGE);
        size_t Slot;
        int Rc;

        if (Offset == UINT64_MAX)
            return -ERR_NO_MEMORY;
        Rc = BoxAsmPadTo(File, Offset);
        if (Rc == 0)
            Rc = BoxAsmBufferAppendZeroes(File, Segment->FileBytes);
        if (Rc != 0)
            return Rc;
        BoxAsmSegmentCopy(Image, Segment, File->Data + Offset);
        Layout->SegmentOffset[Kind] = Offset;
        for (Slot = Segment->First; Slot <= Segment->Last; Slot++) {
            BoxAsmOutputSection *Section = &Image->Sections[Slot];

            Section->FileOffset = Section->Type == BOXASM_SHT_NOBITS
                                  ? Offset + Segment->FileBytes
                                  : Offset + Section->Address - Segment->Address;
        }
    }
    return 0;
}

/* After the segments, where none maps it: the Recipe, then the symbols and
 * the section names. */
static int BoxAsmWriteTrailer(BoxAsmLinkImage *Image, const BoxAsmBlob *Recipe,
                              BoxAsmBuffer *File, const BoxAsmBuffer *SectionNames,
                              BoxAsmFileLayout *Layout)
{
    int Rc = 0;

    if (Recipe) {
        Layout->RecipeOffset = File->Size;
        Rc = BoxAsmBufferAppend(File, Recipe->Data, Recipe->Size);
    }
    if (Rc == 0)
        Rc = BoxAsmBufferAlign(File, 8u);

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
                                      const BoxAsmFileLayout *Layout, uint64_t RecipeSize,
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
    if (Rc == 0 && Trailer->Recipe != 0) {
        Header = (BoxAsmElfSectionHeader){
            Layout->Names[Trailer->Recipe], BOXASM_SHT_PROGBITS, 0, 0,
            Layout->RecipeOffset, RecipeSize, 0, 0, 1, 0
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
                                   const BoxAsmFileLayout *Layout, BoxAsmBuffer *File)
{
    const BoxAsmSegment *Data = &Layout->Segments[BOXASM_SEGMENT_DATA];
    BoxAsmElfProgramHeader Programs[BOXASM_SEGMENT_COUNT + 1];
    BoxAsmElfHeader Header;
    uint16_t Count = 0;
    size_t Kind;

    for (Kind = 0; Kind < BOXASM_SEGMENT_COUNT; Kind++) {
        const BoxAsmSegment *Segment = &Layout->Segments[Kind];

        if (Segment->Memory != 0)
            Programs[Count++] = (BoxAsmElfProgramHeader){
                BOXASM_PT_LOAD, Segment->Rights, Layout->SegmentOffset[Kind],
                Segment->Address, Segment->Address, Segment->FileBytes, Segment->Memory,
                BOXASM_PAGE
            };
    }
    if (Image->TlsMemorySize != 0)
        Programs[Count++] = (BoxAsmElfProgramHeader){
            BOXASM_PT_TLS, BOXASM_PF_R,
            Layout->SegmentOffset[BOXASM_SEGMENT_DATA] + Image->TlsAddress - Data->Address,
            Image->TlsAddress, Image->TlsAddress, Image->Sections[BOXASM_SLOT_TDATA].Size,
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
    Header.ProgramHeaderCount = Count;
    Header.SectionHeaderSize = sizeof(BoxAsmElfSectionHeader);
    Header.SectionHeaderCount = Trailer->Count;
    Header.SectionNameIndex = Trailer->Shstrtab;
    memcpy(File->Data, &Header, sizeof(Header));
    memcpy(File->Data + sizeof(Header), Programs, Count * sizeof(BoxAsmElfProgramHeader));
}

/* The image as the BoxOS loader takes it: ELF header and program headers in
 * the first page, a PT_LOAD for each segment with the rights user.ld gives
 * it, PT_TLS when there is a TLS block, then the Recipe when there is one,
 * the section names and headers. */
int BoxAsmWriteOutput(BoxAsmLinker *Link, BoxAsmBlob *Out)
{
    BoxAsmLinkImage *Image = &Link->Image;
    BoxAsmBuffer File = { 0 };
    BoxAsmBuffer SectionNames = { 0 };
    BoxAsmFileLayout Layout;
    BoxAsmTrailer Trailer;
    size_t Kind;
    int Rc;

    memset(&Layout, 0, sizeof(Layout));
    memset(&Trailer, 0, sizeof(Trailer));
    Trailer.Count = BOXASM_SLOT_COUNT;
    if (Link->Recipe)
        Trailer.Recipe = Trailer.Count++;
    if (Image->Symbols.Size != 0) {
        Trailer.Symtab = Trailer.Count++;
        Trailer.Strtab = Trailer.Count++;
    }
    Trailer.Shstrtab = Trailer.Count++;
    for (Kind = 0; Kind < BOXASM_SEGMENT_COUNT; Kind++)
        BoxAsmSegmentOf(Image, (BoxAsmSegmentKind)Kind, &Layout.Segments[Kind]);

    Rc = BoxAsmBuildSectionNames(Image, &Trailer, &SectionNames, &Layout);
    if (Rc == 0)                               /* room for every program header there can be */
        Rc = BoxAsmBufferAppendZeroes(&File, sizeof(BoxAsmElfHeader) +
                                      (BOXASM_SEGMENT_COUNT + 1u) * sizeof(BoxAsmElfProgramHeader));
    if (Rc == 0)
        Rc = BoxAsmWriteSegments(Image, &File, &Layout);
    if (Rc == 0)
        Rc = BoxAsmWriteTrailer(Image, Link->Recipe, &File, &SectionNames, &Layout);
    if (Rc == 0)
        Rc = BoxAsmAppendSectionHeaders(Image, &Trailer, &Layout,
                                        Link->Recipe ? Link->Recipe->Size : 0,
                                        SectionNames.Size, &File);
    if (Rc == 0) {
        BoxAsmWriteFileHeaders(Image, &Trailer, &Layout, &File);
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
