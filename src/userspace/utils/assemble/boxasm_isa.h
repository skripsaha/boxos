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
    BOXASM_CLASS_RM32,
    BOXASM_CLASS_XMM,        /* an xmm register, in ModRM.reg */
    BOXASM_CLASS_XMM_R,      /* an xmm register, in ModRM.rm */
    BOXASM_CLASS_XMM_M32,    /* an xmm register or 4 bytes of memory, in ModRM.rm */
    BOXASM_CLASS_XMM_M64,    /* an xmm register or 8 bytes of memory */
    BOXASM_CLASS_XMM_M128,   /* an xmm register or 16 bytes of memory on a 16-byte boundary */
    BOXASM_CLASS_XMM_U128,   /* an xmm register or 16 bytes of memory anywhere (movups) */
    BOXASM_CLASS_M64,        /* 8 bytes of memory */
    BOXASM_CLASS_M128,       /* 16 bytes of memory on a 16-byte boundary */
    BOXASM_CLASS_R32_M16     /* a 32- or 64-bit register, or 2 bytes of memory (pinsrw) */
} BoxAsmOperandClass;

#define BOXASM_SIZE_8   0x1u
#define BOXASM_SIZE_16  0x2u
#define BOXASM_SIZE_32  0x4u
#define BOXASM_SIZE_64  0x8u
#define BOXASM_SIZE_128 0x10u
#define BOXASM_SIZE_WIDE (BOXASM_SIZE_16 | BOXASM_SIZE_32 | BOXASM_SIZE_64)

#define BOXASM_DIGIT_NONE 0xFEu  /* no ModRM byte */
#define BOXASM_DIGIT_REG  0xFFu  /* ModRM.reg holds the REG operand */

#define BOXASM_FORM_PLUS_REG   0x01u  /* the REG operand is added to the last opcode byte */
#define BOXASM_FORM_DEFAULT64  0x02u  /* 64-bit without REX.W */
#define BOXASM_FORM_SHORT      0x04u  /* rel8 branch */
#define BOXASM_FORM_CONDITION  0x08u  /* the condition code is added to the last opcode byte */
#define BOXASM_FORM_AS32       0x10u  /* encoded as the 32-bit operation (mov r64, u32) */
#define BOXASM_FORM_NOT_EAX    0x20u  /* not with eax as the +r register (xchg eax, eax) */
#define BOXASM_FORM_REX_W      0x40u  /* REX.W whatever the size (cmpxchg16b) */
#define BOXASM_FORM_PREDICATE  0x80u  /* the statement's condition follows as an imm8 (cmpltps) */

typedef struct {
    uint8_t Class[3];
    uint8_t Sizes;
    uint8_t Opcode[4];
    uint8_t OpcodeLength;
    uint8_t Digit;
    uint8_t Flags;
    uint8_t Prefix;      /* a prefix that is part of the opcode (F3 of popcnt), 0 none */
} BoxAsmForm;

typedef enum {
    BOXASM_KIND_PLAIN = 0,
    BOXASM_KIND_JUMP,          /* jmp and jcc: rel8 when it reaches */
    BOXASM_KIND_CALL,
    BOXASM_KIND_SHORT_BRANCH   /* loop and jrcxz: rel8 only */
} BoxAsmInstructionKind;

/* The prefixes of the source an instruction takes. */
#define BOXASM_TAKES_LOCK  0x1u   /* lock, with a memory destination */
#define BOXASM_TAKES_REP   0x2u   /* rep: movs, stos, lods */
#define BOXASM_TAKES_REPE  0x4u   /* repe and repne: cmps, scas */
#define BOXASM_TAKES_BOTH  0x8u   /* xchg: it writes both operands, so either is a destination */

struct BoxAsmInstruction {
    const char       *Name;
    const BoxAsmForm *Forms;
    uint8_t           FormCount;
    uint8_t           Kind;
    uint8_t           Conditional;
    uint8_t           SwapImmediate;   /* imul r, imm means imul r, r, imm */
    uint8_t           Takes;           /* BOXASM_TAKES_* */
};

typedef struct BoxAsmInstruction BoxAsmInstruction;

/* The instruction a mnemonic names (any case); for jcc, setcc and cmovcc the
 * condition code goes to *Condition. NULL when there is none. */
const BoxAsmInstruction *BoxAsmFindInstruction(const char *Name, size_t Length,
                                               uint8_t *Condition);

/* The prefix a word names (lock, rep, repe, repz, repne, repnz, any case), as
 * a BOXASM_PREFIX_*; BOXASM_PREFIX_NONE when it names none. */
uint8_t BoxAsmFindPrefix(const char *Name, size_t Length);

/* The bytes of one instruction whose operands carry this pass's values. Short
 * asks a jump for its rel8 form. A field that depends on a symbol is left zero
 * and described in Out->Fixups. */
int BoxAsmEncode(BoxAsmAssembler *Asm, const BoxAsmStatement *Statement,
                 uint8_t Short, BoxAsmEncoding *Out);

#endif
