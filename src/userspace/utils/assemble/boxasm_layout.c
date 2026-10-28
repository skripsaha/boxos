#include "boxasm_isa.h"

#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define BOXASM_MAX_PASSES        64u
#define BOXASM_MAX_SECTION_BYTES (1ull << 32)

static const uint8_t BoxAsmEndbr64[4] = { 0xF3, 0x0F, 0x1E, 0xFA };

/* The offset a value names inside Section, when it is anchored there. */
static int BoxAsmLocalTarget(const BoxAsmAssembler *Asm, const BoxAsmValue *Value,
                             uint8_t Section, uint64_t *Offset)
{
    const BoxAsmSymbol *Symbol;

    if (Value->Symbol == 0)
        return 0;
    Symbol = &Asm->Symbols[Value->Symbol];
    if (Symbol->Section != Section ||
        (Symbol->Kind != BOXASM_SYMBOL_LABEL && Symbol->Kind != BOXASM_SYMBOL_SECTION))
        return 0;
    *Offset = (Symbol->Kind == BOXASM_SYMBOL_LABEL ? Symbol->Offset : 0) +
              (uint64_t)Value->Constant;
    return 1;
}

static int BoxAsmEvaluateOperands(BoxAsmAssembler *Asm, BoxAsmStatement *Statement,
                                  uint64_t Here)
{
    uint8_t Index;

    for (Index = 0; Index < Statement->OperandCount; Index++) {
        BoxAsmOperand *Operand = &Statement->Operands[Index];
        int Rc;

        memset(&Operand->Value, 0, sizeof(Operand->Value));
        if (Operand->Expr != 0 &&
            (Rc = BoxAsmEvaluate(Asm, Operand->Expr, Here, Statement->Section,
                                 &Operand->Value)) != 0)
            return Rc;
    }
    return 0;
}

static int BoxAsmIsJump(const BoxAsmStatement *Statement)
{
    return Statement->Kind == BOXASM_STMT_INSTRUCTION &&
           Statement->Instruction->Kind == BOXASM_KIND_JUMP &&
           Statement->OperandCount == 1 &&
           Statement->Operands[0].Kind == BOXASM_OPERAND_IMMEDIATE;
}

static int BoxAsmIsShortOnly(const BoxAsmStatement *Statement)
{
    return Statement->Kind == BOXASM_STMT_INSTRUCTION &&
           Statement->Instruction->Kind == BOXASM_KIND_SHORT_BRANCH;
}

/* Whether a jump at Here takes its two-byte form: when it reaches its target
 * in the same section, or when the source says `short`. A target not yet
 * placed by the first pass is assumed near, so the first pass starts short.
 * loop and jrcxz have no other form. */
static uint8_t BoxAsmShortJump(const BoxAsmAssembler *Asm, const BoxAsmStatement *Statement,
                               uint64_t Here)
{
    const BoxAsmOperand *Target = &Statement->Operands[0];
    uint64_t Offset;
    int64_t Distance;

    if (BoxAsmIsShortOnly(Statement))
        return 1;
    if (!BoxAsmIsJump(Statement) || Statement->Long || Statement->Times != 0 ||
        Target->Reach == BOXASM_BRANCH_NEAR)
        return 0;
    if (Target->Reach == BOXASM_BRANCH_SHORT)
        return 1;
    if (!BoxAsmLocalTarget(Asm, &Target->Value, Statement->Section, &Offset))
        return 0;
    if (Asm->Symbols[Target->Value.Symbol].Kind == BOXASM_SYMBOL_LABEL &&
        !Asm->Symbols[Target->Value.Symbol].Placed)
        return 1;
    Distance = (int64_t)(Offset - (Here + 2u));
    return Distance >= -128 && Distance <= 127;
}

static int BoxAsmDataSize(const BoxAsmAssembler *Asm, const BoxAsmStatement *Statement,
                          uint64_t *Out)
{
    uint64_t Size = 0;
    uint32_t Index;

    for (Index = 0; Index < Statement->ItemCount; Index++) {
        const BoxAsmDataItem *Item = &Asm->Items[Statement->FirstItem + Index];
        uint64_t Bytes = Item->IsString
                         ? BoxAsmAlignUp(Item->StringLength, Statement->Width)
                         : Statement->Width;
        if (Bytes > BOXASM_MAX_SECTION_BYTES - Size)
            return -ERR_INVALID_ARGUMENT;
        Size += Bytes;
    }
    *Out = Size;
    return 0;
}

static int BoxAsmCount(BoxAsmAssembler *Asm, uint32_t Expr, const BoxAsmStatement *Statement,
                       const char *What, int64_t *Out)
{
    int Rc = BoxAsmEvaluateConstant(Asm, Expr, Statement->Offset, Statement->Section, Out);

    if (Rc == 0 && (*Out < 0 || (uint64_t)*Out > BOXASM_MAX_SECTION_BYTES))
        return BoxAsmFail(Asm, &Asm->Exprs[Expr].Where, What, NULL);
    return Rc;
}

/* The bytes one statement takes at Here, `times` included. */
static int BoxAsmStatementSize(BoxAsmAssembler *Asm, BoxAsmStatement *Statement,
                               uint64_t Here, uint64_t *Size)
{
    int64_t Times = 1;
    int64_t Count;
    uint64_t One = 0;
    BoxAsmEncoding Encoding;
    int Rc = 0;

    if (Statement->Times != 0 &&
        (Rc = BoxAsmCount(Asm, Statement->Times, Statement, "times repeats 0 to 2^32 times",
                          &Times)) != 0)
        return Rc;
    switch (Statement->Kind) {
    case BOXASM_STMT_INSTRUCTION:
        if ((Rc = BoxAsmEvaluateOperands(Asm, Statement, Here)) != 0)
            return Rc;
    {
        uint8_t Short = BoxAsmShortJump(Asm, Statement, Here);
        if (BoxAsmIsJump(Statement) && !Short)
            Statement->Long = 1;
        Rc = BoxAsmEncode(Asm, Statement, Short, &Encoding);
        One = Encoding.Length;
        break;
    }
    case BOXASM_STMT_DATA:
        if (BoxAsmDataSize(Asm, Statement, &One) != 0)
            return BoxAsmFail(Asm, &Statement->Where, "the data is larger than 4 GiB", NULL);
        break;
    case BOXASM_STMT_RESERVE:
        if ((Rc = BoxAsmCount(Asm, Statement->Count, Statement,
                              "reserves 0 to 2^32 items", &Count)) == 0)
            One = (uint64_t)Count * Statement->Width;
        break;
    case BOXASM_STMT_ALIGN:
        if ((Rc = BoxAsmCount(Asm, Statement->Count, Statement, "aligns to 1 to 4096 bytes",
                              &Count)) != 0)
            return Rc;
        if (Count == 0 || Count > 4096 || (Count & (Count - 1)) != 0)
            return BoxAsmFail(Asm, &Statement->Where,
                              "aligns to a power of two from 1 to 4096", NULL);
        One = BoxAsmAlignUp(Here, (uint64_t)Count) - Here;
        if ((uint64_t)Count > Asm->Sections[Statement->Section].Align)
            Asm->Sections[Statement->Section].Align = (uint64_t)Count;
        break;
    default:
        break;
    }
    if (Rc == 0 && Times != 0 && One > BOXASM_MAX_SECTION_BYTES / (uint64_t)Times)
        return BoxAsmFail(Asm, &Statement->Where, "the section would grow past 4 GiB", NULL);
    *Size = One * (uint64_t)Times;
    return Rc;
}

static int BoxAsmLayoutPass(BoxAsmAssembler *Asm, uint8_t *Changed)
{
    uint64_t Offsets[BOXASM_SECTION_COUNT] = { 0 };
    size_t Index;

    *Changed = 0;
    for (Index = 0; Index < Asm->StatementCount; Index++) {
        BoxAsmStatement *Statement = &Asm->Statements[Index];
        uint64_t Here = Offsets[Statement->Section];
        uint64_t Size = Statement->Size;
        int Rc;

        if (Statement->Offset != Here)
            *Changed = 1;
        Statement->Offset = Here;
        if (Statement->Kind == BOXASM_STMT_LABEL) {
            BoxAsmSymbol *Symbol = &Asm->Symbols[Statement->Symbol];
            if (Symbol->Offset != Here || !Symbol->Placed)
                *Changed = 1;
            Symbol->Offset = Here;
            Symbol->Placed = 1;
        }
        Rc = BoxAsmStatementSize(Asm, Statement, Here, &Size);
        if (Rc != 0 && (!Asm->Measuring || Rc == -ERR_NO_MEMORY))
            return Rc;
        if (Rc != 0)
            Size = Statement->Size;
        if (Size != Statement->Size)
            *Changed = 1;
        Statement->Size = Size;
        if (Size > BOXASM_MAX_SECTION_BYTES - Here) {
            Asm->Measuring = 0;
            return BoxAsmFail(Asm, &Statement->Where, "the section would grow past 4 GiB", NULL);
        }
        Offsets[Statement->Section] = Here + Size;
    }
    for (Index = 0; Index < BOXASM_SECTION_COUNT; Index++)
        Asm->Sections[Index].Size = Offsets[Index];
    return 0;
}

/* Passes until no offset or size moves. A jump only ever grows, so the
 * passes end; the last one, over settled values, says what is wrong. */
int BoxAsmLayout(BoxAsmAssembler *Asm)
{
    uint32_t Pass;

    for (Pass = 0; Pass < BOXASM_MAX_PASSES; Pass++) {
        uint8_t Changed;
        int Rc;

        Asm->Measuring = 1;
        if ((Rc = BoxAsmLayoutPass(Asm, &Changed)) != 0)
            return Rc;
        if (Changed)
            continue;
        Asm->Measuring = 0;
        if ((Rc = BoxAsmLayoutPass(Asm, &Changed)) != 0)
            return Rc;
        if (!Changed)
            return 0;
    }
    Asm->Measuring = 0;
    return BoxAsmFail(Asm, &Asm->Statements[0].Where,
                      "the program's layout does not settle: an address depends on "
                      "itself through times or align", NULL);
}

/* ---- Emitting ----------------------------------------------------------- */

static int BoxAsmAddRelocation(BoxAsmAssembler *Asm, uint8_t Section, uint64_t Offset,
                               uint32_t Type, uint32_t Symbol, int64_t Addend)
{
    BoxAsmSectionImage *Image = &Asm->Sections[Section];

    if (BoxAsmGrowArray((void **)&Image->Relocations, &Image->RelocationCapacity,
                        Image->RelocationCount, sizeof(BoxAsmRelocation)) != 0)
        return -ERR_NO_MEMORY;
    Image->Relocations[Image->RelocationCount++] =
        (BoxAsmRelocation){ Offset, Type, Symbol, Addend };
    Asm->Symbols[Symbol].Referenced = 1;
    return 0;
}

/* A code address used as a value is remembered, to be held to endbr64. */
static int BoxAsmNoteTaken(BoxAsmAssembler *Asm, const BoxAsmValue *Value,
                           const BoxAsmLocation *Where)
{
    uint64_t Offset;

    if (!BoxAsmLocalTarget(Asm, Value, BOXASM_SECTION_TEXT, &Offset))
        return 0;
    if (BoxAsmGrowArray((void **)&Asm->Taken, &Asm->TakenCapacity, Asm->TakenCount,
                        sizeof(BoxAsmTakenAddress)) != 0)
        return -ERR_NO_MEMORY;
    Asm->Taken[Asm->TakenCount++] = (BoxAsmTakenAddress){
        Offset,
        Asm->Symbols[Value->Symbol].Kind == BOXASM_SYMBOL_LABEL && Value->Constant == 0
            ? Value->Symbol : 0,
        *Where
    };
    return 0;
}

/* A field takes Width bytes: a signed one -2^(n-1)..2^(n-1)-1, an unsigned
 * one that or 0..2^n-1, as data may be written either way. */
static int BoxAsmRangeFails(BoxAsmAssembler *Asm, const BoxAsmLocation *Where, int64_t Value,
                            uint8_t Width, uint8_t Signed)
{
    int64_t Low = -(1ll << (Width * 8 - 1));
    int64_t High = Signed ? (1ll << (Width * 8 - 1)) - 1 : (int64_t)((1ull << (Width * 8)) - 1u);
    BoxAsmMessage Message;

    if (Width >= 8 || (Value >= Low && Value <= High))
        return 0;
    BoxAsmFailBegin(Asm, Where, &Message);
    BoxAsmMessageDecimal(&Message, Value);
    BoxAsmMessageText(&Message, " does not fit in ");
    BoxAsmMessageDecimal(&Message, Width * 8);
    BoxAsmMessageText(&Message, Signed ? " signed bits" : " bits");
    return BoxAsmFailEnd(Asm, Where, &Message);
}

/* A rel8 that cannot carry its target: one in another section or another
 * object is the linker's to place, and a byte is too narrow for that. */
static int BoxAsmShortReachFails(BoxAsmAssembler *Asm, const BoxAsmStatement *Statement,
                                 int Local, int64_t Distance)
{
    BoxAsmMessage Message;

    BoxAsmFailBegin(Asm, &Statement->Where, &Message);
    if (BoxAsmIsShortOnly(Statement)) {
        BoxAsmMessageName(&Message, Statement->Instruction->Name);
        BoxAsmMessageText(&Message, " reaches");
    } else {
        BoxAsmMessageText(&Message, "a short jump reaches");
    }
    if (!Local) {
        BoxAsmMessageText(&Message, " only a label of its own section");
    } else {
        BoxAsmMessageText(&Message, " -128 to 127 bytes, and its target is ");
        BoxAsmMessageDecimal(&Message, Distance);
        BoxAsmMessageText(&Message, " away");
    }
    return BoxAsmFailEnd(Asm, &Statement->Where, &Message);
}

/* A field resting on a symbol: a distance within the section is written now;
 * anything the linker must place becomes a relocation. */
static int BoxAsmResolveFixup(BoxAsmAssembler *Asm, const BoxAsmStatement *Statement,
                              uint64_t Here, uint8_t Length, const BoxAsmFixup *Fixup,
                              uint8_t *Bytes)
{
    const BoxAsmValue *Value = &Fixup->Value;
    uint64_t Place = Here + Fixup->Offset;
    uint64_t End = Here + Length;
    uint64_t Target;
    int64_t Field;
    int Rc;

    if (Fixup->TakesAddress && (Rc = BoxAsmNoteTaken(Asm, Value, &Statement->Where)) != 0)
        return Rc;
    if (Fixup->Kind == BOXASM_FIXUP_RELATIVE || Fixup->Kind == BOXASM_FIXUP_BRANCH) {
        int Local;

        if (Value->Symbol == 0)
            return BoxAsmFail(Asm, &Statement->Where, "a jump or call goes to a label", NULL);
        Local = BoxAsmLocalTarget(Asm, Value, Statement->Section, &Target);
        Field = Local ? (int64_t)(Target - End) : 0;
        if (Fixup->Width == 1 && (!Local || Field < -128 || Field > 127))
            return BoxAsmShortReachFails(Asm, Statement, Local, Field);
        if (Local) {
            if ((Rc = BoxAsmRangeFails(Asm, &Statement->Where, Field, Fixup->Width, 1)) != 0)
                return Rc;
            memcpy(Bytes + Fixup->Offset, &Field, Fixup->Width);
            return 0;
        }
        return BoxAsmAddRelocation(Asm, Statement->Section, Place,
                                   Fixup->Kind == BOXASM_FIXUP_BRANCH ? BOXASM_R_X86_64_PLT32
                                                                      : BOXASM_R_X86_64_PC32,
                                   Value->Symbol, Value->Constant - (int64_t)(End - Place));
    }
    if (Fixup->Width == 8)
        return BoxAsmAddRelocation(Asm, Statement->Section, Place, BOXASM_R_X86_64_64,
                                   Value->Symbol, Value->Constant);
    if (Fixup->Width != 4)
        return BoxAsmFail(Asm, &Statement->Where, "an address needs a 32- or 64-bit field", NULL);
    return BoxAsmAddRelocation(Asm, Statement->Section, Place,
                               Fixup->Kind == BOXASM_FIXUP_ABSOLUTE ? BOXASM_R_X86_64_32
                                                                    : BOXASM_R_X86_64_32S,
                               Value->Symbol, Value->Constant);
}

static int BoxAsmEmitInstruction(BoxAsmAssembler *Asm, BoxAsmStatement *Statement,
                                 uint64_t Times)
{
    uint64_t One = Times != 0 ? Statement->Size / Times : 0;
    uint64_t Repeat;

    for (Repeat = 0; Repeat < Times; Repeat++) {
        uint64_t Here = Statement->Offset + Repeat * One;
        BoxAsmEncoding Encoding;
        uint8_t Index;
        int Rc;

        if ((Rc = BoxAsmEvaluateOperands(Asm, Statement, Here)) != 0 ||
            (Rc = BoxAsmEncode(Asm, Statement, BoxAsmShortJump(Asm, Statement, Here),
                               &Encoding)) != 0)
            return Rc;
        if (Encoding.Length != One)
            return BoxAsmFail(Asm, &Statement->Where,
                              "repeated by times, this instruction changes its length", NULL);
        for (Index = 0; Index < Encoding.FixupCount; Index++)
            if ((Rc = BoxAsmResolveFixup(Asm, Statement, Here, Encoding.Length,
                                         &Encoding.Fixups[Index], Encoding.Bytes)) != 0)
                return Rc;
        if (BoxAsmBufferAppend(&Asm->Sections[Statement->Section].Bytes, Encoding.Bytes,
                               Encoding.Length) != 0)
            return -ERR_NO_MEMORY;
    }
    return 0;
}

static int BoxAsmEmitItem(BoxAsmAssembler *Asm, const BoxAsmStatement *Statement,
                          const BoxAsmDataItem *Item, BoxAsmBuffer *Bytes)
{
    BoxAsmFixup Fixup;
    BoxAsmValue Value;
    uint64_t Here = Bytes->Size;
    uint64_t Field;
    int Rc;

    if (Item->IsString) {
        if (BoxAsmBufferAppend(Bytes, Asm->StringBytes.Data + Item->StringOffset,
                               Item->StringLength) != 0 ||
            BoxAsmBufferAppendZeroes(Bytes, BoxAsmAlignUp(Item->StringLength, Statement->Width) -
                                            Item->StringLength) != 0)
            return -ERR_NO_MEMORY;
        return 0;
    }
    if ((Rc = BoxAsmEvaluate(Asm, Item->Expr, Here, Statement->Section, &Value)) != 0)
        return Rc;
    Field = Value.Symbol != 0 ? 0 : (uint64_t)Value.Constant;
    if (Value.Symbol == 0 &&
        (Rc = BoxAsmRangeFails(Asm, &Item->Where, Value.Constant, Statement->Width, 0)) != 0)
        return Rc;
    if (Value.Symbol != 0) {
        Fixup = (BoxAsmFixup){ BOXASM_FIXUP_ABSOLUTE, 0, Statement->Width, 1, Value };
        if ((Rc = BoxAsmResolveFixup(Asm, Statement, Here, Statement->Width, &Fixup,
                                     (uint8_t *)&Field)) != 0)
            return Rc;
    }
    return BoxAsmBufferAppend(Bytes, &Field, Statement->Width) == 0 ? 0 : -ERR_NO_MEMORY;
}

static int BoxAsmEmitStatement(BoxAsmAssembler *Asm, BoxAsmStatement *Statement)
{
    BoxAsmBuffer *Bytes = &Asm->Sections[Statement->Section].Bytes;
    int64_t Times = 1;
    int64_t Repeat;
    uint32_t Index;
    int Rc;

    if (Statement->Section == BOXASM_SECTION_BSS || Statement->Kind == BOXASM_STMT_LABEL)
        return 0;
    if (Bytes->Size != Statement->Offset)
        return BoxAsmFail(Asm, &Statement->Where,
                          "the emitted bytes do not match the layout", NULL);
    if (Statement->Times != 0 &&
        (Rc = BoxAsmCount(Asm, Statement->Times, Statement, "times repeats 0 to 2^32 times",
                          &Times)) != 0)
        return Rc;
    if (Statement->Kind == BOXASM_STMT_INSTRUCTION)
        return BoxAsmEmitInstruction(Asm, Statement, (uint64_t)Times);
    if (Statement->Kind == BOXASM_STMT_ALIGN) {
        uint8_t Fill = Statement->Section == BOXASM_SECTION_TEXT ? 0x90 : 0;
        for (Repeat = 0; (uint64_t)Repeat < Statement->Size; Repeat++)
            if (BoxAsmBufferAppend(Bytes, &Fill, 1u) != 0)
                return -ERR_NO_MEMORY;
        return 0;
    }
    for (Repeat = 0; Repeat < Times; Repeat++)
        for (Index = 0; Index < Statement->ItemCount; Index++)
            if ((Rc = BoxAsmEmitItem(Asm, Statement, &Asm->Items[Statement->FirstItem + Index],
                                     Bytes)) != 0)
                return Rc;
    return 0;
}

/* CET: every address of code a program uses as a value may become the
 * target of an indirect jump or call, and must begin with endbr64. */
static int BoxAsmCheckTaken(BoxAsmAssembler *Asm)
{
    const BoxAsmBuffer *Text = &Asm->Sections[BOXASM_SECTION_TEXT].Bytes;
    size_t Index;

    for (Index = 0; Index < Asm->TakenCount; Index++) {
        const BoxAsmTakenAddress *Taken = &Asm->Taken[Index];

        if (Taken->Offset <= Text->Size && Text->Size - Taken->Offset >= 4u &&
            memcmp(Text->Data + Taken->Offset, BoxAsmEndbr64, 4u) == 0)
            continue;
        return BoxAsmFail(Asm, &Taken->Where,
                          Taken->Symbol
                          ? "is used as an address of code, so it must begin with endbr64: "
                            "on a CET machine an indirect jump or call to it stops the program"
                          : "this address of code must point at endbr64: on a CET machine an "
                            "indirect jump or call to it stops the program",
                          Taken->Symbol ? Asm->Symbols[Taken->Symbol].Name : NULL);
    }
    return 0;
}

int BoxAsmEmit(BoxAsmAssembler *Asm)
{
    size_t Index;
    int Rc;

    for (Index = 0; Index < Asm->StatementCount; Index++)
        if ((Rc = BoxAsmEmitStatement(Asm, &Asm->Statements[Index])) != 0)
            return Rc;
    for (Index = 0; Index < BOXASM_SECTION_COUNT; Index++)
        if (Index != BOXASM_SECTION_BSS &&
            Asm->Sections[Index].Bytes.Size != Asm->Sections[Index].Size)
            return BoxAsmFail(Asm, &Asm->Statements[0].Where,
                              "the emitted bytes do not match the layout", NULL);
    return BoxAsmCheckTaken(Asm);
}
