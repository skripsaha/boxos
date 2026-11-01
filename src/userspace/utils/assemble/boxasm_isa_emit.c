#include "boxasm_isa_internal.h"

#include "box/error.h"
#include "box/string.h"

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
    uint8_t HasIndex = Operand->Index.Class != BOXASM_REGISTER_NONE;
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
           Class == BOXASM_CLASS_M256 || Class == BOXASM_CLASS_XMM_M8 ||
           BoxAsmIsGatherClass(Class);
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
int BoxAsmEmitForm(BoxAsmEncoder *Encoder, uint8_t Short)
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
