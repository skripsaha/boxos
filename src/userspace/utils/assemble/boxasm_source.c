#include "boxasm_source.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

typedef struct {
    char    *Name;
    uint64_t Offset;
} BoxAsmSourceLabel;

typedef struct {
    uint64_t Offset;
    uint32_t Type;
    int64_t  Addend;
    uint32_t Symbol;
} BoxAsmSourceRelocation;

typedef struct {
    char    *Name;
    uint64_t Offset;
    uint32_t NameOffset;
} BoxAsmSourceString;

typedef struct {
    BoxAsmBuffer Text;
    BoxAsmBuffer Rodata;
    BoxAsmSourceLabel *Labels;
    size_t LabelCount;
    size_t LabelCapacity;
    BoxAsmSourceString *Strings;
    size_t StringCount;
    size_t StringCapacity;
    BoxAsmSourceRelocation *Relocations;
    size_t RelocationCount;
    size_t RelocationCapacity;
    uint8_t InText;
    uint8_t SawGlobalMain;
    uint8_t SawMain;
    uint8_t SawExit;
} BoxAsmSource;

#define BOXASM_SOURCE_SYMBOL_PRINT 0xFFFFFFFFu
#define BOXASM_SOURCE_SYMBOL_EXIT  0xFFFFFFFEu

static void BoxAsmSourceDrop(BoxAsmSource *Source)
{
    size_t Index;

    if (!Source)
        return;
    for (Index = 0; Index < Source->LabelCount; Index++)
        free(Source->Labels[Index].Name);
    for (Index = 0; Index < Source->StringCount; Index++)
        free(Source->Strings[Index].Name);
    free(Source->Labels);
    free(Source->Strings);
    free(Source->Relocations);
    BoxAsmBufferRelease(&Source->Text);
    BoxAsmBufferRelease(&Source->Rodata);
    memset(Source, 0, sizeof(*Source));
}

static char *BoxAsmTrim(char *Text)
{
    char *End;

    while (*Text == ' ' || *Text == '\t' || *Text == '\r')
        Text++;
    End = Text + strlen(Text);
    while (End > Text && (End[-1] == ' ' || End[-1] == '\t' || End[-1] == '\r'))
        *--End = '\0';
    return Text;
}

static int BoxAsmGrow(void **Array, size_t *Capacity, size_t Count,
                      size_t ElementSize)
{
    size_t NewCapacity;
    void *Fresh;

    if (Count < *Capacity)
        return 0;
    NewCapacity = *Capacity ? *Capacity * 2u : 16u;
    if (NewCapacity < Count || NewCapacity > SIZE_MAX / ElementSize)
        return -ERR_NO_MEMORY;
    Fresh = realloc(*Array, NewCapacity * ElementSize);
    if (!Fresh)
        return -ERR_NO_MEMORY;
    *Array = Fresh;
    *Capacity = NewCapacity;
    return 0;
}

static int BoxAsmAddLabel(BoxAsmSource *Source, const char *Name, uint64_t Offset)
{
    char *Copy;
    int Rc;

    if (!Name || Name[0] == '\0')
        return -ERR_INVALID_ARGUMENT;
    if (strlen(Name) > 127u)
        return -ERR_INVALID_ARGUMENT;
    if (BoxAsmGrow((void **)&Source->Labels, &Source->LabelCapacity,
                   Source->LabelCount, sizeof(*Source->Labels)) != 0)
        return -ERR_NO_MEMORY;
    Copy = BoxAsmCopyText(Name, strlen(Name));
    if (!Copy)
        return -ERR_NO_MEMORY;
    Source->Labels[Source->LabelCount++] = (BoxAsmSourceLabel){ Copy, Offset };
    Rc = 0;
    return Rc;
}

static int BoxAsmAddString(BoxAsmSource *Source, const char *Value, size_t Length,
                           uint64_t *OutOffset)
{
    char *Name;
    int Rc;

    if (!OutOffset || Length > SIZE_MAX - 1u)
        return -ERR_INVALID_ARGUMENT;
    if (BoxAsmBufferAppend(&Source->Rodata, Value, Length) != 0 ||
        BoxAsmBufferAppend(&Source->Rodata, "", 1u) != 0)
        return -ERR_NO_MEMORY;
    if (BoxAsmGrow((void **)&Source->Strings, &Source->StringCapacity,
                   Source->StringCount, sizeof(*Source->Strings)) != 0)
        return -ERR_NO_MEMORY;
    Name = (char *)malloc(32u);
    if (!Name)
        return -ERR_NO_MEMORY;
    /* The generated symbol is local and cannot collide with user labels. */
    {
        char Digits[16];
        size_t Count = 0;
        size_t Position = 0;
        size_t Index = Source->StringCount;
        do {
            Digits[Count++] = (char)('0' + (Index % 10u));
            Index /= 10u;
        } while (Index != 0);
        memcpy(Name + Position, ".Lbox_string_", 13u);
        Position += 13u;
        while (Count != 0)
            Name[Position++] = Digits[--Count];
        Name[Position] = '\0';
    }
    *OutOffset = Source->Rodata.Size - (uint64_t)Length - 1u;
    Source->Strings[Source->StringCount++] =
        (BoxAsmSourceString){ Name, *OutOffset, 0 };
    Rc = 0;
    return Rc;
}

static int BoxAsmAddRelocation(BoxAsmSource *Source, uint64_t Offset,
                               uint32_t Type, int64_t Addend, uint32_t Symbol)
{
    if (BoxAsmGrow((void **)&Source->Relocations,
                   &Source->RelocationCapacity, Source->RelocationCount,
                   sizeof(*Source->Relocations)) != 0)
        return -ERR_NO_MEMORY;
    Source->Relocations[Source->RelocationCount++] =
        (BoxAsmSourceRelocation){ Offset, Type, Addend, Symbol };
    return 0;
}

static int BoxAsmParseInteger(const char *Text, int64_t *Out)
{
    uint64_t Value = 0;
    int Negative = 0;
    size_t Index = 0;
    size_t Digits = 0;
    uint32_t Base = 10;

    if (!Text || !Out)
        return -ERR_INVALID_ARGUMENT;
    if (Text[Index] == '-') {
        Negative = 1;
        Index++;
    } else if (Text[Index] == '+') {
        Index++;
    }
    if (Text[Index] == '0' && (Text[Index + 1] == 'x' || Text[Index + 1] == 'X')) {
        Base = 16;
        Index += 2;
    }
    for (; Text[Index] != '\0'; Index++) {
        uint32_t Digit;
        char Ch = Text[Index];
        if (Ch >= '0' && Ch <= '9') Digit = (uint32_t)(Ch - '0');
        else if (Ch >= 'a' && Ch <= 'f') Digit = (uint32_t)(Ch - 'a' + 10);
        else if (Ch >= 'A' && Ch <= 'F') Digit = (uint32_t)(Ch - 'A' + 10);
        else return -ERR_INVALID_ARGUMENT;
        if (Digit >= Base || Value > (UINT64_MAX - Digit) / Base)
            return -ERR_INVALID_ARGUMENT;
        Value = Value * Base + Digit;
        Digits++;
    }
    if (Digits == 0)
        return -ERR_INVALID_ARGUMENT;
    if (Negative) {
        if (Value > (uint64_t)INT64_MAX + 1u)
            return -ERR_INVALID_ARGUMENT;
        *Out = -(int64_t)Value;
    } else {
        if (Value > (uint64_t)INT64_MAX)
            return -ERR_INVALID_ARGUMENT;
        *Out = (int64_t)Value;
    }
    return 0;
}

static int BoxAsmParseString(const char *Text, BoxAsmBuffer *Out)
{
    size_t Length;
    size_t Index;

    if (!Text || Text[0] != '"')
        return -ERR_INVALID_ARGUMENT;
    Length = strlen(Text);
    if (Length < 2u || Text[Length - 1u] != '"')
        return -ERR_INVALID_ARGUMENT;
    for (Index = 1; Index + 1u < Length; Index++) {
        char Ch = Text[Index];
        if (Ch != '\\') {
            if (BoxAsmBufferAppend(Out, &Ch, 1u) != 0)
                return -ERR_NO_MEMORY;
            continue;
        }
        if (++Index + 1u >= Length)
            return -ERR_INVALID_ARGUMENT;
        Ch = Text[Index];
        switch (Ch) {
        case 'n': Ch = '\n'; break;
        case 'r': Ch = '\r'; break;
        case 't': Ch = '\t'; break;
        case '0': Ch = '\0'; break;
        case '\\': break;
        case '"': break;
        default: return -ERR_INVALID_ARGUMENT;
        }
        if (BoxAsmBufferAppend(Out, &Ch, 1u) != 0)
            return -ERR_NO_MEMORY;
    }
    return 0;
}

static int BoxAsmParseLine(BoxAsmSource *Source, char *Line,
                           char *Error, size_t ErrorCapacity)
{
    char *Text = BoxAsmTrim(Line);
    int Rc;

    if (*Text == '\0')
        return 0;
    if (*Text == ';' || *Text == '#')
        return 0;
    {
        char *Comment = strchr(Text, ';');
        if (Comment) {
            *Comment = '\0';
            Text = BoxAsmTrim(Text);
            if (*Text == '\0')
                return 0;
        }
    }
    if (strncmp(Text, "section ", 8u) == 0) {
        const char *Section = BoxAsmTrim(Text + 8u);
        if (strcmp(Section, ".text") == 0) Source->InText = 1;
        else if (strcmp(Section, ".rodata") == 0) Source->InText = 0;
        else {
            BoxAsmMessageSet(Error, ErrorCapacity, "BoxASM supports only .text and .rodata");
            return -ERR_INVALID_ARGUMENT;
        }
        return 0;
    }
    if (strncmp(Text, "global ", 7u) == 0) {
        if (strcmp(BoxAsmTrim(Text + 7u), "main") != 0) {
            BoxAsmMessageSet(Error, ErrorCapacity, "BoxASM v0 exports only main");
            return -ERR_INVALID_ARGUMENT;
        }
        Source->SawGlobalMain = 1;
        return 0;
    }
    {
        size_t Length = strlen(Text);
        if (Length > 1u && Text[Length - 1u] == ':') {
            Text[Length - 1u] = '\0';
            if (!Source->InText || strcmp(Text, "main") != 0 || Source->SawMain) {
                BoxAsmMessageSet(Error, ErrorCapacity, "only one text label, main, is supported");
                return -ERR_INVALID_ARGUMENT;
            }
            Rc = BoxAsmAddLabel(Source, "main", Source->Text.Size);
            if (Rc != 0)
                return Rc;
            /* _start enters main with the ABI's incoming RSP % 16 == 8.
             * Keep main a real function before it calls BoxLib: the push
             * changes the caller-side alignment to 16, and ENDBR64 is the
             * landing pad required by the userspace CET policy. */
            if (BoxAsmBufferAppend(&Source->Text,
                                   "\xF3\x0F\x1E\xFA\x55\x48\x89\xE5", 8u) != 0)
                return -ERR_NO_MEMORY;
            Source->SawMain = 1;
            return 0;
        }
    }
    if (strncmp(Text, "box.say ", 8u) == 0) {
        BoxAsmBuffer Value = { 0 };
        uint64_t StringOffset;
        uint64_t At;

        if (!Source->InText || BoxAsmParseString(BoxAsmTrim(Text + 8u), &Value) != 0) {
            BoxAsmBufferRelease(&Value);
            BoxAsmMessageSet(Error, ErrorCapacity, "box.say requires a quoted string in .text");
            return -ERR_INVALID_ARGUMENT;
        }
        if (BoxAsmAddString(Source, (const char *)Value.Data, (size_t)Value.Size,
                            &StringOffset) != 0) {
            BoxAsmBufferRelease(&Value);
            return -ERR_NO_MEMORY;
        }
        BoxAsmBufferRelease(&Value);
        At = Source->Text.Size;
        if (BoxAsmBufferAppend(&Source->Text, "\x48\x8D\x3D\0\0\0\0", 7u) != 0 ||
            BoxAsmAddRelocation(Source, At + 3u, BOXASM_R_X86_64_PC32, -4,
                                (uint32_t)Source->StringCount - 1u) != 0 ||
            BoxAsmBufferAppend(&Source->Text, "\xE8\0\0\0\0", 5u) != 0 ||
            BoxAsmAddRelocation(Source, At + 8u, BOXASM_R_X86_64_PLT32, -4,
                                BOXASM_SOURCE_SYMBOL_PRINT) != 0)
            return -ERR_NO_MEMORY;
        (void)StringOffset;
        return 0;
    }
    if (strncmp(Text, "box.exit ", 9u) == 0) {
        int64_t Code;
        uint8_t Move[5] = { 0xBF, 0, 0, 0, 0 };
        uint64_t At;

        if (!Source->InText || BoxAsmParseInteger(BoxAsmTrim(Text + 9u), &Code) != 0 ||
            Code < INT32_MIN || Code > INT32_MAX) {
            BoxAsmMessageSet(Error, ErrorCapacity, "box.exit requires a signed 32-bit code");
            return -ERR_INVALID_ARGUMENT;
        }
        memcpy(Move + 1u, &Code, sizeof(uint32_t));
        At = Source->Text.Size;
        if (BoxAsmBufferAppend(&Source->Text, Move, sizeof(Move)) != 0 ||
            BoxAsmBufferAppend(&Source->Text, "\xE8\0\0\0\0", 5u) != 0 ||
            BoxAsmAddRelocation(Source, At + 6u, BOXASM_R_X86_64_PLT32, -4,
                                BOXASM_SOURCE_SYMBOL_EXIT) != 0)
            return -ERR_NO_MEMORY;
        Source->SawExit = 1;
        return 0;
    }

    BoxAsmMessageSet(Error, ErrorCapacity, "unknown BoxASM v0 statement");
    return -ERR_INVALID_ARGUMENT;
}

static int BoxAsmAppendString(BoxAsmBuffer *Strings, const char *Text,
                              uint32_t *OutOffset)
{
    if (!Strings || !Text || !OutOffset || Strings->Size > UINT32_MAX)
        return -ERR_INVALID_ARGUMENT;
    *OutOffset = (uint32_t)Strings->Size;
    return BoxAsmBufferAppend(Strings, Text, strlen(Text) + 1u);
}

static int BoxAsmBuildObject(BoxAsmSource *Source, BoxAsmBlob *Out,
                             char *Error, size_t ErrorCapacity)
{
    BoxAsmBuffer Strings = { 0 };
    BoxAsmBuffer SectionNames = { 0 };
    BoxAsmBuffer Rela = { 0 };
    BoxAsmBuffer Symbols = { 0 };
    BoxAsmBuffer File = { 0 };
    BoxAsmElfSectionHeader Sections[7];
    BoxAsmElfSymbol Symbol;
    BoxAsmElfRela Relocation;
    BoxAsmElfHeader Header;
    uint32_t MainName;
    uint32_t PrintName;
    uint32_t ExitName;
    uint32_t TextSectionName;
    uint32_t RelaTextName;
    uint32_t RodataName;
    uint32_t SymtabName;
    uint32_t StrtabName;
    uint32_t ShstrtabName;
    uint32_t FirstGlobal;
    size_t Index;
    int Rc = -ERR_NO_MEMORY;

    if (!Source->SawGlobalMain || !Source->SawMain || !Source->SawExit) {
        BoxAsmMessageSet(Error, ErrorCapacity,
                          "source must export main and contain box.exit");
        return -ERR_INVALID_ARGUMENT;
    }
    if (BoxAsmAppendString(&Strings, "", &MainName) != 0 ||
        BoxAsmAppendString(&Strings, "main", &MainName) != 0 ||
        BoxAsmAppendString(&Strings, "print", &PrintName) != 0 ||
        BoxAsmAppendString(&Strings, "exit", &ExitName) != 0)
        goto Done;
    for (Index = 0; Index < Source->StringCount; Index++)
        if (BoxAsmAppendString(&Strings, Source->Strings[Index].Name,
                               &Source->Strings[Index].NameOffset) != 0)
            goto Done;

    if (BoxAsmAppendString(&SectionNames, "", &TextSectionName) != 0 ||
        BoxAsmAppendString(&SectionNames, ".text", &TextSectionName) != 0 ||
        BoxAsmAppendString(&SectionNames, ".rela.text", &RelaTextName) != 0 ||
        BoxAsmAppendString(&SectionNames, ".rodata", &RodataName) != 0 ||
        BoxAsmAppendString(&SectionNames, ".symtab", &SymtabName) != 0 ||
        BoxAsmAppendString(&SectionNames, ".strtab", &StrtabName) != 0 ||
        BoxAsmAppendString(&SectionNames, ".shstrtab", &ShstrtabName) != 0)
        goto Done;

    /* ELF puts every local symbol before the first global one, and the
     * symbol table's sh_info names that boundary. The strings are local:
     * symbols 1..StringCount; main, print and exit follow. */
    memset(&Symbol, 0, sizeof(Symbol));
    if (BoxAsmBufferAppend(&Symbols, &Symbol, sizeof(Symbol)) != 0)
        goto Done;
    for (Index = 0; Index < Source->StringCount; Index++) {
        uint64_t End = Index + 1u < Source->StringCount ?
                       Source->Strings[Index + 1u].Offset : Source->Rodata.Size;
        memset(&Symbol, 0, sizeof(Symbol));
        Symbol.Name = Source->Strings[Index].NameOffset;
        Symbol.Info = BOXASM_SYMBOL_INFO(BOXASM_STB_LOCAL, BOXASM_STT_OBJECT);
        Symbol.SectionIndex = 3;
        Symbol.Value = Source->Strings[Index].Offset;
        Symbol.Size = End - Source->Strings[Index].Offset;
        if (BoxAsmBufferAppend(&Symbols, &Symbol, sizeof(Symbol)) != 0)
            goto Done;
    }
    FirstGlobal = (uint32_t)(Symbols.Size / sizeof(Symbol));
    memset(&Symbol, 0, sizeof(Symbol));
    Symbol.Name = MainName;
    Symbol.Info = BOXASM_SYMBOL_INFO(BOXASM_STB_GLOBAL, BOXASM_STT_FUNC);
    Symbol.SectionIndex = 1;
    Symbol.Value = Source->Labels[0].Offset;
    Symbol.Size = Source->Text.Size - Source->Labels[0].Offset;
    if (BoxAsmBufferAppend(&Symbols, &Symbol, sizeof(Symbol)) != 0)
        goto Done;
    memset(&Symbol, 0, sizeof(Symbol));
    Symbol.Name = PrintName;
    Symbol.Info = BOXASM_SYMBOL_INFO(BOXASM_STB_GLOBAL, BOXASM_STT_NOTYPE);
    Symbol.SectionIndex = BOXASM_SHN_UNDEF;
    if (BoxAsmBufferAppend(&Symbols, &Symbol, sizeof(Symbol)) != 0)
        goto Done;
    memset(&Symbol, 0, sizeof(Symbol));
    Symbol.Name = ExitName;
    Symbol.Info = BOXASM_SYMBOL_INFO(BOXASM_STB_GLOBAL, BOXASM_STT_NOTYPE);
    Symbol.SectionIndex = BOXASM_SHN_UNDEF;
    if (BoxAsmBufferAppend(&Symbols, &Symbol, sizeof(Symbol)) != 0)
        goto Done;

    for (Index = 0; Index < Source->RelocationCount; Index++) {
        BoxAsmSourceRelocation *Input = &Source->Relocations[Index];
        Relocation.Address = Input->Offset;
        Relocation.Info = BOXASM_RELA_INFO(
            Input->Symbol == BOXASM_SOURCE_SYMBOL_PRINT ? FirstGlobal + 1u :
            (Input->Symbol == BOXASM_SOURCE_SYMBOL_EXIT ? FirstGlobal + 2u :
             1u + Input->Symbol),
            Input->Type);
        Relocation.Addend = Input->Addend;
        if (BoxAsmBufferAppend(&Rela, &Relocation, sizeof(Relocation)) != 0)
            goto Done;
    }

    memset(Sections, 0, sizeof(Sections));
    Sections[1] = (BoxAsmElfSectionHeader){ TextSectionName,
                                            BOXASM_SHT_PROGBITS,
                                            BOXASM_SHF_ALLOC | BOXASM_SHF_EXECINSTR,
                                            0, 0, Source->Text.Size, 0, 0, 16, 0 };
    Sections[2] = (BoxAsmElfSectionHeader){ RelaTextName, BOXASM_SHT_RELA, 0, 0, 0,
                                            Rela.Size, 4, 1, 8, sizeof(BoxAsmElfRela) };
    Sections[3] = (BoxAsmElfSectionHeader){ RodataName, BOXASM_SHT_PROGBITS,
                                            BOXASM_SHF_ALLOC, 0, 0, Source->Rodata.Size,
                                            0, 0, 1, 0 };
    Sections[4] = (BoxAsmElfSectionHeader){ SymtabName, BOXASM_SHT_SYMTAB, 0, 0, 0,
                                            Symbols.Size, 5, FirstGlobal, 8,
                                            sizeof(BoxAsmElfSymbol) };
    Sections[5] = (BoxAsmElfSectionHeader){ StrtabName, BOXASM_SHT_STRTAB, 0, 0, 0,
                                            Strings.Size, 0, 0, 1, 0 };
    Sections[6] = (BoxAsmElfSectionHeader){ ShstrtabName, BOXASM_SHT_STRTAB, 0, 0, 0,
                                            SectionNames.Size, 0, 0, 1, 0 };

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
    Header.SectionHeaderOffset = 0;
    Header.HeaderSize = sizeof(Header);
    Header.SectionHeaderSize = sizeof(BoxAsmElfSectionHeader);
    Header.SectionHeaderCount = 7;
    Header.SectionNameIndex = 6;

    if (BoxAsmBufferAppend(&File, &Header, sizeof(Header)) != 0 ||
        BoxAsmBufferAlign(&File, 16u) != 0) goto Done;
    Sections[1].Offset = File.Size;
    if (BoxAsmBufferAppend(&File, Source->Text.Data, Source->Text.Size) != 0 ||
        BoxAsmBufferAlign(&File, 8u) != 0) goto Done;
    Sections[2].Offset = File.Size;
    if (BoxAsmBufferAppend(&File, Rela.Data, Rela.Size) != 0 ||
        BoxAsmBufferAlign(&File, 8u) != 0) goto Done;
    Sections[3].Offset = File.Size;
    if (BoxAsmBufferAppend(&File, Source->Rodata.Data, Source->Rodata.Size) != 0 ||
        BoxAsmBufferAlign(&File, 8u) != 0) goto Done;
    Sections[4].Offset = File.Size;
    if (BoxAsmBufferAppend(&File, Symbols.Data, Symbols.Size) != 0) goto Done;
    Sections[5].Offset = File.Size;
    if (BoxAsmBufferAppend(&File, Strings.Data, Strings.Size) != 0) goto Done;
    Sections[6].Offset = File.Size;
    if (BoxAsmBufferAppend(&File, SectionNames.Data, SectionNames.Size) != 0 ||
        BoxAsmBufferAlign(&File, 8u) != 0) goto Done;
    Header.SectionHeaderOffset = File.Size;
    if (BoxAsmBufferAppend(&File, Sections, sizeof(Sections)) != 0) goto Done;
    memcpy(File.Data, &Header, sizeof(Header));

    Out->Data = File.Data;
    Out->Size = File.Size;
    File.Data = NULL;
    File.Size = 0;
    File.Capacity = 0;
    Rc = 0;

Done:
    BoxAsmBufferRelease(&Strings);
    BoxAsmBufferRelease(&SectionNames);
    BoxAsmBufferRelease(&Rela);
    BoxAsmBufferRelease(&Symbols);
    BoxAsmBufferRelease(&File);
    return Rc;
}

int BoxAsmSourceAssemble(const char *Source, size_t SourceSize,
                         BoxAsmBlob *Out, char *Error, size_t ErrorCapacity)
{
    BoxAsmSource State;
    char *Copy;
    size_t Position = 0;
    int Rc = 0;

    if (!Source || !Out) {
        BoxAsmMessageSet(Error, ErrorCapacity, "source is missing");
        return -ERR_INVALID_ARGUMENT;
    }
    memset(Out, 0, sizeof(*Out));
    memset(&State, 0, sizeof(State));
    State.InText = 1;
    Copy = (char *)malloc(SourceSize + 1u);
    if (!Copy)
        return -ERR_NO_MEMORY;
    memcpy(Copy, Source, SourceSize);
    Copy[SourceSize] = '\0';

    while (Position <= SourceSize) {
        size_t Begin = Position;
        size_t Length;
        char *Line;

        while (Position < SourceSize && Copy[Position] != '\n')
            Position++;
        Length = Position - Begin;
        if (Position < SourceSize)
            Copy[Position++] = '\0';
        else
            Position++;
        Line = Copy + Begin;
        if (Length > 0 && Line[Length - 1u] == '\r')
            Line[Length - 1u] = '\0';
        Rc = BoxAsmParseLine(&State, Line, Error, ErrorCapacity);
        if (Rc != 0)
            break;
        if (Position > SourceSize)
            break;
    }
    if (Rc == 0)
        Rc = BoxAsmBuildObject(&State, Out, Error, ErrorCapacity);
    free(Copy);
    BoxAsmSourceDrop(&State);
    return Rc;
}
