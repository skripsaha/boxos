#include "boxasm_isa.h"

#include "box/error.h"
#include "box/string.h"

/* Why the last form tried did not match, to say the most useful thing when
 * none does. */
typedef enum {
    BOXASM_MISMATCH_OPERANDS = 0,
    BOXASM_MISMATCH_UNSIZED,
    BOXASM_MISMATCH_SIZE,
    BOXASM_MISMATCH_RANGE
} BoxAsmMismatch;

typedef struct {
    BoxAsmAssembler       *Asm;
    const BoxAsmStatement *Statement;
    const BoxAsmForm      *Form;
    BoxAsmOperand          Operands[3];
    uint8_t                Count;
    uint8_t                OperationSize;   /* bytes; 0 for a form without one */
    uint8_t                OnlySize;        /* the instruction's one size, 0 when it has several */
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

static int BoxAsmClassMatches(const BoxAsmOperand *Operand, uint8_t Class, uint8_t Size,
                              BoxAsmMismatch *Why)
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
    default:                   return 0;
    }
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

        if (Class != BOXASM_CLASS_REG && Class != BOXASM_CLASS_RM &&
            Class != BOXASM_CLASS_MEM && Class != BOXASM_CLASS_ACC &&
            Class != BOXASM_CLASS_IMM)
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

static int BoxAsmFormMatches(BoxAsmEncoder *Encoder, const BoxAsmForm *Form, uint8_t Short,
                             BoxAsmMismatch *Why)
{
    uint8_t Count = 0;
    uint8_t Index;

    while (Count < 3 && Form->Class[Count] != BOXASM_CLASS_NONE)
        Count++;
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
        if (!BoxAsmClassMatches(&Encoder->Operands[Index], Form->Class[Index],
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
            Kind, Out->Length, Width, TakesAddress, *Value
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
           Class == BOXASM_CLASS_RM16 || Class == BOXASM_CLASS_RM32;
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
    if (Size == 8 && !(Form->Flags & BOXASM_FORM_DEFAULT64) && Form->Sizes != 0)
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
        if (Encoder->Form->Class[Index] == BOXASM_CLASS_REG) {
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

/* Prefixes, REX, opcode, ModRM, SIB, displacement and immediate, in the order
 * the processor reads them. */
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
    if (Encoder->ForceRex)
        Encoder->Rex |= 0x40;
    if (Encoder->Rex != 0 && Encoder->HasHigh8)
        return BoxAsmFail(Encoder->Asm, &Encoder->Statement->Where,
                          "ah, bh, ch and dh cannot be used in an instruction that needs REX", NULL);
    if (Size == 2 && Form->Sizes != 0 && (Rc = BoxAsmPut(Encoder, "\x66", 1)) != 0)
        return Rc;
    if (Encoder->Rex != 0 && (Rc = BoxAsmPut(Encoder, &Encoder->Rex, 1)) != 0)
        return Rc;
    if ((Rc = BoxAsmPut(Encoder, Opcode, Form->OpcodeLength)) != 0 ||
        (Rc = BoxAsmPut(Encoder, Address, AddressLength)) != 0)
        return Rc;
    if (DisplacementWidth != 0 &&
        (Rc = BoxAsmPutField(Encoder, &Displacement, DisplacementWidth, DisplacementKind,
                             Form->Class[RmIndex] == BOXASM_CLASS_ADDRESS)) != 0)
        return Rc;
    return BoxAsmPutImmediates(Encoder, Short);
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
    case BOXASM_MISMATCH_RANGE: {
        uint8_t At;
        for (At = 0; At < Encoder->Count; At++)
            if (BoxAsmIsConstant(&Encoder->Operands[At]))
                BoxAsmMessageDecimal(&Message, Encoder->Operands[At].Value.Constant);
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
    Encoder.Count = Statement->OperandCount;
    memcpy(Encoder.Operands, Statement->Operands, sizeof(Encoder.Operands));
    if (Instruction->SwapImmediate && Encoder.Count == 2 &&
        Encoder.Operands[1].Kind == BOXASM_OPERAND_IMMEDIATE) {
        Encoder.Operands[2] = Encoder.Operands[1];
        Encoder.Operands[1] = Encoder.Operands[0];
        Encoder.Count = 3;
    }
    for (Index = 0; Index < Instruction->FormCount; Index++)
        Sizes |= Instruction->Forms[Index].Sizes;
    Encoder.OnlySize = BoxAsmSingleSize(Sizes);
    for (Index = 0; Index < Instruction->FormCount; Index++) {
        if (BoxAsmFormMatches(&Encoder, &Instruction->Forms[Index], Short, &Why)) {
            Encoder.Form = &Instruction->Forms[Index];
            return BoxAsmEmitForm(&Encoder, Short);
        }
    }
    return BoxAsmNoForm(&Encoder, Why);
}
