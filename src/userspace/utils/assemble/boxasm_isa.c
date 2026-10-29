#include "boxasm_isa.h"

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
    BOXASM_MISMATCH_SELECTOR    /* something but a register where a blend selects by one */
} BoxAsmMismatch;

typedef struct {
    BoxAsmAssembler       *Asm;
    const BoxAsmStatement *Statement;
    const BoxAsmForm      *Form;
    BoxAsmOperand          Given[4];        /* the operands as written */
    uint8_t                GivenCount;
    BoxAsmOperand          Operands[4];     /* those of the form being tried */
    uint8_t                Count;
    uint8_t                OperationSize;   /* bytes; 0 for a form without one */
    uint8_t                OnlySize;        /* the instruction's one size, 0 when it has several */
    uint8_t                MemoryWanted;    /* MISMATCH_MEMORY: the bytes the form reads */
    BoxAsmEncoding        *Out;
    uint8_t                Rex;
    uint8_t                HasHigh8;
    uint8_t                ForceRex;
} BoxAsmEncoder;

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
    case BOXASM_CLASS_XMM_M16: return BoxAsmIsXmm(Operand) || BoxAsmIsMemoryOf(Encoder, Operand, 2, Why);
    case BOXASM_CLASS_XMM_M32: return BoxAsmIsXmm(Operand) || BoxAsmIsMemoryOf(Encoder, Operand, 4, Why);
    case BOXASM_CLASS_XMM_M64: return BoxAsmIsXmm(Operand) || BoxAsmIsMemoryOf(Encoder, Operand, 8, Why);
    case BOXASM_CLASS_XMM_M128:
    case BOXASM_CLASS_XMM_U128: return BoxAsmIsXmm(Operand) ||
                                       BoxAsmIsMemoryOf(Encoder, Operand, 16, Why);
    case BOXASM_CLASS_YMM_M256:
    case BOXASM_CLASS_YMM_U256: return BoxAsmIsYmm(Operand) ||
                                       BoxAsmIsMemoryOf(Encoder, Operand, 32, Why);
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

/* ---- Bytes -------------------------------------------------------------- */

static int BoxAsmPut(BoxAsmEncoder *Encoder, const void *Bytes, uint8_t Length)
{
    BoxAsmEncoding *Out = Encoder->Out;

    if (Out->Length + Length > BOXASM_MAX_INSTRUCTION)
        return BoxAsmFail(Encoder->Asm, &Encoder->Statement->Where,
                          "the instruction would be longer than 15 bytes", NULL);
    memcpy(Out->Bytes + Out->Length, Bytes, Length);
    Out->Length = (uint8_t)(Out->Length + Length);
    return 0;
}

/* A field of Width bytes holding Value: the constant now, or zero and a fixup
 * when it rests on a symbol. */
static int BoxAsmPutField(BoxAsmEncoder *Encoder, const BoxAsmValue *Value, uint8_t Width,
                          uint8_t Kind, uint8_t TakesAddress)
{
    BoxAsmEncoding *Out = Encoder->Out;
    int64_t Constant = Value->Symbol == 0 && Kind != BOXASM_FIXUP_RELATIVE &&
                       Kind != BOXASM_FIXUP_BRANCH ? Value->Constant : 0;

    if (Value->Symbol != 0 || Kind == BOXASM_FIXUP_RELATIVE || Kind == BOXASM_FIXUP_BRANCH) {
        if (Out->FixupCount == sizeof(Out->Fixups) / sizeof(Out->Fixups[0]))
            return -ERR_INVALID_STATE;
        Out->Fixups[Out->FixupCount++] = (BoxAsmFixup){
            Kind, Out->Length, Width, TakesAddress, 0, *Value
        };
    }
    return BoxAsmPut(Encoder, &Constant, Width);
}

static void BoxAsmNoteRegister(BoxAsmEncoder *Encoder, const BoxAsmRegister *Register)
{
    if (Register->Class == BOXASM_REGISTER_HIGH8)
        Encoder->HasHigh8 = 1;
    if (Register->NeedsRex)
        Encoder->ForceRex = 1;
}

/* ModRM, SIB and displacement for an r/m operand; REX.X and REX.B noted. */
static void BoxAsmPutAddress(BoxAsmEncoder *Encoder, uint8_t RegField,
                             const BoxAsmOperand *Operand, uint8_t *Bytes, uint8_t *Length,
                             BoxAsmValue *Displacement, uint8_t *DisplacementKind,
                             uint8_t *DisplacementWidth)
{
    const BoxAsmValue *Value = &Operand->Value;
    uint8_t HasBase = Operand->Base.Class == BOXASM_REGISTER_GENERAL;
    uint8_t HasIndex = Operand->Index.Class == BOXASM_REGISTER_GENERAL;
    uint8_t Base = Operand->Base.Number;
    uint8_t Scale = Operand->Scale == 8 ? 3 : Operand->Scale == 4 ? 2 : Operand->Scale == 2 ? 1 : 0;
    uint8_t Mod;

    *Displacement = *Value;
    *DisplacementWidth = 0;
    *DisplacementKind = BOXASM_FIXUP_ABSOLUTE_SIGNED;
    if (Operand->Kind == BOXASM_OPERAND_REGISTER) {
        BoxAsmNoteRegister(Encoder, &Operand->Register);
        if (Operand->Register.Number >= 8)
            Encoder->Rex |= 0x41;
        Bytes[(*Length)++] = (uint8_t)(0xC0 | ((RegField & 7) << 3) | (Operand->Register.Number & 7));
        return;
    }
    if (HasIndex && Operand->Index.Number >= 8)
        Encoder->Rex |= 0x42;
    if (HasBase && Base >= 8)
        Encoder->Rex |= 0x41;
    /* RIP-relative: a symbol is reached from the end of the instruction; a
     * bare number after rip is the displacement itself. */
    if (Operand->Base.Class == BOXASM_REGISTER_RIP ||
        (!HasBase && !HasIndex && Value->Symbol != 0 && !Operand->Absolute)) {
        Bytes[(*Length)++] = (uint8_t)(((RegField & 7) << 3) | 5);
        if (Value->Symbol != 0)
            *DisplacementKind = BOXASM_FIXUP_RELATIVE;
        *DisplacementWidth = 4;
        return;
    }
    if (!HasBase) {
        Bytes[(*Length)++] = (uint8_t)(((RegField & 7) << 3) | 4);
        Bytes[(*Length)++] = (uint8_t)((Scale << 6) | ((HasIndex ? Operand->Index.Number : 4) & 7) << 3 | 5);
        *DisplacementWidth = 4;
        return;
    }
    if (Value->Symbol != 0 || Value->Constant < -128 || Value->Constant > 127) {
        Mod = 2;
        *DisplacementWidth = 4;
    } else if (Value->Constant != 0 || (Base & 7) == 5) {
        Mod = 1;
        *DisplacementWidth = 1;
    } else {
        Mod = 0;
    }
    if (HasIndex || (Base & 7) == 4) {
        Bytes[(*Length)++] = (uint8_t)((Mod << 6) | ((RegField & 7) << 3) | 4);
        Bytes[(*Length)++] = (uint8_t)((Scale << 6) |
                                       ((HasIndex ? Operand->Index.Number : 4) & 7) << 3 |
                                       (Base & 7));
    } else {
        Bytes[(*Length)++] = (uint8_t)((Mod << 6) | ((RegField & 7) << 3) | (Base & 7));
    }
}

static uint8_t BoxAsmImmediateWidth(const BoxAsmEncoder *Encoder, uint8_t Class, uint8_t Short)
{
    switch (Class) {
    case BOXASM_CLASS_IMM:     return Encoder->OperationSize == 8 ? 4 : Encoder->OperationSize;
    case BOXASM_CLASS_IMM_U32:
    case BOXASM_CLASS_IMM_S32: return 4;
    case BOXASM_CLASS_IMM8S:
    case BOXASM_CLASS_IMM8:    return 1;
    case BOXASM_CLASS_IMM16:   return 2;
    case BOXASM_CLASS_IMM64:   return 8;
    case BOXASM_CLASS_REL:     return Short ? 1 : 4;
    default:                   return 0;
    }
}

static uint8_t BoxAsmImmediateKind(const BoxAsmEncoder *Encoder, uint8_t Class, uint8_t Width)
{
    if (Class == BOXASM_CLASS_REL)
        return BOXASM_FIXUP_BRANCH;
    if (Width == 4 && Encoder->OperationSize == 8 && Class == BOXASM_CLASS_IMM)
        return BOXASM_FIXUP_ABSOLUTE_SIGNED;
    return BOXASM_FIXUP_ABSOLUTE;
}

static int BoxAsmIsRmClass(uint8_t Class)
{
    return Class == BOXASM_CLASS_RM || Class == BOXASM_CLASS_MEM ||
           Class == BOXASM_CLASS_ADDRESS || Class == BOXASM_CLASS_RM8 ||
           Class == BOXASM_CLASS_RM16 || Class == BOXASM_CLASS_RM32 ||
           Class == BOXASM_CLASS_RM64 ||
           (Class >= BOXASM_CLASS_XMM_R && Class <= BOXASM_CLASS_R32_M16) ||
           Class == BOXASM_CLASS_YMM_R || Class == BOXASM_CLASS_YMM_M256 ||
           Class == BOXASM_CLASS_YMM_U256 || Class == BOXASM_CLASS_U256 ||
           Class == BOXASM_CLASS_M256;
}

/* The boundary the memory an r/m of this class names must sit on, 0 for none:
 * 16 bytes for a packed SSE operand, but for the instructions made to read it
 * anywhere; of AVX only the moves that name an aligned width, 16 or 32. */
static uint8_t BoxAsmClassAligned(uint8_t Class)
{
    switch (Class) {
    case BOXASM_CLASS_XMM_M128:
    case BOXASM_CLASS_M128:     return 16;
    case BOXASM_CLASS_YMM_M256:
    case BOXASM_CLASS_M256:     return 32;
    default:                    return 0;
    }
}

static int BoxAsmIsImmediateClass(uint8_t Class)
{
    return Class >= BOXASM_CLASS_IMM && Class <= BOXASM_CLASS_REL;
}

/* The opcode with its condition code and +r register added, and the REX
 * bits the operation size and the +r register ask for. */
static void BoxAsmPrepareOpcode(BoxAsmEncoder *Encoder, uint8_t Size, uint8_t *Opcode)
{
    const BoxAsmForm *Form = Encoder->Form;
    uint8_t Last = (uint8_t)(Form->OpcodeLength - 1u);
    uint8_t Index;

    memcpy(Opcode, Form->Opcode, Form->OpcodeLength);
    if (Form->Flags & BOXASM_FORM_CONDITION)
        Opcode[Last] = (uint8_t)(Opcode[Last] + Encoder->Statement->Condition);
    if ((Form->Flags & BOXASM_FORM_REX_W) ||
        (Size == 8 && !(Form->Flags & BOXASM_FORM_DEFAULT64) && Form->Sizes != 0))
        Encoder->Rex |= 0x48;
    for (Index = 0; Index < Encoder->Count; Index++) {
        const BoxAsmOperand *Operand = &Encoder->Operands[Index];

        if (Operand->Kind == BOXASM_OPERAND_REGISTER)
            BoxAsmNoteRegister(Encoder, &Operand->Register);
        if (Form->Class[Index] == BOXASM_CLASS_REG && (Form->Flags & BOXASM_FORM_PLUS_REG)) {
            Opcode[Last] = (uint8_t)(Opcode[Last] + (Operand->Register.Number & 7));
            if (Operand->Register.Number >= 8)
                Encoder->Rex |= 0x41;
        }
    }
}

/* The operand ModRM.rm describes: the form's r/m operand, or — in a /digit
 * form that names a plain register, as mov r64, imm32 — that register. */
static uint8_t BoxAsmRmIndex(const BoxAsmEncoder *Encoder)
{
    uint8_t Index;

    for (Index = 0; Index < Encoder->Count; Index++)
        if (BoxAsmIsRmClass(Encoder->Form->Class[Index]))
            return Index;
    for (Index = 0; Index < Encoder->Count; Index++)
        if (Encoder->Form->Class[Index] == BOXASM_CLASS_REG &&
            Encoder->Form->Digit != BOXASM_DIGIT_REG)
            return Index;
    return 0;
}

static uint8_t BoxAsmRegField(BoxAsmEncoder *Encoder)
{
    uint8_t Index;

    if (Encoder->Form->Digit != BOXASM_DIGIT_REG)
        return Encoder->Form->Digit;
    for (Index = 0; Index < Encoder->Count; Index++) {
        if (Encoder->Form->Class[Index] == BOXASM_CLASS_REG ||
            Encoder->Form->Class[Index] == BOXASM_CLASS_XMM ||
            Encoder->Form->Class[Index] == BOXASM_CLASS_YMM) {
            uint8_t Number = Encoder->Operands[Index].Register.Number;
            if (Number >= 8)
                Encoder->Rex |= 0x44;
            return Number;
        }
    }
    return 0;
}

static int BoxAsmPutImmediates(BoxAsmEncoder *Encoder, uint8_t Short)
{
    const BoxAsmForm *Form = Encoder->Form;
    uint8_t Index;

    for (Index = 0; Index < Encoder->Count; Index++) {
        uint8_t Class = Form->Class[Index];
        uint8_t Width = BoxAsmImmediateWidth(Encoder, Class, Short);
        int Rc;

        if (Class == BOXASM_CLASS_XMM_IS4 || Class == BOXASM_CLASS_YMM_IS4) {
            uint8_t Selector = (uint8_t)(Encoder->Operands[Index].Register.Number << 4);

            if ((Rc = BoxAsmPut(Encoder, &Selector, 1)) != 0)
                return Rc;
            continue;
        }
        if (!BoxAsmIsImmediateClass(Class) || Width == 0)
            continue;
        Rc = BoxAsmPutField(Encoder, &Encoder->Operands[Index].Value, Width,
                            BoxAsmImmediateKind(Encoder, Class, Width),
                            Class != BOXASM_CLASS_REL);
        if (Rc != 0)
            return Rc;
    }
    return 0;
}

/* The prefix that goes after the operand-size prefix and before REX, as
 * NASM writes it: the source's rep, repe or repne, or the form's own F3. */
static uint8_t BoxAsmRepeatByte(uint8_t Prefix, uint8_t FormPrefix)
{
    switch (Prefix) {
    case BOXASM_PREFIX_REP:
    case BOXASM_PREFIX_REPE:  return 0xF3;
    case BOXASM_PREFIX_REPNE: return 0xF2;
    default:                  return FormPrefix;
    }
}

/* The legacy prefixes and REX: lock, the operand-size prefix, the repeat or
 * mandatory prefix, then REX. */
static int BoxAsmPutLegacyPrefixes(BoxAsmEncoder *Encoder, uint8_t Size)
{
    const BoxAsmForm *Form = Encoder->Form;
    uint8_t Repeat = BoxAsmRepeatByte(Encoder->Statement->Prefix, Form->Prefix);
    int Rc;

    if (Encoder->ForceRex)
        Encoder->Rex |= 0x40;
    if (Encoder->Rex != 0 && Encoder->HasHigh8)
        return BoxAsmFail(Encoder->Asm, &Encoder->Statement->Where,
                          "ah, bh, ch and dh cannot be used in an instruction that needs REX", NULL);
    if (Encoder->Statement->Prefix == BOXASM_PREFIX_LOCK &&
        (Rc = BoxAsmPut(Encoder, "\xF0", 1)) != 0)
        return Rc;
    if (((Size == 2 && Form->Sizes != 0) || (Form->Flags & BOXASM_FORM_DATA16)) &&
        (Rc = BoxAsmPut(Encoder, "\x66", 1)) != 0)
        return Rc;
    if (Repeat != 0 && (Rc = BoxAsmPut(Encoder, &Repeat, 1)) != 0)
        return Rc;
    if (Encoder->Rex != 0)
        return BoxAsmPut(Encoder, &Encoder->Rex, 1);
    return 0;
}

/* The register VEX.vvvv holds: that of the operand of a _V class, 0 when the
 * form has none (the field is then 1111, as the inverted 0 is). */
static uint8_t BoxAsmVexSource(const BoxAsmEncoder *Encoder)
{
    uint8_t Index;

    for (Index = 0; Index < Encoder->Count; Index++)
        if (Encoder->Form->Class[Index] == BOXASM_CLASS_XMM_V ||
            Encoder->Form->Class[Index] == BOXASM_CLASS_YMM_V)
            return Encoder->Operands[Index].Register.Number;
    return 0;
}

static uint8_t BoxAsmVexPrefixBits(uint8_t Prefix)
{
    switch (Prefix) {
    case 0x66: return 1;
    case 0xF3: return 2;
    case 0xF2: return 3;
    default:   return 0;
    }
}

/* The VEX prefix, which stands for the legacy prefixes, REX and the map's
 * escape bytes: the two bytes of C5 when only R is needed of REX's bits and
 * the map is 0F, else the three of C4. R, X, B and vvvv are written inverted.
 * REX's bits are collected as for a legacy form, and are read here. */
static int BoxAsmPutVex(BoxAsmEncoder *Encoder)
{
    const BoxAsmForm *Form = Encoder->Form;
    uint8_t Map = (uint8_t)((Form->Flags & BOXASM_FORM_VEX_MAP) >> BOXASM_FORM_VEX_SHIFT);
    uint8_t Wide = ((Encoder->Rex & 0x08) || (Form->Flags & BOXASM_FORM_VEX_W1)) ? 1u : 0u;
    uint8_t Long = (Form->Flags & BOXASM_FORM_VEX_L256) ? 1u : 0u;
    uint8_t Source = (uint8_t)(~BoxAsmVexSource(Encoder) & 0x0F);
    uint8_t Reg = (Encoder->Rex & 0x04) ? 0u : 1u;
    uint8_t Index = (Encoder->Rex & 0x02) ? 0u : 1u;
    uint8_t Base = (Encoder->Rex & 0x01) ? 0u : 1u;
    uint8_t Last = (uint8_t)((Source << 3) | (Long << 2) | BoxAsmVexPrefixBits(Form->Prefix));
    uint8_t Bytes[3];

    if (Map == 1 && !Wide && Index && Base) {
        Bytes[0] = 0xC5;
        Bytes[1] = (uint8_t)((Reg << 7) | Last);
        return BoxAsmPut(Encoder, Bytes, 2);
    }
    Bytes[0] = 0xC4;
    Bytes[1] = (uint8_t)((Reg << 7) | (Index << 6) | (Base << 5) | Map);
    Bytes[2] = (uint8_t)((Wide << 7) | Last);
    return BoxAsmPut(Encoder, Bytes, 3);
}

/* Prefixes, REX, opcode, ModRM, SIB, displacement and immediate, in the order
 * the processor reads them; a VEX form's prefix is the one before its opcode. */
static int BoxAsmEmitForm(BoxAsmEncoder *Encoder, uint8_t Short)
{
    const BoxAsmForm *Form = Encoder->Form;
    uint8_t Address[6];
    uint8_t AddressLength = 0;
    uint8_t Opcode[4];
    uint8_t Size = (Form->Flags & BOXASM_FORM_AS32) ? 4 : Encoder->OperationSize;
    uint8_t RmIndex = BoxAsmRmIndex(Encoder);
    BoxAsmValue Displacement = { 0, 0 };
    uint8_t DisplacementKind = 0;
    uint8_t DisplacementWidth = 0;
    int Rc;

    BoxAsmPrepareOpcode(Encoder, Size, Opcode);
    if (Form->Digit != BOXASM_DIGIT_NONE)
        BoxAsmPutAddress(Encoder, BoxAsmRegField(Encoder), &Encoder->Operands[RmIndex],
                         Address, &AddressLength, &Displacement, &DisplacementKind,
                         &DisplacementWidth);
    Rc = (Form->Flags & BOXASM_FORM_VEX_MAP) ? BoxAsmPutVex(Encoder) : BoxAsmPutLegacyPrefixes(Encoder, Size);
    if (Rc != 0)
        return Rc;
    if ((Rc = BoxAsmPut(Encoder, Opcode, Form->OpcodeLength)) != 0 ||
        (Rc = BoxAsmPut(Encoder, Address, AddressLength)) != 0)
        return Rc;
    if (DisplacementWidth != 0) {
        uint8_t Before = Encoder->Out->FixupCount;

        if ((Rc = BoxAsmPutField(Encoder, &Displacement, DisplacementWidth, DisplacementKind,
                                 Form->Class[RmIndex] == BOXASM_CLASS_ADDRESS)) != 0)
            return Rc;
        if (Encoder->Out->FixupCount != Before)
            Encoder->Out->Fixups[Before].Aligned = BoxAsmClassAligned(Form->Class[RmIndex]);
    }
    if ((Rc = BoxAsmPutImmediates(Encoder, Short)) != 0)
        return Rc;
    if (Form->Flags & BOXASM_FORM_PREDICATE)
        return BoxAsmPut(Encoder, &Encoder->Statement->Condition, 1);
    return 0;
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
 * which instruction takes ymm, AVX's before AVX2 have a 256-bit form only
 * where the processor has one. */
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
        BoxAsmMessageText(Message, " has no 256-bit form before AVX2");
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

int BoxAsmEncode(BoxAsmAssembler *Asm, const BoxAsmStatement *Statement, uint8_t Short,
                 BoxAsmEncoding *Out)
{
    const BoxAsmInstruction *Instruction = Statement->Instruction;
    BoxAsmEncoder Encoder;
    BoxAsmMismatch Why = BOXASM_MISMATCH_OPERANDS;
    uint8_t Sizes = 0;
    uint8_t Index;

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
    for (Index = 0; Index < Instruction->FormCount; Index++) {
        if (BoxAsmFormMatches(&Encoder, &Instruction->Forms[Index], Short, &Why)) {
            int Rc;

            Encoder.Form = &Instruction->Forms[Index];
            if ((Rc = BoxAsmCheckPrefix(&Encoder)) != 0)
                return Rc;
            return BoxAsmEmitForm(&Encoder, Short);
        }
    }
    return BoxAsmNoForm(&Encoder, Why);
}
