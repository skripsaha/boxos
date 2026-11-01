#include "boxasm_isa_internal.h"

#include "box/error.h"
#include "box/string.h"

/* Why the last form tried did not match, to say the most useful thing when
 * none does. */
typedef enum {
    BOXASM_MISMATCH_OPERANDS = 0,
    BOXASM_MISMATCH_UNSIZED,
    BOXASM_MISMATCH_SIZE,
    BOXASM_MISMATCH_RANGE,
    BOXASM_MISMATCH_MEMORY,     /* memory of another size than the form reads */
    BOXASM_MISMATCH_MASK,       /* an xmm register where the form reads xmm0 */
    BOXASM_MISMATCH_WIDTH,      /* an xmm register where the form reads a ymm, or the reverse */
    BOXASM_MISMATCH_SELECTOR,   /* something but a register where a blend selects by one */
    BOXASM_MISMATCH_VSIB        /* an address without a vector index where a gather reads one */
} BoxAsmMismatch;

static uint8_t BoxAsmSizeBit(uint8_t Bytes)
{
    switch (Bytes) {
    case 1: return BOXASM_SIZE_8;
    case 2: return BOXASM_SIZE_16;
    case 4: return BOXASM_SIZE_32;
    case 8: return BOXASM_SIZE_64;
    case 16: return BOXASM_SIZE_128;
    default: return 0;
    }
}

static uint8_t BoxAsmSingleSize(uint8_t Sizes)
{
    switch (Sizes) {
    case BOXASM_SIZE_8:  return 1;
    case BOXASM_SIZE_16: return 2;
    case BOXASM_SIZE_32: return 4;
    case BOXASM_SIZE_64: return 8;
    case BOXASM_SIZE_128: return 16;
    default: return 0;
    }
}

static int BoxAsmIsConstant(const BoxAsmOperand *Operand)
{
    return Operand->Kind == BOXASM_OPERAND_IMMEDIATE && Operand->Value.Symbol == 0;
}

static int BoxAsmInRange(const BoxAsmOperand *Operand, int64_t Low, int64_t High)
{
    return BoxAsmIsConstant(Operand) &&
           Operand->Value.Constant >= Low && Operand->Value.Constant <= High;
}

/* Whether an immediate fits the operation size: a constant by its range, a
 * symbol only where a 32-bit field can carry it through a relocation. */
static int BoxAsmImmediateFits(const BoxAsmOperand *Operand, uint8_t Size)
{
    if (!BoxAsmIsConstant(Operand))
        return Size == 4 || Size == 8;
    switch (Size) {
    case 1: return BoxAsmInRange(Operand, -128, 255);
    case 2: return BoxAsmInRange(Operand, -32768, 65535);
    case 4: return BoxAsmInRange(Operand, INT32_MIN, (int64_t)UINT32_MAX);
    case 8: return BoxAsmInRange(Operand, INT32_MIN, INT32_MAX);
    default: return 0;
    }
}

/* The immediate as an operation of Size bytes reads it — cut to its width,
 * taken as signed — fits a sign-extended byte: add eax, 0xFFFFFFFF is
 * add eax, -1. */
static int BoxAsmSignedByte(const BoxAsmOperand *Operand, uint8_t Size)
{
    int64_t Value;

    if (!BoxAsmIsConstant(Operand) || !BoxAsmImmediateFits(Operand, Size))
        return 0;
    Value = Operand->Value.Constant;
    if (Size == 1)
        Value = (int8_t)Value;
    else if (Size == 2)
        Value = (int16_t)Value;
    else if (Size == 4)
        Value = (int32_t)Value;
    return Value >= -128 && Value <= 127;
}

static int BoxAsmIsRegister(const BoxAsmOperand *Operand, uint8_t Size)
{
    return Operand->Kind == BOXASM_OPERAND_REGISTER &&
           (Operand->Register.Class == BOXASM_REGISTER_GENERAL ||
            Operand->Register.Class == BOXASM_REGISTER_HIGH8) &&
           Operand->Register.Size == Size;
}

static int BoxAsmIsMemory(const BoxAsmOperand *Operand, uint8_t Size)
{
    return Operand->Kind == BOXASM_OPERAND_MEMORY &&
           (Operand->Size == 0 || Operand->Size == Size);
}

static int BoxAsmIsXmm(const BoxAsmOperand *Operand)
{
    return Operand->Kind == BOXASM_OPERAND_REGISTER &&
           Operand->Register.Class == BOXASM_REGISTER_XMM;
}

static int BoxAsmIsYmm(const BoxAsmOperand *Operand)
{
    return Operand->Kind == BOXASM_OPERAND_REGISTER &&
           Operand->Register.Class == BOXASM_REGISTER_YMM;
}

static int BoxAsmIsVector(const BoxAsmOperand *Operand)
{
    return BoxAsmIsXmm(Operand) || BoxAsmIsYmm(Operand);
}

/* The register a blend selects by: when what stands there is no vector
 * register at all, noted, so the refusal can say that one is wanted. */
static int BoxAsmIsSelector(const BoxAsmOperand *Operand, int Matches, BoxAsmMismatch *Why)
{
    if (!Matches && !BoxAsmIsVector(Operand))
        *Why = BOXASM_MISMATCH_SELECTOR;
    return Matches;
}

/* Memory of Bytes for a form that reads exactly that many; memory said to be
 * of another size is noted, so the refusal can say what the form reads. */
static int BoxAsmIsMemoryOf(BoxAsmEncoder *Encoder, const BoxAsmOperand *Operand,
                            uint8_t Bytes, BoxAsmMismatch *Why)
{
    if (BoxAsmIsMemory(Operand, Bytes))
        return 1;
    if (Operand->Kind == BOXASM_OPERAND_MEMORY) {
        *Why = BOXASM_MISMATCH_MEMORY;
        Encoder->MemoryWanted = Bytes;
    }
    return 0;
}

/* The address of a gather: memory indexed by a vector register of the class's
 * width, and of the size of one element when it says a size. */
static int BoxAsmIsGatherAddress(BoxAsmEncoder *Encoder, const BoxAsmOperand *Operand,
                                 uint8_t IndexClass, uint8_t Element, BoxAsmMismatch *Why)
{
    if (Operand->Kind != BOXASM_OPERAND_MEMORY)
        return 0;
    if (Operand->Index.Class != BOXASM_REGISTER_XMM && Operand->Index.Class != BOXASM_REGISTER_YMM) {
        *Why = BOXASM_MISMATCH_VSIB;
        return 0;
    }
    if (Operand->Index.Class != IndexClass) {
        if (*Why == BOXASM_MISMATCH_OPERANDS)
            *Why = BOXASM_MISMATCH_WIDTH;
        return 0;
    }
    return BoxAsmIsMemoryOf(Encoder, Operand, Element, Why);
}

static int BoxAsmIsMask(const BoxAsmOperand *Operand, BoxAsmMismatch *Why)
{
    if (!BoxAsmIsXmm(Operand))
        return 0;
    if (Operand->Register.Number == 0)
        return 1;
    *Why = BOXASM_MISMATCH_MASK;
    return 0;
}

static int BoxAsmClassHolds(BoxAsmEncoder *Encoder, const BoxAsmOperand *Operand,
                            uint8_t Class, uint8_t Size, BoxAsmMismatch *Why)
{
    int Immediate = Operand->Kind == BOXASM_OPERAND_IMMEDIATE &&
                    (Operand->Size == 0 || Operand->Size == Size);

    switch (Class) {
    case BOXASM_CLASS_REG:     return BoxAsmIsRegister(Operand, Size);
    case BOXASM_CLASS_RM:      return BoxAsmIsRegister(Operand, Size) || BoxAsmIsMemory(Operand, Size);
    case BOXASM_CLASS_MEM:     return BoxAsmIsMemory(Operand, Size);
    case BOXASM_CLASS_ADDRESS: return Operand->Kind == BOXASM_OPERAND_MEMORY;
    case BOXASM_CLASS_ACC:     return BoxAsmIsRegister(Operand, Size) &&
                                      Operand->Register.Class == BOXASM_REGISTER_GENERAL &&
                                      Operand->Register.Number == 0;
    case BOXASM_CLASS_CL:      return BoxAsmIsRegister(Operand, 1) &&
                                      Operand->Register.Class == BOXASM_REGISTER_GENERAL &&
                                      Operand->Register.Number == 1;
    case BOXASM_CLASS_ONE:     return Immediate && BoxAsmInRange(Operand, 1, 1);
    case BOXASM_CLASS_IMM:
        if (Immediate && !BoxAsmImmediateFits(Operand, Size))
            *Why = BOXASM_MISMATCH_RANGE;
        return Immediate && BoxAsmImmediateFits(Operand, Size);
    case BOXASM_CLASS_IMM_U32: return Immediate && BoxAsmInRange(Operand, 0, (int64_t)UINT32_MAX);
    case BOXASM_CLASS_IMM_S32: return Immediate && BoxAsmInRange(Operand, INT32_MIN, INT32_MAX);
    case BOXASM_CLASS_IMM8S:   return Immediate && BoxAsmSignedByte(Operand, Size);
    case BOXASM_CLASS_IMM8:    return Operand->Kind == BOXASM_OPERAND_IMMEDIATE &&
                                      BoxAsmInRange(Operand, -128, 255);
    case BOXASM_CLASS_IMM16:   return Operand->Kind == BOXASM_OPERAND_IMMEDIATE &&
                                      BoxAsmInRange(Operand, 0, 65535);
    case BOXASM_CLASS_IMM64:   return Immediate;
    case BOXASM_CLASS_REL:     return Operand->Kind == BOXASM_OPERAND_IMMEDIATE;
    case BOXASM_CLASS_RM8:     return BoxAsmIsRegister(Operand, 1) ||
                                      (Operand->Kind == BOXASM_OPERAND_MEMORY && Operand->Size == 1);
    case BOXASM_CLASS_RM16:    return BoxAsmIsRegister(Operand, 2) ||
                                      (Operand->Kind == BOXASM_OPERAND_MEMORY && Operand->Size == 2);
    case BOXASM_CLASS_RM32:    return BoxAsmIsRegister(Operand, 4) ||
                                      (Operand->Kind == BOXASM_OPERAND_MEMORY && Operand->Size == 4);
    case BOXASM_CLASS_RM64:    return BoxAsmIsRegister(Operand, 8) ||
                                      (Operand->Kind == BOXASM_OPERAND_MEMORY && Operand->Size == 8);
    case BOXASM_CLASS_REG_R:   return BoxAsmIsRegister(Operand, Size);
    case BOXASM_CLASS_XMM:
    case BOXASM_CLASS_XMM_R:
    case BOXASM_CLASS_XMM_V:   return BoxAsmIsXmm(Operand);
    case BOXASM_CLASS_XMM_IS4: return BoxAsmIsSelector(Operand, BoxAsmIsXmm(Operand), Why);
    case BOXASM_CLASS_YMM:
    case BOXASM_CLASS_YMM_R:
    case BOXASM_CLASS_YMM_V:   return BoxAsmIsYmm(Operand);
    case BOXASM_CLASS_YMM_IS4: return BoxAsmIsSelector(Operand, BoxAsmIsYmm(Operand), Why);
    case BOXASM_CLASS_XMM0:    return BoxAsmIsMask(Operand, Why);
    case BOXASM_CLASS_XMM_M8:  return BoxAsmIsXmm(Operand) || BoxAsmIsMemoryOf(Encoder, Operand, 1, Why);
    case BOXASM_CLASS_XMM_M16: return BoxAsmIsXmm(Operand) || BoxAsmIsMemoryOf(Encoder, Operand, 2, Why);
    case BOXASM_CLASS_XMM_M32: return BoxAsmIsXmm(Operand) || BoxAsmIsMemoryOf(Encoder, Operand, 4, Why);
    case BOXASM_CLASS_XMM_M64: return BoxAsmIsXmm(Operand) || BoxAsmIsMemoryOf(Encoder, Operand, 8, Why);
    case BOXASM_CLASS_XMM_M128:
    case BOXASM_CLASS_XMM_U128: return BoxAsmIsXmm(Operand) ||
                                       BoxAsmIsMemoryOf(Encoder, Operand, 16, Why);
    case BOXASM_CLASS_YMM_M256:
    case BOXASM_CLASS_YMM_U256: return BoxAsmIsYmm(Operand) ||
                                       BoxAsmIsMemoryOf(Encoder, Operand, 32, Why);
    case BOXASM_CLASS_VSIB_X32: return BoxAsmIsGatherAddress(Encoder, Operand,
                                                             BOXASM_REGISTER_XMM, 4, Why);
    case BOXASM_CLASS_VSIB_X64: return BoxAsmIsGatherAddress(Encoder, Operand,
                                                             BOXASM_REGISTER_XMM, 8, Why);
    case BOXASM_CLASS_VSIB_Y32: return BoxAsmIsGatherAddress(Encoder, Operand,
                                                             BOXASM_REGISTER_YMM, 4, Why);
    case BOXASM_CLASS_VSIB_Y64: return BoxAsmIsGatherAddress(Encoder, Operand,
                                                             BOXASM_REGISTER_YMM, 8, Why);
    case BOXASM_CLASS_U128:    return BoxAsmIsMemoryOf(Encoder, Operand, 16, Why);
    case BOXASM_CLASS_U256:
    case BOXASM_CLASS_M256:    return BoxAsmIsMemoryOf(Encoder, Operand, 32, Why);
    case BOXASM_CLASS_M8:      return BoxAsmIsMemoryOf(Encoder, Operand, 1, Why);
    case BOXASM_CLASS_M16:     return BoxAsmIsMemoryOf(Encoder, Operand, 2, Why);
    case BOXASM_CLASS_M32:     return BoxAsmIsMemoryOf(Encoder, Operand, 4, Why);
    case BOXASM_CLASS_M64:     return BoxAsmIsMemoryOf(Encoder, Operand, 8, Why);
    case BOXASM_CLASS_M128:    return BoxAsmIsMemoryOf(Encoder, Operand, 16, Why);
    case BOXASM_CLASS_R32_M16: return BoxAsmIsRegister(Operand, 4) || BoxAsmIsRegister(Operand, 8) ||
                                      BoxAsmIsMemoryOf(Encoder, Operand, 2, Why);
    default:                   return 0;
    }
}

/* The bytes of vector register a class takes, 0 for a class that takes none. */
static uint8_t BoxAsmClassWidth(uint8_t Class)
{
    switch (Class) {
    case BOXASM_CLASS_XMM:
    case BOXASM_CLASS_XMM0:
    case BOXASM_CLASS_XMM_R:
    case BOXASM_CLASS_XMM_M32:
    case BOXASM_CLASS_XMM_M16:
    case BOXASM_CLASS_XMM_M8:
    case BOXASM_CLASS_XMM_M64:
    case BOXASM_CLASS_XMM_M128:
    case BOXASM_CLASS_XMM_U128:
    case BOXASM_CLASS_XMM_V:
    case BOXASM_CLASS_XMM_IS4: return 16;
    case BOXASM_CLASS_YMM:
    case BOXASM_CLASS_YMM_R:
    case BOXASM_CLASS_YMM_V:
    case BOXASM_CLASS_YMM_IS4:
    case BOXASM_CLASS_YMM_M256:
    case BOXASM_CLASS_YMM_U256: return 32;
    default:                   return 0;
    }
}

/* A vector register of the other width than the class takes is noted, unless
 * something more particular has been: xmm and ymm are different registers in
 * every class. */
static int BoxAsmClassMatches(BoxAsmEncoder *Encoder, const BoxAsmOperand *Operand,
                              uint8_t Class, uint8_t Size, BoxAsmMismatch *Why)
{
    int Matched = BoxAsmClassHolds(Encoder, Operand, Class, Size, Why);
    uint8_t Width = BoxAsmClassWidth(Class);

    if (!Matched && Width != 0 && BoxAsmIsVector(Operand) && Operand->Register.Size != Width &&
        *Why == BOXASM_MISMATCH_OPERANDS)
        *Why = BOXASM_MISMATCH_WIDTH;
    return Matched;
}

/* The size the form's sized operands agree on, the instruction's only size
 * when none says (push, setcc, cqo), or 0 with *Why when they disagree or
 * nothing says: inc [rax] is refused, never taken for a byte. */
static uint8_t BoxAsmOperationSize(const BoxAsmEncoder *Encoder, const BoxAsmForm *Form,
                                   BoxAsmMismatch *Why)
{
    uint8_t Size = 0;
    uint8_t Index;

    for (Index = 0; Index < Encoder->Count; Index++) {
        const BoxAsmOperand *Operand = &Encoder->Operands[Index];
        uint8_t Class = Form->Class[Index];
        uint8_t Own;

        if (Class != BOXASM_CLASS_REG && Class != BOXASM_CLASS_REG_R &&
            Class != BOXASM_CLASS_RM && Class != BOXASM_CLASS_MEM &&
            Class != BOXASM_CLASS_ACC && Class != BOXASM_CLASS_IMM)
            continue;
        if (BoxAsmIsVector(Operand))    /* never a general operand: the class refuses it */
            continue;
        Own = Operand->Kind == BOXASM_OPERAND_REGISTER ? Operand->Register.Size : Operand->Size;
        if (Own == 0)
            continue;
        if (Size != 0 && Own != Size) {
            *Why = BOXASM_MISMATCH_SIZE;
            return 0;
        }
        Size = Own;
    }
    if (Size == 0 && Form->Sizes != 0)
        Size = Encoder->OnlySize;
    if (Size == 0 && Form->Sizes != 0)
        *Why = BOXASM_MISMATCH_UNSIZED;
    return Size;
}

static uint8_t BoxAsmFormOperands(const BoxAsmForm *Form)
{
    uint8_t Count = 0;

    while (Count < 4 && Form->Class[Count] != BOXASM_CLASS_NONE)
        Count++;
    return Count;
}

/* The operands as a form reads them: those written, and for a form that lets
 * the second be left out, that second as the first again. */
static void BoxAsmPresentOperands(BoxAsmEncoder *Encoder, const BoxAsmForm *Form, uint8_t Count)
{
    uint8_t Index;

    memcpy(Encoder->Operands, Encoder->Given, sizeof(Encoder->Operands));
    Encoder->Count = Encoder->GivenCount;
    if (!(Form->Flags & BOXASM_FORM_DUPLICATE) || Encoder->GivenCount == 0 ||
        Count != Encoder->GivenCount + 1u)
        return;
    for (Index = Count - 1u; Index > 1; Index--)
        Encoder->Operands[Index] = Encoder->Operands[Index - 1u];
    Encoder->Operands[1] = Encoder->Operands[0];
    Encoder->Count = Count;
}

static int BoxAsmFormMatches(BoxAsmEncoder *Encoder, const BoxAsmForm *Form, uint8_t Short,
                             BoxAsmMismatch *Why)
{
    uint8_t Count = BoxAsmFormOperands(Form);
    uint8_t Index;

    BoxAsmPresentOperands(Encoder, Form, Count);
    if (Count != Encoder->Count)
        return 0;
    if (Form->Class[0] == BOXASM_CLASS_REL &&
        ((Form->Flags & BOXASM_FORM_SHORT) != 0) != (Short != 0))
        return 0;
    Encoder->OperationSize = BoxAsmOperationSize(Encoder, Form, Why);
    if (Form->Sizes != 0 &&
        (Encoder->OperationSize == 0 ||
         (Form->Sizes & BoxAsmSizeBit(Encoder->OperationSize)) == 0))
        return 0;
    for (Index = 0; Index < Count; Index++)
        if (!BoxAsmClassMatches(Encoder, &Encoder->Operands[Index], Form->Class[Index],
                                Encoder->OperationSize, Why))
            return 0;
    if ((Form->Flags & BOXASM_FORM_NOT_EAX) != 0 && Encoder->OperationSize == 4 &&
        Encoder->Operands[0].Register.Number == 0 && Encoder->Operands[1].Register.Number == 0)
        return 0;
    return 1;
}

int BoxAsmIsGatherClass(uint8_t Class)
{
    return Class >= BOXASM_CLASS_VSIB_X32 && Class <= BOXASM_CLASS_VSIB_Y64;
}

/* Whether the prefix the source wrote goes with the instruction and the
 * form it took: lock only on a memory destination of an instruction the
 * processor lets lock, rep only on movs, stos and lods, repe and repne only on
 * cmps and scas. Anything else the processor rejects (#UD) or reads as
 * another instruction (rep bsf is tzcnt), so it is refused here. */
static int BoxAsmCheckPrefix(BoxAsmEncoder *Encoder)
{
    const BoxAsmStatement *Statement = Encoder->Statement;
    const BoxAsmInstruction *Instruction = Statement->Instruction;
    const char *Why = NULL;
    BoxAsmMessage Message;

    switch (Statement->Prefix) {
    case BOXASM_PREFIX_LOCK:
        if (!(Instruction->Takes & BOXASM_TAKES_LOCK))
            Why = "lock goes only on add, adc, and, btc, btr, bts, cmpxchg, cmpxchg8b, "
                  "cmpxchg16b, dec, inc, neg, not, or, sbb, sub, xadd, xchg and xor, not on ";
        else if (Encoder->Operands[0].Kind != BOXASM_OPERAND_MEMORY &&
                 !((Instruction->Takes & BOXASM_TAKES_BOTH) &&
                   Encoder->Operands[1].Kind == BOXASM_OPERAND_MEMORY))
            Why = "lock needs a memory destination: the first operand of ";
        break;
    case BOXASM_PREFIX_REP:
        if (!(Instruction->Takes & (BOXASM_TAKES_REP | BOXASM_TAKES_REPE)))
            Why = "rep goes only on movs, stos and lods, not on ";
        else if (Encoder->Count != 0)
            Why = "rep repeats only a string instruction, written without operands, "
                  "not this form of ";
        else if (Instruction->Takes & BOXASM_TAKES_REPE)
            Why = "say when to stop with repe (while equal) or repne (while not equal), "
                  "not rep, before ";
        break;
    case BOXASM_PREFIX_REPE:
    case BOXASM_PREFIX_REPNE:
        if (!(Instruction->Takes & (BOXASM_TAKES_REP | BOXASM_TAKES_REPE)))
            Why = "repe and repne go only on cmps and scas, not on ";
        else if (Encoder->Count != 0)
            Why = "repe and repne repeat only cmps or scas, written without operands, "
                  "not this form of ";
        else if (!(Instruction->Takes & BOXASM_TAKES_REPE))
            Why = "movs, stos and lods repeat with rep; repe and repne go on cmps and "
                  "scas, not on ";
        break;
    default:
        break;
    }
    if (!Why)
        return 0;
    BoxAsmFailBegin(Encoder->Asm, &Statement->Where, &Message);
    BoxAsmMessageText(&Message, Why);
    BoxAsmMessageName(&Message, Instruction->Name);
    return BoxAsmFailEnd(Encoder->Asm, &Statement->Where, &Message);
}

static int BoxAsmHasYmmForm(const BoxAsmInstruction *Instruction)
{
    uint8_t Form;
    uint8_t Operand;

    for (Form = 0; Form < Instruction->FormCount; Form++)
        for (Operand = 0; Operand < 4; Operand++)
            if (BoxAsmClassWidth(Instruction->Forms[Form].Class[Operand]) == 32)
                return 1;
    return 0;
}

static int BoxAsmGivenYmm(const BoxAsmEncoder *Encoder)
{
    uint8_t Index;

    for (Index = 0; Index < Encoder->GivenCount; Index++)
        if (BoxAsmIsYmm(&Encoder->Given[Index]))
            return 1;
    return 0;
}

/* Registers of a width the instruction does not take: SSE's take xmm and say
 * which instruction takes ymm, AVX's have a 256-bit form only where the
 * processor has one. */
static void BoxAsmWidthMessage(const BoxAsmEncoder *Encoder, BoxAsmMessage *Message)
{
    const BoxAsmInstruction *Instruction = Encoder->Statement->Instruction;
    char Vex[32];
    uint8_t Condition;
    size_t Length = strlen(Instruction->Name);

    if (!(Instruction->Forms[0].Flags & BOXASM_FORM_VEX_MAP) && BoxAsmGivenYmm(Encoder)) {
        BoxAsmMessageName(Message, Instruction->Name);
        BoxAsmMessageText(Message, " is SSE's and takes xmm registers");
        if (Length + 2u <= sizeof(Vex)) {
            Vex[0] = 'v';
            memcpy(Vex + 1, Instruction->Name, Length + 1u);
            if (BoxAsmFindInstruction(Vex, Length + 1u, &Condition)) {
                BoxAsmMessageText(Message, "; for ymm write ");
                BoxAsmMessageName(Message, Vex);
            }
        }
        return;
    }
    BoxAsmMessageName(Message, Instruction->Name);
    if (BoxAsmGivenYmm(Encoder) && !BoxAsmHasYmmForm(Instruction))
        BoxAsmMessageText(Message, " has no 256-bit form");
    else
        BoxAsmMessageText(Message, " has no form with registers of these widths");
}

static int BoxAsmNoForm(BoxAsmEncoder *Encoder, BoxAsmMismatch Why)
{
    const BoxAsmStatement *Statement = Encoder->Statement;
    BoxAsmMessage Message;

    BoxAsmFailBegin(Encoder->Asm, &Statement->Where, &Message);
    switch (Why) {
    case BOXASM_MISMATCH_UNSIZED:
        BoxAsmMessageText(&Message, "the operation size is not said: write byte, word, "
                                    "dword or qword before the memory operand of ");
        break;
    case BOXASM_MISMATCH_SIZE:
        BoxAsmMessageText(&Message, "the operands are of different sizes for ");
        break;
    case BOXASM_MISMATCH_MEMORY:
        BoxAsmMessageText(&Message, "the memory is read as ");
        BoxAsmMessageDecimal(&Message, Encoder->MemoryWanted);
        BoxAsmMessageText(&Message, Encoder->MemoryWanted == 32
                                    ? " bytes (yword, or no size) by "
                                    : Encoder->MemoryWanted == 16
                                    ? " bytes (oword, or no size) by "
                                    : Encoder->MemoryWanted == 8
                                    ? " bytes (qword, or no size) by "
                                    : Encoder->MemoryWanted == 4
                                    ? " bytes (dword, or no size) by "
                                    : Encoder->MemoryWanted == 2
                                    ? " bytes (word, or no size) by "
                                    : " byte (byte, or no size) by ");
        break;
    case BOXASM_MISMATCH_MASK:
        BoxAsmMessageText(&Message, "the mask is only xmm0, written third or left out, in ");
        break;
    case BOXASM_MISMATCH_WIDTH:
        BoxAsmWidthMessage(Encoder, &Message);
        return BoxAsmFailEnd(Encoder->Asm, &Statement->Where, &Message);
    case BOXASM_MISMATCH_VSIB:
        BoxAsmMessageText(&Message,
                          "the address of a gather is indexed by an xmm or ymm register, in ");
        break;
    case BOXASM_MISMATCH_SELECTOR:
        BoxAsmMessageText(&Message, "the selector is a register, written last, in ");
        break;
    case BOXASM_MISMATCH_RANGE: {
        uint8_t At;
        for (At = 0; At < Encoder->GivenCount; At++)
            if (BoxAsmIsConstant(&Encoder->Given[At]))
                BoxAsmMessageDecimal(&Message, Encoder->Given[At].Value.Constant);
        BoxAsmMessageText(&Message, " does not fit the ");
        BoxAsmMessageDecimal(&Message, Encoder->OperationSize * 8);
        BoxAsmMessageText(&Message, "-bit operation of ");
        break;
    }
    default:
        BoxAsmMessageText(&Message, "these operands are not a form of ");
        break;
    }
    BoxAsmMessageName(&Message, Statement->Instruction->Name);
    return BoxAsmFailEnd(Encoder->Asm, &Statement->Where, &Message);
}

static int BoxAsmHasGatherForm(const BoxAsmInstruction *Instruction)
{
    uint8_t Form;

    for (Form = 0; Form < Instruction->FormCount; Form++)
        if (BoxAsmIsGatherClass(Instruction->Forms[Form].Class[1]))
            return 1;
    return 0;
}

static int BoxAsmVectorIndex(const BoxAsmOperand *Operand)
{
    return Operand->Kind == BOXASM_OPERAND_MEMORY &&
           (Operand->Index.Class == BOXASM_REGISTER_XMM ||
            Operand->Index.Class == BOXASM_REGISTER_YMM);
}

/* A vector register indexes the address of a gather and nothing else. */
static int BoxAsmCheckVectorIndex(BoxAsmEncoder *Encoder)
{
    const BoxAsmStatement *Statement = Encoder->Statement;
    BoxAsmMessage Message;
    uint8_t Index;

    for (Index = 0; Index < Encoder->GivenCount; Index++) {
        if (!BoxAsmVectorIndex(&Encoder->Given[Index]) ||
            BoxAsmHasGatherForm(Statement->Instruction))
            continue;
        BoxAsmFailBegin(Encoder->Asm, &Statement->Where, &Message);
        BoxAsmMessageText(&Message, "only a gather takes a vector register as the index of its "
                                    "address, not ");
        BoxAsmMessageName(&Message, Statement->Instruction->Name);
        return BoxAsmFailEnd(Encoder->Asm, &Statement->Where, &Message);
    }
    return 0;
}

/* The processor stops the program (#UD) when a gather's destination, its
 * index and its mask are not three different registers, whatever their
 * widths: the number of each is what is compared. */
static int BoxAsmCheckGather(BoxAsmEncoder *Encoder)
{
    const BoxAsmForm *Form = Encoder->Form;
    const BoxAsmStatement *Statement = Encoder->Statement;
    uint8_t Destination = 0;
    uint8_t Index = 0;
    uint8_t Mask = 0;
    BoxAsmMessage Message;
    uint8_t At;

    if (!BoxAsmIsGatherClass(Form->Class[1]))
        return 0;
    for (At = 0; At < Encoder->Count; At++) {
        uint8_t Class = Form->Class[At];

        if (BoxAsmIsGatherClass(Class))
            Index = Encoder->Operands[At].Index.Number;
        else if (Class == BOXASM_CLASS_XMM || Class == BOXASM_CLASS_YMM)
            Destination = Encoder->Operands[At].Register.Number;
        else if (Class == BOXASM_CLASS_XMM_V || Class == BOXASM_CLASS_YMM_V)
            Mask = Encoder->Operands[At].Register.Number;
    }
    if (Destination != Index && Destination != Mask && Index != Mask)
        return 0;
    BoxAsmFailBegin(Encoder->Asm, &Statement->Where, &Message);
    BoxAsmMessageText(&Message, "the destination, the index and the mask are three different "
                                "registers, or the processor stops the program, in ");
    BoxAsmMessageName(&Message, Statement->Instruction->Name);
    return BoxAsmFailEnd(Encoder->Asm, &Statement->Where, &Message);
}

int BoxAsmEncode(BoxAsmAssembler *Asm, const BoxAsmStatement *Statement, uint8_t Short,
                 BoxAsmEncoding *Out)
{
    const BoxAsmInstruction *Instruction = Statement->Instruction;
    BoxAsmEncoder Encoder;
    BoxAsmMismatch Why = BOXASM_MISMATCH_OPERANDS;
    uint8_t Sizes = 0;
    uint8_t Index;
    int Rc;

    memset(Out, 0, sizeof(*Out));
    memset(&Encoder, 0, sizeof(Encoder));
    Encoder.Asm = Asm;
    Encoder.Statement = Statement;
    Encoder.Out = Out;
    Encoder.GivenCount = Statement->OperandCount;
    memcpy(Encoder.Given, Statement->Operands, sizeof(Encoder.Given));
    if (Instruction->SwapImmediate && Encoder.GivenCount == 2 &&
        Encoder.Given[1].Kind == BOXASM_OPERAND_IMMEDIATE) {
        Encoder.Given[2] = Encoder.Given[1];
        Encoder.Given[1] = Encoder.Given[0];
        Encoder.GivenCount = 3;
    }
    for (Index = 0; Index < Instruction->FormCount; Index++)
        Sizes |= Instruction->Forms[Index].Sizes;
    Encoder.OnlySize = BoxAsmSingleSize(Sizes);
    if (Instruction->Kind == BOXASM_KIND_SHORT_BRANCH && Encoder.GivenCount == 1 &&
        Encoder.Given[0].Reach == BOXASM_BRANCH_NEAR)
        return BoxAsmFail(Asm, &Statement->Where,
                          "has no near form: it reaches -128 to 127 bytes", Instruction->Name);
    if ((Rc = BoxAsmCheckVectorIndex(&Encoder)) != 0)
        return Rc;
    for (Index = 0; Index < Instruction->FormCount; Index++) {
        if (BoxAsmFormMatches(&Encoder, &Instruction->Forms[Index], Short, &Why)) {
            Encoder.Form = &Instruction->Forms[Index];
            if ((Rc = BoxAsmCheckPrefix(&Encoder)) != 0 ||
                (Rc = BoxAsmCheckGather(&Encoder)) != 0)
                return Rc;
            return BoxAsmEmitForm(&Encoder, Short);
        }
    }
    return BoxAsmNoForm(&Encoder, Why);
}