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
    BOXASM_CLASS_RM64,
    BOXASM_CLASS_XMM,        /* an xmm register, in ModRM.reg */
    BOXASM_CLASS_XMM0,       /* exactly xmm0, named by no field (the mask of blendvps) */
    BOXASM_CLASS_XMM_R,      /* an xmm register, in ModRM.rm */
    BOXASM_CLASS_XMM_M32,    /* an xmm register or 4 bytes of memory, in ModRM.rm */
    BOXASM_CLASS_XMM_M16,    /* an xmm register or 2 bytes of memory (pmovzxbq) */
    BOXASM_CLASS_XMM_M64,    /* an xmm register or 8 bytes of memory */
    BOXASM_CLASS_XMM_M128,   /* an xmm register or 16 bytes of memory on a 16-byte boundary */
    BOXASM_CLASS_XMM_U128,   /* an xmm register or 16 bytes of memory anywhere (movups) */
    BOXASM_CLASS_U128,       /* 16 bytes of memory anywhere (lddqu) */
    BOXASM_CLASS_M8,         /* 1 byte of memory */
    BOXASM_CLASS_M16,        /* 2 bytes of memory */
    BOXASM_CLASS_M32,        /* 4 bytes of memory */
    BOXASM_CLASS_M64,        /* 8 bytes of memory */
    BOXASM_CLASS_M128,       /* 16 bytes of memory on a 16-byte boundary */
    BOXASM_CLASS_REG_R,      /* a general register of the operation size, in ModRM.rm (pextrb) */
    BOXASM_CLASS_R32_M16,    /* a 32- or 64-bit register, or 2 bytes of memory (pinsrw) */
    /* AVX: where a vector register goes is part of the class. VEX.vvvv holds
     * a register of its own, and imm8[7:4] the register a blend selects by. */
    BOXASM_CLASS_XMM_V,      /* an xmm register, in VEX.vvvv */
    BOXASM_CLASS_XMM_IS4,    /* an xmm register, in imm8[7:4] (vblendvps) */
    BOXASM_CLASS_YMM,        /* a ymm register, in ModRM.reg */
    BOXASM_CLASS_YMM_R,      /* a ymm register, in ModRM.rm */
    BOXASM_CLASS_YMM_V,      /* a ymm register, in VEX.vvvv */
    BOXASM_CLASS_YMM_IS4,    /* a ymm register, in imm8[7:4] */
    BOXASM_CLASS_YMM_M256,   /* a ymm register or 32 bytes of memory on a 32-byte boundary */
    BOXASM_CLASS_YMM_U256,   /* a ymm register or 32 bytes of memory anywhere */
    BOXASM_CLASS_U256,       /* 32 bytes of memory anywhere (vlddqu, vmaskmovps) */
    BOXASM_CLASS_M256,       /* 32 bytes of memory on a 32-byte boundary (vmovntps) */
    /* The address of a gather, in ModRM.rm with a SIB byte whose index is a
     * vector register (VSIB): the register's width, then the bytes of one
     * element the memory is said to hold. */
    BOXASM_CLASS_VSIB_X32,   /* indexed by an xmm register, 4 bytes an element (vgatherdps) */
    BOXASM_CLASS_VSIB_X64,   /* indexed by an xmm register, 8 bytes an element (vgatherdpd) */
    BOXASM_CLASS_VSIB_Y32,   /* indexed by a ymm register, 4 bytes an element */
    BOXASM_CLASS_VSIB_Y64,   /* indexed by a ymm register, 8 bytes an element */
    BOXASM_CLASS_XMM_M8      /* an xmm register or 1 byte of memory, in ModRM.rm (vpbroadcastb) */
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
#define BOXASM_FORM_DATA16     0x100u /* a 66 prefix whatever the size (crc32 r32, r/m16) */
#define BOXASM_FORM_DUPLICATE  0x200u /* the second operand may be left out, and is then the
                                         first again: vaddps xmm1, xmm2 is vaddps xmm1, xmm1, xmm2 */

/* The VEX prefix a form is written with, in its Flags; none of these bits is
 * a form of the legacy encodings. The opcode bytes of a VEX form are those
 * after the map, and its Prefix is VEX.pp: 66, F3 or F2 pick it, and none is
 * pp 0. A field the instruction ignores (VEX.WIG, VEX.LIG) is written 0, as
 * NASM writes it. VEX.R, X and B come from the operands as REX's do; VEX.vvvv
 * from the operand of a _V class, 1111 when there is none. */
#define BOXASM_FORM_VEX_MAP    0xC00u   /* the two bits of the map: 1, 2, 3, or 0 for no VEX */
#define BOXASM_FORM_VEX_SHIFT  10u
#define BOXASM_FORM_VEX_0F     0x400u   /* the escape 0F */
#define BOXASM_FORM_VEX_0F38   0x800u   /* 0F 38 */
#define BOXASM_FORM_VEX_0F3A   0xC00u   /* 0F 3A */
#define BOXASM_FORM_VEX_L256   0x1000u  /* VEX.L: 256 bits; left clear, 128 bits or ignored */
#define BOXASM_FORM_VEX_W1     0x2000u  /* VEX.W: 1; left clear, 0 or ignored */

typedef struct {
    uint8_t Class[4];
    uint8_t Sizes;
    uint8_t Opcode[4];
    uint8_t OpcodeLength;
    uint8_t Digit;
    uint16_t Flags;
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
