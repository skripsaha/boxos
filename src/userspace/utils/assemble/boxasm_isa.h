#ifndef BOXASM_ISA_H
#define BOXASM_ISA_H

#include "boxasm_asm.h"

/* What an operand of a form must be. Sizes come from the form's Sizes mask
 * unless the class names its own. */
typedef enum {
    BOXASM_CLASS_NONE = 0,
    BOXASM_CLASS_REG,        /* a general register of the operation size */
    BOXASM_CLASS_RM,         /* a register or memory of the operation size */
    BOXASM_CLASS_MEM,        /* memory of the operation size */
    BOXASM_CLASS_ADDRESS,    /* memory of any size, its address taken (lea) */
    BOXASM_CLASS_ACC,        /* al, ax, eax or rax */
    BOXASM_CLASS_CL,
    BOXASM_CLASS_ONE,        /* the constant 1 */
    BOXASM_CLASS_IMM,        /* immediate of the operation size; 64 sign-extends 32 */
    BOXASM_CLASS_IMM_U32,    /* constant in 0..0xFFFFFFFF */
    BOXASM_CLASS_IMM_S32,    /* constant in -2^31..2^31-1 */
    BOXASM_CLASS_IMM8S,      /* constant in -128..127, sign-extended */
    BOXASM_CLASS_IMM8,       /* constant in -128..255 */
    BOXASM_CLASS_IMM16,      /* constant in 0..65535 */
    BOXASM_CLASS_IMM64,      /* any immediate, written in eight bytes */
    BOXASM_CLASS_REL,        /* a branch target */
    BOXASM_CLASS_RM8,        /* a register or memory of exactly 1 byte */
    BOXASM_CLASS_RM16,
    BOXASM_CLASS_RM32
} BoxAsmOperandClass;

#define BOXASM_SIZE_8   0x1u
#define BOXASM_SIZE_16  0x2u
#define BOXASM_SIZE_32  0x4u
#define BOXASM_SIZE_64  0x8u
#define BOXASM_SIZE_WIDE (BOXASM_SIZE_16 | BOXASM_SIZE_32 | BOXASM_SIZE_64)

#define BOXASM_DIGIT_NONE 0xFEu  /* no ModRM byte */
#define BOXASM_DIGIT_REG  0xFFu  /* ModRM.reg holds the REG operand */

#define BOXASM_FORM_PLUS_REG   0x01u  /* the REG operand is added to the last opcode byte */
#define BOXASM_FORM_DEFAULT64  0x02u  /* 64-bit without REX.W */
#define BOXASM_FORM_SHORT      0x04u  /* rel8 branch */
#define BOXASM_FORM_CONDITION  0x08u  /* the condition code is added to the last opcode byte */
#define BOXASM_FORM_AS32       0x10u  /* encoded as the 32-bit operation (mov r64, u32) */
#define BOXASM_FORM_NOT_EAX    0x20u  /* not with eax as the +r register (xchg eax, eax) */

typedef struct {
    uint8_t Class[3];
    uint8_t Sizes;
    uint8_t Opcode[4];
    uint8_t OpcodeLength;
    uint8_t Digit;
    uint8_t Flags;
} BoxAsmForm;

typedef enum {
    BOXASM_KIND_PLAIN = 0,
    BOXASM_KIND_JUMP,          /* jmp and jcc: rel8 when it reaches */
    BOXASM_KIND_CALL
} BoxAsmInstructionKind;

struct BoxAsmInstruction {
    const char       *Name;
    const BoxAsmForm *Forms;
    uint8_t           FormCount;
    uint8_t           Kind;
    uint8_t           Conditional;
    uint8_t           SwapImmediate;   /* imul r, imm means imul r, r, imm */
};

typedef struct BoxAsmInstruction BoxAsmInstruction;

/* The instruction a mnemonic names (any case); for jcc, setcc and cmovcc the
 * condition code goes to *Condition. NULL when there is none. */
const BoxAsmInstruction *BoxAsmFindInstruction(const char *Name, size_t Length,
                                               uint8_t *Condition);

/* The bytes of one instruction whose operands carry this pass's values. Short
 * asks a jump for its rel8 form. A field that depends on a symbol is left zero
 * and described in Out->Fixups. */
int BoxAsmEncode(BoxAsmAssembler *Asm, const BoxAsmStatement *Statement,
                 uint8_t Short, BoxAsmEncoding *Out);

#endif
