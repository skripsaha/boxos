#include "boxasm_source.h"

#include "boxasm_asm.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define BOXASM_SHOWN_LINE 120u

const char *const BoxAsmSectionNames[BOXASM_SECTION_COUNT] = {
    ".text", ".rodata", ".data", ".bss"
};

static const uint64_t BoxAsmSectionAlign[BOXASM_SECTION_COUNT] = { 16, 8, 8, 8 };

uint32_t BoxAsmSectionSymbol(BoxAsmSection Section)
{
    return (uint32_t)Section + 1u;
}

int BoxAsmGrowArray(void **Items, size_t *Capacity, size_t Count, size_t Size)
{
    size_t Fresh;
    void *Grown;

    if (Count < *Capacity)
        return 0;
    Fresh = *Capacity ? *Capacity * 2u : 64u;
    if (Fresh < Count || Fresh > SIZE_MAX / Size)
        return -ERR_NO_MEMORY;
    Grown = realloc(*Items, Fresh * Size);
    if (!Grown)
        return -ERR_NO_MEMORY;
    *Items = Grown;
    *Capacity = Fresh;
    return 0;
}

/* ---- Diagnostics -------------------------------------------------------- */

/* "file:line:column: " — or nothing at all during a layout pass, whose
 * values are not settled yet. */
void BoxAsmFailBegin(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, BoxAsmMessage *Message)
{
    if (Asm->Measuring) {
        BoxAsmMessageBegin(Message, NULL, 0);
        return;
    }
    BoxAsmMessageBegin(Message, Asm->Error, Asm->ErrorCapacity);
    BoxAsmMessageText(Message, Asm->FileName);
    BoxAsmMessageText(Message, ":");
    BoxAsmMessageDecimal(Message, Where->Line);
    BoxAsmMessageText(Message, ":");
    BoxAsmMessageDecimal(Message, Where->Column);
    BoxAsmMessageText(Message, ": ");
}

/* The source line under the message and a caret at the column, its tabs
 * kept so the caret lands where the eye does. */
int BoxAsmFailEnd(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, BoxAsmMessage *Message)
{
    const char *Line = Where->LineStart;
    size_t Length = 0;
    uint32_t Column;

    if (!Line)
        return -ERR_INVALID_ARGUMENT;
    while (Length < BOXASM_SHOWN_LINE && Line[Length] != '\0' && Line[Length] != '\n' &&
           Line[Length] != '\r')
        Length++;
    BoxAsmMessageText(Message, "\n    ");
    BoxAsmMessageSpan(Message, Line, Length);
    BoxAsmMessageText(Message, "\n    ");
    for (Column = 1; Column < Where->Column && Column <= Length; Column++)
        BoxAsmMessageSpan(Message, Line[Column - 1u] == '\t' ? "\t" : " ", 1u);
    BoxAsmMessageText(Message, "^");
    (void)Asm;
    return -ERR_INVALID_ARGUMENT;
}

int BoxAsmFail(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, const char *Text,
               const char *Name)
{
    BoxAsmMessage Message;

    BoxAsmFailBegin(Asm, Where, &Message);
    if (Name) {
        BoxAsmMessageName(&Message, Name);
        BoxAsmMessageText(&Message, " ");
    }
    BoxAsmMessageText(&Message, Text);
    return BoxAsmFailEnd(Asm, Where, &Message);
}

/* ---- The assembler's life ----------------------------------------------- */

/* Symbol 0 means none; 1..4 stand for the sections' starts. */
static int BoxAsmBegin(BoxAsmAssembler *Asm, const char *FileName, char *Error,
                       size_t ErrorCapacity)
{
    size_t Index;

    memset(Asm, 0, sizeof(*Asm));
    Asm->FileName = FileName ? FileName : "source";
    Asm->Error = Error;
    Asm->ErrorCapacity = ErrorCapacity;
    Asm->ExprCount = 1;
    if (BoxAsmGrowArray((void **)&Asm->Symbols, &Asm->SymbolCapacity, 0,
                        sizeof(BoxAsmSymbol)) != 0)
        return -ERR_NO_MEMORY;
    memset(Asm->Symbols, 0, (BOXASM_SECTION_COUNT + 1u) * sizeof(BoxAsmSymbol));
    for (Index = 0; Index < BOXASM_SECTION_COUNT; Index++) {
        BoxAsmSymbol *Symbol = &Asm->Symbols[BoxAsmSectionSymbol((BoxAsmSection)Index)];
        Symbol->Kind = BOXASM_SYMBOL_SECTION;
        Symbol->Section = (uint8_t)Index;
        Symbol->Placed = 1;
        Asm->Sections[Index].Align = BoxAsmSectionAlign[Index];
    }
    Asm->SymbolCount = BOXASM_SECTION_COUNT + 1u;
    return 0;
}

static void BoxAsmEnd(BoxAsmAssembler *Asm)
{
    size_t Index;

    for (Index = 0; Index < Asm->SymbolCount; Index++)
        free(Asm->Symbols[Index].Name);
    for (Index = 0; Index < BOXASM_SECTION_COUNT; Index++) {
        BoxAsmBufferRelease(&Asm->Sections[Index].Bytes);
        free(Asm->Sections[Index].Relocations);
    }
    free(Asm->Symbols);
    free(Asm->Exprs);
    free(Asm->Statements);
    free(Asm->Items);
    free(Asm->Taken);
    BoxAsmBufferRelease(&Asm->StringBytes);
    BoxAsmNameIndexRelease(&Asm->SymbolIndex);
    memset(Asm, 0, sizeof(*Asm));
}

int BoxAsmSourceAssemble(const char *Source, size_t SourceSize, const char *FileName,
                         BoxAsmBlob *Out, char *Error, size_t ErrorCapacity)
{
    BoxAsmAssembler Asm;
    char *Text;
    int Rc;

    BoxAsmMessageSet(Error, ErrorCapacity, "");
    if (!Source || !Out || SourceSize == SIZE_MAX) {
        BoxAsmMessageSet(Error, ErrorCapacity, "the source is missing");
        return -ERR_INVALID_ARGUMENT;
    }
    memset(Out, 0, sizeof(*Out));
    Text = BoxAsmCopyText(Source, SourceSize);
    if (!Text)
        return -ERR_NO_MEMORY;
    Rc = BoxAsmBegin(&Asm, FileName, Error, ErrorCapacity);
    if (Rc == 0)
        Rc = BoxAsmParse(&Asm, Text, SourceSize);
    if (Rc == 0)
        Rc = BoxAsmLayout(&Asm);
    if (Rc == 0)
        Rc = BoxAsmEmit(&Asm);
    if (Rc == 0)
        Rc = BoxAsmWriteObject(&Asm, Out);
    if (Rc == -ERR_NO_MEMORY)
        BoxAsmMessageSet(Error, ErrorCapacity, "out of memory while assembling");
    BoxAsmEnd(&Asm);
    free(Text);
    if (Out->Data)
        Out->Name = FileName;
    return Rc;
}
