#include "boxasm_macro.h"
#include "boxasm_source.h"

#include "boxasm_asm.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define BOXASM_SHOWN_LINE 120u

/* The stack the work may take: of the 64 KiB a BoxOS program has, what the
 * command takes before the work begins, what an inner construct takes before
 * it is the one named (BOXASM_STACK_OWN), and what one more level and a
 * refusal take after the last look are left over. */
#define BOXASM_STACK_BUDGET (40u << 10)
#define BOXASM_STACK_OWN    (4u << 10)

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

int BoxAsmTooDeep(const BoxAsmAssembler *Asm, uintptr_t Start)
{
    uintptr_t Here = (uintptr_t)__builtin_frame_address(0);

    return Here < Asm->StackBase && Asm->StackBase - Here > BOXASM_STACK_BUDGET &&
           Here < Start && Start - Here > BOXASM_STACK_OWN;
}

/* ---- Diagnostics -------------------------------------------------------- */

/* The source line a location stands for, and the macro call it was
 * written in: through the preprocessor's lines once the text read is its
 * expansion, as the preprocessor said while it works. */
static void BoxAsmPlace(const BoxAsmAssembler *Asm, const BoxAsmLocation *Where, uint32_t *Line,
                        uint32_t *Frame)
{
    *Line = Where->Line;
    *Frame = Asm->FailFrame;
    if (Asm->Expanded && Asm->Expansion)
        BoxAsmExpansionPlace(Asm->Expansion, Where->Line, Line, Frame);
}

/* "file:line:column: " — or nothing at all during a layout pass, whose
 * values are not settled yet. */
void BoxAsmFailBegin(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, BoxAsmMessage *Message)
{
    uint32_t Line;
    uint32_t Frame;

    if (Asm->Measuring) {
        BoxAsmMessageBegin(Message, NULL, 0);
        return;
    }
    BoxAsmPlace(Asm, Where, &Line, &Frame);
    BoxAsmMessageBegin(Message, Asm->Error, Asm->ErrorCapacity);
    BoxAsmMessageText(Message, Asm->FileName);
    BoxAsmMessageText(Message, ":");
    BoxAsmMessageDecimal(Message, Line);
    BoxAsmMessageText(Message, ":");
    BoxAsmMessageDecimal(Message, Where->Column);
    BoxAsmMessageText(Message, ": ");
}

/* The line under the message — as the macros made it — and a caret at the
 * column, its tabs kept so the caret lands where the eye does; then each
 * macro call the line was reached through, the innermost first. */
int BoxAsmFailEnd(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, BoxAsmMessage *Message)
{
    const char *Line = Where->LineStart;
    size_t Length = 0;
    uint32_t Column;
    uint32_t Source;
    uint32_t Frame;

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
    BoxAsmPlace(Asm, Where, &Source, &Frame);
    while (Asm->Expansion && Frame != 0 && Frame < Asm->Expansion->FrameCount) {
        const BoxAsmFrame *Call = &Asm->Expansion->Frames[Frame];
        const char *Name = (const char *)Asm->Expansion->Names.Data + Call->Name;
        uint32_t Times = 1;

        /* A macro calling itself from one line is one line said once. */
        Frame = Call->Parent;
        while (Frame != 0 && Frame < Asm->Expansion->FrameCount &&
               Asm->Expansion->Frames[Frame].Line == Call->Line &&
               strcmp((const char *)Asm->Expansion->Names.Data +
                      Asm->Expansion->Frames[Frame].Name, Name) == 0) {
            Frame = Asm->Expansion->Frames[Frame].Parent;
            Times++;
        }
        BoxAsmMessageText(Message, "\n    in the macro ");
        BoxAsmMessageName(Message, Name);
        BoxAsmMessageText(Message, " called at line ");
        BoxAsmMessageDecimal(Message, Call->Line);
        if (Times > 1) {
            BoxAsmMessageText(Message, " (");
            BoxAsmMessageDecimal(Message, Times);
            BoxAsmMessageText(Message, " calls, one inside another)");
        }
    }
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
    BoxAsmExpansion Expansion;
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
    memset(&Expansion, 0, sizeof(Expansion));
    Rc = BoxAsmBegin(&Asm, FileName, Error, ErrorCapacity);
    Asm.StackBase = (uintptr_t)__builtin_frame_address(0);
    if (Rc == 0)
        Rc = BoxAsmPreprocess(&Asm, Text, SourceSize, &Expansion);
    if (Rc == 0) {
        Asm.Expanded = 1;
        Rc = BoxAsmParse(&Asm, (const char *)Expansion.Text.Data, (size_t)Expansion.Text.Size);
    }
    if (Rc == 0)
        Rc = BoxAsmLayout(&Asm);
    if (Rc == 0)
        Rc = BoxAsmEmit(&Asm);
    if (Rc == 0)
        Rc = BoxAsmWriteObject(&Asm, Out);
    if (Rc == -ERR_NO_MEMORY)
        BoxAsmMessageSet(Error, ErrorCapacity, "out of memory while assembling");
    BoxAsmEnd(&Asm);
    BoxAsmExpansionRelease(&Expansion);
    free(Text);
    if (Out->Data)
        Out->Name = FileName;
    return Rc;
}
