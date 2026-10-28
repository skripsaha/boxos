#ifndef BOXASM_MACRO_H
#define BOXASM_MACRO_H

#include "boxasm_asm.h"

/* Where a line of the expanded source came from: the source line, and the
 * macro call it was written in (0: none). */
typedef struct {
    uint32_t Line;
    uint32_t Frame;
} BoxAsmOrigin;

/* One macro call: the macro's name (an offset into Names), the source line
 * the call stands on, and the call that line was itself written in. */
typedef struct {
    uint32_t Name;
    uint32_t Line;
    uint32_t Parent;
} BoxAsmFrame;

/* The source as the assembler reads it once the preprocessor is done: its
 * text, one line per output line, and where each line came from. */
struct BoxAsmExpansion {
    BoxAsmBuffer  Text;
    BoxAsmOrigin *Lines;
    size_t        LineCount;
    size_t        LineCapacity;
    BoxAsmFrame  *Frames;          /* Frames[0] stands for no call */
    size_t        FrameCount;
    size_t        FrameCapacity;
    BoxAsmBuffer  Names;
};

/* The source with its %define, %macro, %if, %rep and the rest carried out,
 * as NASM's preprocessor carries them out. A refusal is said through Asm's
 * error, with the line of the source and the calls it was reached through. */
int BoxAsmPreprocess(BoxAsmAssembler *Asm, const char *Source, size_t Size,
                     BoxAsmExpansion *Out);

void BoxAsmExpansionRelease(BoxAsmExpansion *Expansion);

/* The source line and the call a line of the expanded text stands for. */
void BoxAsmExpansionPlace(const BoxAsmExpansion *Expansion, uint32_t Line,
                          uint32_t *Source, uint32_t *Frame);

#endif
