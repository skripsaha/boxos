#ifndef BOXASM_ISA_INTERNAL_H
#define BOXASM_ISA_INTERNAL_H

#include "boxasm_isa.h"

/* What boxasm_isa.c, which chooses an instruction's form, and
 * boxasm_isa_emit.c, which writes its bytes, share. */

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

/* Whether the class is the address of a gather (VSIB). */
int BoxAsmIsGatherClass(uint8_t Class);

/* The bytes of the form the encoder has chosen, its operands as that form
 * reads them: prefixes, REX or VEX, opcode, ModRM, SIB, displacement and
 * immediates. */
int BoxAsmEmitForm(BoxAsmEncoder *Encoder, uint8_t Short);

#endif
