#ifndef BOXASM_MACRO_H
#define BOXASM_MACRO_H

#include "boxasm_asm.h"
#include "boxasm_source.h"

/* Where a line of the expanded source came from: the file it was written in
 * (an index of Files, 0 the source itself), its line there, and the macro
 * call or %include it was reached through (0: none). */
typedef struct {
    uint32_t File;
    uint32_t Line;
    uint32_t Frame;
} BoxAsmOrigin;

typedef enum {
    BOXASM_FRAME_MACRO = 0,    /* a macro called: Name is the macro's */
    BOXASM_FRAME_FILE          /* a file included: Name is the file's */
} BoxAsmFrameKind;

/* One macro call or %include: what it named (an offset into Names), the file
 * and line it stands on, and the call or %include that line was itself
 * reached through. */
typedef struct {
    uint32_t Name;
    uint32_t File;
    uint32_t Line;
    uint32_t Parent;
    uint8_t  Kind;
} BoxAsmFrame;

/* The source as the assembler reads it once the preprocessor is done: its
 * text, one line per output line, where each line came from, and the names
 * of the files read. */
struct BoxAsmExpansion {
    BoxAsmBuffer  Text;
    BoxAsmOrigin *Lines;
    size_t        LineCount;
    size_t        LineCapacity;
    BoxAsmFrame  *Frames;          /* Frames[0] stands for none */
    size_t        FrameCount;
    size_t        FrameCapacity;
    uint32_t     *Files;           /* each file's name, an offset into Names */
    size_t        FileCount;
    size_t        FileCapacity;
    BoxAsmBuffer  Names;
};

/* The source with its %define, %macro, %if, %rep, %include and the rest
 * carried out, as NASM's preprocessor carries them out, its %includes found
 * by name and tags on the source's volume. A refusal is said through Asm's
 * error, with the line of the file and the calls and %includes it was
 * reached through. */
int BoxAsmPreprocess(BoxAsmAssembler *Asm, const BoxAsmSource *Source, BoxAsmExpansion *Out);

void BoxAsmExpansionRelease(BoxAsmExpansion *Expansion);

/* The file, its line and the call a line of the expanded text stands for. */
void BoxAsmExpansionPlace(const BoxAsmExpansion *Expansion, uint32_t Line, uint32_t *File,
                          uint32_t *Source, uint32_t *Frame);

/* The name of file File of the expansion. */
const char *BoxAsmExpansionFile(const BoxAsmExpansion *Expansion, uint32_t File);

#endif
