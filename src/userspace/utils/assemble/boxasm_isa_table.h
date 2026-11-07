#ifndef BOXASM_ISA_TABLE_H
#define BOXASM_ISA_TABLE_H

#include "boxasm_isa.h"

/* What the tables of forms are written with: boxasm_isa_table.c for the legacy
 * encodings and MMX, boxasm_isa_vex.c for VEX, boxasm_isa_x87.c for x87. */

#define C(A, B, D) { BOXASM_CLASS_##A, BOXASM_CLASS_##B, BOXASM_CLASS_##D }
#define C4(A, B, D, E) { BOXASM_CLASS_##A, BOXASM_CLASS_##B, BOXASM_CLASS_##D, BOXASM_CLASS_##E }
#define S8   BOXASM_SIZE_8
#define S16  BOXASM_SIZE_16
#define S32  BOXASM_SIZE_32
#define S64  BOXASM_SIZE_64
#define S128 BOXASM_SIZE_128
#define SW   BOXASM_SIZE_WIDE
#define R    BOXASM_DIGIT_REG
#define NO   BOXASM_DIGIT_NONE

#define INSTRUCTION(Name, Forms, Kind, Conditional, Swap, Takes)          \
    { Name, Forms, sizeof(Forms) / sizeof(Forms[0]), Kind, Conditional, Swap, Takes }
#define PLAIN(Name, Forms) INSTRUCTION(Name, Forms, BOXASM_KIND_PLAIN, 0, 0, 0)

/* Name, lowered, equals Word. */
int BoxAsmNameEquals(const char *Name, size_t Length, const char *Word);

/* boxasm_isa_vex.c: the instruction a VEX mnemonic names, vcmpltps and its
 * kin included with their predicate in *Condition; NULL when there is none. */
const BoxAsmInstruction *BoxAsmFindVexInstruction(const char *Name, size_t Length,
                                                  uint8_t *Condition);

/* boxasm_isa_x87.c: the x87 instruction a mnemonic names; NULL when none. */
const BoxAsmInstruction *BoxAsmFindX87Instruction(const char *Name, size_t Length);

#endif
